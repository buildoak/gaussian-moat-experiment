#pragma once

#include <cuda_runtime.h>
#include <stdint.h>

#include "gpu_constants.cuh"
#include "gpu_math.cuh"
#include "gpu_types.cuh"

static __device__ __forceinline__ bool gpu_bitmap_test_compact(const uint32_t* bitmap, int row, int col) {
    return ((bitmap[row * BITMAP_WORDS_PER_ROW + (col >> 5)] >> (col & 31)) & 1u) != 0u;
}

static __device__ __forceinline__ void gpu_bitmap_set_atomic_compact(uint32_t* bitmap, int row, int col) {
    atomicOr(&bitmap[row * BITMAP_WORDS_PER_ROW + (col >> 5)], 1u << (col & 31));
}

__device__ void block_exclusive_scan(uint32_t* data, int n, int tid) {
    const uint32_t original = tid < n ? data[tid] : 0u;
    for (int offset = 1; offset < n; offset <<= 1) {
        uint32_t addend = 0u;
        if (tid < n && tid >= offset) {
            addend = data[tid - offset];
        }
        __syncthreads();
        if (tid < n) {
            data[tid] += addend;
        }
        __syncthreads();
    }

    if (tid < n) {
        data[tid] -= original;
    }
    __syncthreads();
}

__device__ void block_exclusive_scan_u16(uint16_t* data, int n, int tid) {
    const uint16_t original = tid < n ? data[tid] : 0u;
    for (int offset = 1; offset < n; offset <<= 1) {
        uint16_t addend = 0u;
        if (tid < n && tid >= offset) {
            addend = data[tid - offset];
        }
        __syncthreads();
        if (tid < n) {
            data[tid] = static_cast<uint16_t>(data[tid] + addend);
        }
        __syncthreads();
    }

    if (tid < n) {
        data[tid] = static_cast<uint16_t>(data[tid] - original);
    }
    __syncthreads();
}

__device__ int compact_row(
    const uint32_t* bitmap,
    uint16_t* row_prefix,
    uint32_t* prime_pos,
    int tid) {
    uint32_t row_count = 0;

    if (tid < ACTIVE_ROWS) {
        #pragma unroll
        for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
            uint32_t word = bitmap[tid * BITMAP_WORDS_PER_ROW + w];
            if (w == (BITMAP_WORDS_PER_ROW - 1)) {
                word &= LAST_WORD_MASK;
            }
            row_count += __popc(word);
        }
        row_prefix[tid] = static_cast<uint16_t>(row_count);
    }
    __syncthreads();

    block_exclusive_scan_u16(row_prefix, ACTIVE_ROWS, tid);
    __syncthreads();

    if (tid == (ACTIVE_ROWS - 1)) {
        row_prefix[ACTIVE_ROWS] = static_cast<uint16_t>(row_prefix[ACTIVE_ROWS - 1] + row_count);
    }
    __syncthreads();

    if (tid < ACTIVE_ROWS) {
        uint16_t offset = row_prefix[tid];
        #pragma unroll
        for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
            uint32_t word = bitmap[tid * BITMAP_WORDS_PER_ROW + w];
            if (w == (BITMAP_WORDS_PER_ROW - 1)) {
                word &= LAST_WORD_MASK;
            }
            while (word != 0u) {
                const int bit = __ffs(word) - 1;
                if (offset < MAX_PRIMES_GPU) {
                    prime_pos[offset] =
                        static_cast<uint32_t>(tid * SIDE_EXP + (w * 32 + bit));
                }
                offset = static_cast<uint16_t>(offset + 1u);
                word &= (word - 1u);
            }
        }
    }
    __syncthreads();

    return static_cast<int>(row_prefix[ACTIVE_ROWS]);
}

__device__ int uf_index_gpu(int row, int col, const uint32_t* bitmap, const uint16_t* row_prefix) {
    uint32_t idx = row_prefix[row];
    const int full_words = col >> 5;
    for (int w = 0; w < full_words; ++w) {
        idx += __popc(bitmap[row * BITMAP_WORDS_PER_ROW + w]);
    }

    const uint32_t bit_mask = (col & 31) == 0 ? 0u : ((1u << (col & 31)) - 1u);
    idx += __popc(bitmap[row * BITMAP_WORDS_PER_ROW + full_words] & bit_mask);
    return static_cast<int>(idx);
}
