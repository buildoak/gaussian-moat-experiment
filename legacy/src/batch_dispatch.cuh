#ifndef GM_BATCH_DISPATCH_CUH
#define GM_BATCH_DISPATCH_CUH

#include <cstddef>
#include <cstdint>
#include <limits>

#include <cuda_runtime.h>

#include "face_port_io.h"
#include "row_sieve.cuh"
#include "types.h"

namespace gm {

struct BatchContext {
    uint32_t* d_bitmaps = nullptr;
    uint32_t* h_bitmaps = nullptr;
    TileJob* d_jobs = nullptr;
    uint32_t batch_capacity = 0;
    size_t bitmap_words = 0;
    uint64_t side_exp = 0;
};

__device__ __forceinline__
void batch_row_sieve_kernel_body(
    const TileGeometry& geom,
    uint64_t row,
    uint32_t* sieve_bitmap,
    uint32_t* bitmap
) {
    const uint64_t word_count = row_sieve_word_count(geom.side_exp);
    for (uint64_t word = static_cast<uint64_t>(threadIdx.x);
         word < word_count;
         word += static_cast<uint64_t>(blockDim.x)) {
        sieve_bitmap[word] = 0U;
    }
    __syncthreads();

    const int64_t a = geom.a_lo - geom.collar + static_cast<int64_t>(row);
    const int64_t b_start = geom.b_lo - geom.collar;

    for (uint64_t col = static_cast<uint64_t>(threadIdx.x);
         col < geom.side_exp;
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
        row_sieve_mark_residue_class(sieve_bitmap, geom.side_exp, b_start, p, residue);

        const int64_t neg_residue = row_sieve_mod_euclid(-residue, p);
        if (neg_residue != residue) {
            row_sieve_mark_residue_class(sieve_bitmap, geom.side_exp, b_start, p, neg_residue);
        }
    }
    __syncthreads();

    for (uint32_t pidx = threadIdx.x;
         pidx < GM_MOD3_PRIMES_SIZE;
         pidx += static_cast<uint32_t>(blockDim.x)) {
        const uint32_t p = static_cast<uint32_t>(GM_MOD3_PRIMES[pidx]);
        if (row_sieve_mod_euclid(a, p) == 0) {
            row_sieve_mark_residue_class(sieve_bitmap, geom.side_exp, b_start, p, 0);
        }
    }
    __syncthreads();

    const uint64_t abs_a = abs_i64_to_u64(a);
    const bool rescue_small_norms = abs_a <= kRowSieveLimitRoot;
    const uint64_t a_sq = rescue_small_norms ? abs_a * abs_a : 0ULL;
    if (rescue_small_norms) {
        for (uint64_t col = static_cast<uint64_t>(threadIdx.x);
             col < geom.side_exp;
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
         col < geom.side_exp;
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

        const uint64_t global_idx = row * geom.side_exp + col;
        atomicOr(&bitmap[global_idx >> 5], 1U << (global_idx & 31ULL));
    }
}

__global__
void batch_tile_sieved_primality_bitmap_kernel(
    const TileJob* jobs,
    uint32_t num_tiles,
    uint32_t tile_side,
    int64_t collar,
    uint64_t side_exp,
    size_t bitmap_words,
    uint32_t* bitmaps
) {
    extern __shared__ uint32_t sieve_bitmap[];

    const uint64_t block = static_cast<uint64_t>(blockIdx.x);
    const uint64_t tile_idx = block / side_exp;
    const uint64_t row = block % side_exp;
    if (tile_idx >= static_cast<uint64_t>(num_tiles) || row >= side_exp) {
        return;
    }

    const TileJob job = jobs[tile_idx];
    TileGeometry geom{};
    geom.k_sq = GM_TILE_K_SQ;
    geom.collar = collar;
    geom.a_lo = static_cast<int64_t>(job.a_lo);
    geom.a_hi = geom.a_lo + static_cast<int64_t>(tile_side);
    geom.b_lo = static_cast<int64_t>(job.b_lo);
    geom.b_hi = geom.b_lo + static_cast<int64_t>(tile_side);
    geom.expanded_a_lo = geom.a_lo - geom.collar;
    geom.expanded_b_lo = geom.b_lo - geom.collar;
    geom.nominal_extent = static_cast<uint64_t>(tile_side) + 1ULL;
    geom.side_exp = side_exp;
    geom.total_points = side_exp * side_exp;

    uint32_t* bitmap = bitmaps + tile_idx * bitmap_words;
    batch_row_sieve_kernel_body(geom, row, sieve_bitmap, bitmap);
}

inline cudaError_t create_batch_context(
    uint32_t requested_batch_capacity,
    uint64_t side_exp,
    BatchContext* ctx
) {
    if (ctx == nullptr || side_exp == 0) {
        return cudaErrorInvalidValue;
    }

    BatchContext next{};
    next.side_exp = side_exp;

    const uint64_t total_points = side_exp * side_exp;
    next.bitmap_words = bitmap_word_count(total_points);
    const size_t bitmap_bytes = next.bitmap_words * sizeof(uint32_t);

    uint32_t batch_capacity = requested_batch_capacity;
    if (batch_capacity == 0) {
        size_t free_bytes = 0;
        size_t total_bytes = 0;
        cudaError_t status = cudaMemGetInfo(&free_bytes, &total_bytes);
        if (status != cudaSuccess) {
            return status;
        }

        const size_t reserve_bytes =
            free_bytes > (64ULL << 20) ? (64ULL << 20) : free_bytes / 8U;
        const size_t usable_bytes = free_bytes > reserve_bytes ? free_bytes - reserve_bytes : 0;
        const size_t per_tile_device_bytes = bitmap_bytes + sizeof(TileJob);
        if (per_tile_device_bytes == 0 || usable_bytes < per_tile_device_bytes) {
            return cudaErrorMemoryAllocation;
        }

        batch_capacity = static_cast<uint32_t>(usable_bytes / per_tile_device_bytes);
    }

    const uint64_t max_grid_tiles =
        side_exp == 0 ? 0 : static_cast<uint64_t>(std::numeric_limits<unsigned int>::max()) / side_exp;
    if (max_grid_tiles == 0) {
        return cudaErrorInvalidConfiguration;
    }
    if (batch_capacity > max_grid_tiles) {
        batch_capacity = static_cast<uint32_t>(max_grid_tiles);
    }
    if (batch_capacity == 0) {
        return cudaErrorMemoryAllocation;
    }

    const size_t total_bitmap_bytes = static_cast<size_t>(batch_capacity) * bitmap_bytes;
    cudaError_t status = cudaMalloc(&next.d_bitmaps, total_bitmap_bytes);
    if (status != cudaSuccess) {
        return status;
    }

    status = cudaMallocHost(&next.h_bitmaps, total_bitmap_bytes);
    if (status != cudaSuccess) {
        cudaFree(next.d_bitmaps);
        return status;
    }

    status = cudaMalloc(&next.d_jobs, static_cast<size_t>(batch_capacity) * sizeof(TileJob));
    if (status != cudaSuccess) {
        cudaFreeHost(next.h_bitmaps);
        cudaFree(next.d_bitmaps);
        return status;
    }

    next.batch_capacity = batch_capacity;
    *ctx = next;
    return cudaSuccess;
}

inline void destroy_batch_context(BatchContext* ctx) {
    if (ctx == nullptr) {
        return;
    }

    if (ctx->d_jobs != nullptr) {
        cudaFree(ctx->d_jobs);
    }
    if (ctx->h_bitmaps != nullptr) {
        cudaFreeHost(ctx->h_bitmaps);
    }
    if (ctx->d_bitmaps != nullptr) {
        cudaFree(ctx->d_bitmaps);
    }
    *ctx = BatchContext{};
}

inline cudaError_t launch_batch_sieve(
    const BatchContext& ctx,
    const TileJob* h_jobs,
    uint32_t num_tiles,
    uint32_t tile_side,
    int64_t collar
) {
    if (num_tiles == 0 || num_tiles > ctx.batch_capacity) {
        return cudaErrorInvalidValue;
    }

    const size_t jobs_bytes = static_cast<size_t>(num_tiles) * sizeof(TileJob);
    cudaError_t status = cudaMemcpy(ctx.d_jobs, h_jobs, jobs_bytes, cudaMemcpyHostToDevice);
    if (status != cudaSuccess) {
        return status;
    }

    const size_t bitmap_bytes = static_cast<size_t>(num_tiles) * ctx.bitmap_words * sizeof(uint32_t);
    status = cudaMemset(ctx.d_bitmaps, 0, bitmap_bytes);
    if (status != cudaSuccess) {
        return status;
    }

    const uint64_t blocks64 = static_cast<uint64_t>(num_tiles) * ctx.side_exp;
    if (blocks64 > static_cast<uint64_t>(std::numeric_limits<unsigned int>::max())) {
        return cudaErrorInvalidConfiguration;
    }

    batch_tile_sieved_primality_bitmap_kernel<<<
        static_cast<unsigned int>(blocks64),
        kTileRowSieveBlockSize,
        row_sieve_shared_bytes(ctx.side_exp)>>>(
        ctx.d_jobs,
        num_tiles,
        tile_side,
        collar,
        ctx.side_exp,
        ctx.bitmap_words,
        ctx.d_bitmaps
    );

    status = cudaGetLastError();
    if (status != cudaSuccess) {
        return status;
    }

    return cudaDeviceSynchronize();
}

inline cudaError_t transfer_batch_bitmaps(const BatchContext& ctx, uint32_t num_tiles) {
    if (num_tiles == 0 || num_tiles > ctx.batch_capacity) {
        return cudaErrorInvalidValue;
    }

    const size_t bitmap_bytes = static_cast<size_t>(num_tiles) * ctx.bitmap_words * sizeof(uint32_t);
    return cudaMemcpy(ctx.h_bitmaps, ctx.d_bitmaps, bitmap_bytes, cudaMemcpyDeviceToHost);
}

inline const uint32_t* host_bitmap_slice(const BatchContext& ctx, uint32_t tile_idx) {
    return ctx.h_bitmaps + static_cast<size_t>(tile_idx) * ctx.bitmap_words;
}

} // namespace gm

#endif // GM_BATCH_DISPATCH_CUH
