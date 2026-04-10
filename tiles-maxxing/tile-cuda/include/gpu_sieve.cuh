#pragma once

#include <cuda_runtime.h>
#include <stdint.h>

#include "gpu_constants.cuh"
#include "gpu_math.cuh"
#include "gpu_types.cuh"

static __device__ __forceinline__ void gpu_bitmap_set_atomic_sieve(uint32_t* bitmap, int row, int col) {
    atomicOr(&bitmap[row * BITMAP_WORDS_PER_ROW + (col >> 5)], 1u << (col & 31));
}

__device__ void sieve_row(uint32_t ws[BITMAP_WORDS_PER_ROW], int32_t a, int32_t b_start) {
    const uint32_t pattern = ((a ^ b_start) & 1) != 0 ? 0xAAAAAAAAu : 0x55555555u;
    #pragma unroll
    for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
        ws[w] = pattern;
    }
    ws[BITMAP_WORDS_PER_ROW - 1] &= LAST_WORD_MASK;

    for (int k = 0; k < SPLIT_PRIMES_COUNT; ++k) {
        const uint32_t packed = c_split_table[k];
        const uint32_t p = packed & 0xFFFFu;
        const uint32_t root = packed >> 16;
        const int32_t residue = static_cast<int32_t>(
            (static_cast<int64_t>(euclidean_mod_gpu(a, p)) * static_cast<int64_t>(root)) %
            static_cast<int64_t>(p));
        mark_residue_class_reg(ws, b_start, p, residue);

        const int32_t neg_res = euclidean_mod_gpu(-residue, p);
        if (neg_res != residue) {
            mark_residue_class_reg(ws, b_start, p, neg_res);
        }
    }

    for (int k = 0; k < INERT_PRIMES_COUNT; ++k) {
        const uint32_t p = static_cast<uint32_t>(c_inert_primes[k]);
        if (euclidean_mod_gpu(a, p) == 0) {
            mark_residue_class_reg(ws, b_start, p, 0);
        }
    }
}

__device__ uint32_t count_sieve_survivors(const uint32_t ws[BITMAP_WORDS_PER_ROW]) {
    uint32_t count = 0;
    #pragma unroll
    for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
        uint32_t survivors = ~ws[w];
        if (w == (BITMAP_WORDS_PER_ROW - 1)) {
            survivors &= LAST_WORD_MASK;
        }
        count += __popc(survivors);
    }
    return count;
}

__device__ void scatter_survivors(
    const uint32_t ws[BITMAP_WORDS_PER_ROW], uint32_t* cand_list, int offset, int row) {
    #pragma unroll
    for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
        uint32_t survivors = ~ws[w];
        if (w == (BITMAP_WORDS_PER_ROW - 1)) {
            survivors &= LAST_WORD_MASK;
        }
        while (survivors != 0u) {
            const int bit = __ffs(survivors) - 1;
            const int col = w * 32 + bit;
            if (col < SIDE_EXP) {
                cand_list[offset++] = (static_cast<uint32_t>(row) << 16) | static_cast<uint32_t>(col);
            }
            survivors &= (survivors - 1u);
        }
    }
}

__device__ void mr_test_candidates(
    const uint32_t* cand_list,
    int total_cands,
    int32_t a_start,
    int32_t b_start,
    uint32_t* bitmap,
    int tid,
    int block_size) {
    for (int i = tid; i < total_cands; i += block_size) {
        const uint32_t packed = cand_list[i];
        const int cand_row = static_cast<int>(packed >> 16);
        const int cand_col = static_cast<int>(packed & 0xFFFFu);
        const int32_t ca = a_start + cand_row;
        const int32_t cb = b_start + cand_col;

        if (ca == 0 || cb == 0) {
            if (is_axis_gaussian_prime_gpu(ca, cb)) {
                gpu_bitmap_set_atomic_sieve(bitmap, cand_row, cand_col);
            }
            continue;
        }

        const uint64_t ua = static_cast<uint64_t>(ca < 0 ? -static_cast<int64_t>(ca) : ca);
        const uint64_t ub = static_cast<uint64_t>(cb < 0 ? -static_cast<int64_t>(cb) : cb);
        const uint64_t norm = ua * ua + ub * ub;
        if (is_prime_gpu(norm)) {
            gpu_bitmap_set_atomic_sieve(bitmap, cand_row, cand_col);
        }
    }
}
