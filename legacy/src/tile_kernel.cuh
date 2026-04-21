#ifndef GM_TILE_KERNEL_CUH
#define GM_TILE_KERNEL_CUH

#include <cstddef>
#include <cstdint>

#include <cuda_runtime.h>

#include "miller_rabin.cuh"

__device__ __constant__ uint64_t GM_TILE_K_SQ = 0;

namespace gm {

constexpr int kTileBitmapBlockSize = 256;

__host__ __device__ __forceinline__
uint64_t abs_i64_to_u64(int64_t value) {
    return value < 0
        ? static_cast<uint64_t>(-(value + 1)) + 1ULL
        : static_cast<uint64_t>(value);
}

__host__ __device__ __forceinline__
uint64_t gaussian_norm_u64(int64_t a, int64_t b) {
    const uint64_t ua = abs_i64_to_u64(a);
    const uint64_t ub = abs_i64_to_u64(b);
    const unsigned __int128 norm =
        static_cast<unsigned __int128>(ua) * static_cast<unsigned __int128>(ua) +
        static_cast<unsigned __int128>(ub) * static_cast<unsigned __int128>(ub);
    return static_cast<uint64_t>(norm);
}

__device__ __forceinline__
bool is_axis_gaussian_prime(int64_t coord) {
    const uint64_t magnitude = abs_i64_to_u64(coord);
    return (magnitude & 3ULL) == 3ULL && is_prime(magnitude);
}

__device__ __forceinline__
bool is_gaussian_prime_point(int64_t a, int64_t b) {
    if (a == 0) {
        return is_axis_gaussian_prime(b);
    }
    if (b == 0) {
        return is_axis_gaussian_prime(a);
    }

    const uint64_t norm = gaussian_norm_u64(a, b);
    if (norm < 2) {
        return false;
    }
    if ((norm & 1ULL) == 0ULL && norm > 2) {
        return false;
    }
    return is_prime(norm);
}

__host__ __forceinline__
size_t bitmap_word_count(uint64_t total_points) {
    return static_cast<size_t>((total_points + 31ULL) / 32ULL);
}

__host__ inline
cudaError_t copy_tile_k_sq(uint64_t k_sq) {
    return cudaMemcpyToSymbol(GM_TILE_K_SQ, &k_sq, sizeof(k_sq));
}

__global__
void tile_primality_bitmap_kernel(
    int64_t a_lo,
    int64_t b_lo,
    int64_t collar,
    uint64_t side_exp,
    uint64_t total_points,
    uint32_t* bitmap
) {
    const uint64_t global_idx =
        static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (global_idx >= total_points) {
        return;
    }

    const uint64_t row = global_idx / side_exp;
    const uint64_t col = global_idx % side_exp;
    const int64_t a = a_lo - collar + static_cast<int64_t>(row);
    const int64_t b = b_lo - collar + static_cast<int64_t>(col);

    const uint64_t k_sq = GM_TILE_K_SQ;
    (void)k_sq;

    if (!is_gaussian_prime_point(a, b)) {
        return;
    }

    atomicOr(&bitmap[global_idx >> 5], 1U << (global_idx & 31ULL));
}

} // namespace gm

#endif // GM_TILE_KERNEL_CUH
