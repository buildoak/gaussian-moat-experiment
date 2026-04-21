#ifndef GM_ROW_SIEVE_CUH
#define GM_ROW_SIEVE_CUH

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <vector>

#include <cuda_runtime.h>

#include "cornacchia.cuh"
#include "tile_kernel.cuh"

namespace gm {

constexpr uint16_t kRowSieveLimit = 10000;
constexpr uint64_t kRowSieveLimitRoot = 100;
constexpr uint32_t kRowSieveTableCapacity = 609;
constexpr uint32_t kRowSieveMod3Capacity = 619;
constexpr int kTileRowSieveBlockSize = 256;

__device__ __constant__ uint32_t GM_SIEVE_TABLE[kRowSieveTableCapacity];
__device__ __constant__ uint16_t GM_MOD3_PRIMES[kRowSieveMod3Capacity];
__device__ __constant__ uint32_t GM_SIEVE_TABLE_SIZE = 0;
__device__ __constant__ uint32_t GM_MOD3_PRIMES_SIZE = 0;

__host__ __device__ __forceinline__
uint64_t row_sieve_word_count(uint64_t width) {
    return (width + 31ULL) / 32ULL;
}

__host__ __forceinline__
size_t row_sieve_shared_bytes(uint64_t side_exp) {
    return static_cast<size_t>(row_sieve_word_count(side_exp)) * sizeof(uint32_t);
}

__host__ inline
std::vector<uint16_t> row_sieve_primes_up_to(uint16_t limit) {
    if (limit < 2) {
        return {};
    }

    std::vector<uint8_t> is_prime(static_cast<size_t>(limit) + 1U, 1U);
    is_prime[0] = 0;
    is_prime[1] = 0;

    for (uint32_t p = 2; p * p <= limit; ++p) {
        if (!is_prime[p]) {
            continue;
        }
        for (uint32_t multiple = p * p; multiple <= limit; multiple += p) {
            is_prime[multiple] = 0;
        }
    }

    std::vector<uint16_t> primes;
    primes.reserve(static_cast<size_t>(limit / 2));
    for (uint32_t n = 2; n <= limit; ++n) {
        if (is_prime[n]) {
            primes.push_back(static_cast<uint16_t>(n));
        }
    }
    return primes;
}

__host__ inline
cudaError_t init_row_sieve_tables() {
    const std::vector<uint16_t> primes = row_sieve_primes_up_to(kRowSieveLimit);

    std::vector<uint32_t> sieve_table;
    sieve_table.reserve(kRowSieveTableCapacity);

    std::vector<uint16_t> mod3_primes;
    mod3_primes.reserve(kRowSieveMod3Capacity);

    for (uint16_t p_u16 : primes) {
        if ((p_u16 & 3U) == 1U) {
            const uint64_t p = static_cast<uint64_t>(p_u16);
            uint64_t root = fast_sqrt_neg1(p);
            if (root == UINT64_MAX) {
                return cudaErrorInvalidValue;
            }

            const uint64_t neg_root = p - root;
            if (neg_root < root) {
                root = neg_root;
            }
            sieve_table.push_back(
                (static_cast<uint32_t>(root) << 16) | static_cast<uint32_t>(p_u16));
        } else if ((p_u16 & 3U) == 3U) {
            mod3_primes.push_back(p_u16);
        }
    }

    if (sieve_table.size() != kRowSieveTableCapacity ||
        mod3_primes.size() != kRowSieveMod3Capacity) {
        return cudaErrorInvalidValue;
    }

    const uint32_t sieve_table_size = static_cast<uint32_t>(sieve_table.size());
    const uint32_t mod3_primes_size = static_cast<uint32_t>(mod3_primes.size());

    cudaError_t status = cudaMemcpyToSymbol(
        GM_SIEVE_TABLE,
        sieve_table.data(),
        sieve_table.size() * sizeof(uint32_t));
    if (status != cudaSuccess) {
        return status;
    }

    status = cudaMemcpyToSymbol(
        GM_MOD3_PRIMES,
        mod3_primes.data(),
        mod3_primes.size() * sizeof(uint16_t));
    if (status != cudaSuccess) {
        return status;
    }

    status = cudaMemcpyToSymbol(
        GM_SIEVE_TABLE_SIZE,
        &sieve_table_size,
        sizeof(sieve_table_size));
    if (status != cudaSuccess) {
        return status;
    }

    return cudaMemcpyToSymbol(
        GM_MOD3_PRIMES_SIZE,
        &mod3_primes_size,
        sizeof(mod3_primes_size));
}

__host__ __device__ __forceinline__
int64_t row_sieve_mod_euclid(int64_t value, uint32_t modulus) {
    const int64_t mod = static_cast<int64_t>(modulus);
    int64_t rem = value % mod;
    if (rem < 0) {
        rem += mod;
    }
    return rem;
}

__device__ __forceinline__
void row_sieve_mark_residue_class(
    uint32_t* sieve_bitmap,
    uint64_t side_exp,
    int64_t b_start,
    uint32_t p,
    int64_t residue
) {
    const int64_t b_start_mod = row_sieve_mod_euclid(b_start, p);
    const uint64_t first = static_cast<uint64_t>(
        row_sieve_mod_euclid(residue - b_start_mod, p));
    for (uint64_t idx = first; idx < side_exp; idx += static_cast<uint64_t>(p)) {
        atomicOr(&sieve_bitmap[idx >> 5], 1U << (idx & 31ULL));
    }
}

__device__ __forceinline__
bool row_sieve_is_marked(const uint32_t* sieve_bitmap, uint64_t col) {
    return ((sieve_bitmap[col >> 5] >> (col & 31ULL)) & 1U) != 0U;
}

__global__
void tile_sieved_primality_bitmap_kernel(
    int64_t a_lo,
    int64_t b_lo,
    int64_t collar,
    uint64_t side_exp,
    uint32_t* bitmap
) {
    extern __shared__ uint32_t sieve_bitmap[];

    const uint64_t row = static_cast<uint64_t>(blockIdx.x);
    if (row >= side_exp) {
        return;
    }

    const uint64_t word_count = row_sieve_word_count(side_exp);
    for (uint64_t word = static_cast<uint64_t>(threadIdx.x);
         word < word_count;
         word += static_cast<uint64_t>(blockDim.x)) {
        sieve_bitmap[word] = 0U;
    }
    __syncthreads();

    const int64_t a = a_lo - collar + static_cast<int64_t>(row);
    const int64_t b_start = b_lo - collar;

    for (uint64_t col = static_cast<uint64_t>(threadIdx.x);
         col < side_exp;
         col += static_cast<uint64_t>(blockDim.x)) {
        const int64_t b = b_start + static_cast<int64_t>(col);
        if (((a ^ b) & 1LL) == 0LL) {
            atomicOr(&sieve_bitmap[col >> 5], 1U << (col & 31ULL));
        }
    }
    __syncthreads();

    for (uint32_t pidx = threadIdx.x;
         pidx < GM_SIEVE_TABLE_SIZE;
         pidx += static_cast<uint32_t>(blockDim.x)) {
        const uint32_t packed = GM_SIEVE_TABLE[pidx];
        const uint32_t p = packed & 0xFFFFU;
        const uint32_t r = packed >> 16;
        const int64_t residue =
            (row_sieve_mod_euclid(a, p) * static_cast<int64_t>(r)) % static_cast<int64_t>(p);
        row_sieve_mark_residue_class(sieve_bitmap, side_exp, b_start, p, residue);

        const int64_t neg_residue = row_sieve_mod_euclid(-residue, p);
        if (neg_residue != residue) {
            row_sieve_mark_residue_class(sieve_bitmap, side_exp, b_start, p, neg_residue);
        }
    }
    __syncthreads();

    for (uint32_t pidx = threadIdx.x;
         pidx < GM_MOD3_PRIMES_SIZE;
         pidx += static_cast<uint32_t>(blockDim.x)) {
        const uint32_t p = static_cast<uint32_t>(GM_MOD3_PRIMES[pidx]);
        if (row_sieve_mod_euclid(a, p) == 0) {
            row_sieve_mark_residue_class(sieve_bitmap, side_exp, b_start, p, 0);
        }
    }
    __syncthreads();

    const uint64_t abs_a = abs_i64_to_u64(a);
    const bool rescue_small_norms = abs_a <= kRowSieveLimitRoot;
    const uint64_t a_sq = rescue_small_norms ? abs_a * abs_a : 0ULL;
    if (rescue_small_norms) {
        for (uint64_t col = static_cast<uint64_t>(threadIdx.x);
             col < side_exp;
             col += static_cast<uint64_t>(blockDim.x)) {
            if (!row_sieve_is_marked(sieve_bitmap, col)) {
                continue;
            }

            const int64_t b = b_start + static_cast<int64_t>(col);
            const uint64_t abs_b = abs_i64_to_u64(b);
            const uint64_t norm = a_sq + abs_b * abs_b;
            if (norm < 2 || norm > static_cast<uint64_t>(kRowSieveLimit)) {
                continue;
            }
            if (is_prime(norm)) {
                atomicAnd(&sieve_bitmap[col >> 5], ~(1U << (col & 31ULL)));
            }
        }
    }
    __syncthreads();

    for (uint64_t col = static_cast<uint64_t>(threadIdx.x);
         col < side_exp;
         col += static_cast<uint64_t>(blockDim.x)) {
        const int64_t b = b_start + static_cast<int64_t>(col);
        if (a == 0 || b == 0) {
            if (!is_gaussian_prime_point(a, b)) {
                continue;
            }
        } else if (row_sieve_is_marked(sieve_bitmap, col) ||
                   !is_gaussian_prime_point(a, b)) {
            continue;
        }

        const uint64_t global_idx = row * side_exp + col;
        atomicOr(&bitmap[global_idx >> 5], 1U << (global_idx & 31ULL));
    }
}

} // namespace gm

#endif // GM_ROW_SIEVE_CUH
