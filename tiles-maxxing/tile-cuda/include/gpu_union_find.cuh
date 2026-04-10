#pragma once

#include <cuda_runtime.h>
#include <stdint.h>

#include "gpu_constants.cuh"
#include "gpu_math.cuh"
#include "gpu_types.cuh"

static __device__ __forceinline__ bool gpu_bitmap_test_union_find(const uint32_t* bitmap, int row, int col) {
    return ((bitmap[row * BITMAP_WORDS_PER_ROW + (col >> 5)] >> (col & 31)) & 1u) != 0u;
}

static __device__ __forceinline__ int gpu_uf_index_union_find(
    int row, int col, const uint32_t* bitmap, const uint16_t* row_prefix) {
    uint32_t idx = row_prefix[row];
    const int full_words = col >> 5;
    for (int w = 0; w < full_words; ++w) {
        idx += __popc(bitmap[row * BITMAP_WORDS_PER_ROW + w]);
    }

    const uint32_t bit_mask = (col & 31) == 0 ? 0u : ((1u << (col & 31)) - 1u);
    idx += __popc(bitmap[row * BITMAP_WORDS_PER_ROW + full_words] & bit_mask);
    return static_cast<int>(idx);
}

__device__ uint16_t find_root_local(uint16_t* parent, uint16_t x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

__device__ void union_local(uint16_t* parent, uint16_t x, uint16_t y) {
    uint16_t rx = find_root_local(parent, x);
    uint16_t ry = find_root_local(parent, y);
    if (rx == ry) {
        return;
    }
    if (rx > ry) {
        const uint16_t tmp = rx;
        rx = ry;
        ry = tmp;
    }
    parent[ry] = rx;
}

__device__ void build_components_gpu(
    const uint32_t* bitmap,
    const uint16_t* row_prefix,
    const uint32_t* prime_pos,
    int prime_count,
    uint16_t* parent,
    int lane) {
    const int bounded_prime_count = prime_count < MAX_PRIMES_GPU ? prime_count : MAX_PRIMES_GPU;

    for (int i = lane; i < bounded_prime_count; i += warpSize) {
        parent[i] = static_cast<uint16_t>(i);
    }
    __syncwarp();

    for (int i = lane; i < bounded_prime_count; i += warpSize) {
        const uint32_t packed = prime_pos[i];
        const int row = static_cast<int>(packed / SIDE_EXP);
        const int col = static_cast<int>(packed % SIDE_EXP);

        for (int k = 0; k < NUM_BACKWARD_OFFSETS; ++k) {
            const int nr = row + static_cast<int>(c_bk_dr[k]);
            const int nc = col + static_cast<int>(c_bk_dc[k]);
            if (nr < 0 || nr >= SIDE_EXP || nc < 0 || nc >= SIDE_EXP) {
                continue;
            }
            if (!gpu_bitmap_test_union_find(bitmap, nr, nc)) {
                continue;
            }

            const int j = gpu_uf_index_union_find(nr, nc, bitmap, row_prefix);
            if (j >= 0 && j < bounded_prime_count) {
                union_local(parent, static_cast<uint16_t>(i), static_cast<uint16_t>(j));
            }
        }
    }
    __syncwarp();

    for (int i = lane; i < bounded_prime_count; i += warpSize) {
        parent[i] = find_root_local(parent, static_cast<uint16_t>(i));
    }
}
