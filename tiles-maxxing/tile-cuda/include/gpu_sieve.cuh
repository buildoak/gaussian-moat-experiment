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
        const SplitPrimeBarrettGPU entry = c_split_barrett[k];
        const uint32_t p = static_cast<uint32_t>(entry.p);
        const uint32_t root = static_cast<uint32_t>(entry.root);
        const uint32_t mu = entry.mu;

        const uint32_t a_mod = static_cast<uint32_t>(barrett_euclidean_mod(a, p, mu));
        const uint32_t product = a_mod * root;
        const int32_t residue = static_cast<int32_t>(barrett_mod_u32(product, p, mu));

        mark_residue_class_barrett(ws, b_start, p, residue, mu);

        const int32_t neg_res = (residue == 0) ? 0 : static_cast<int32_t>(p - static_cast<uint32_t>(residue));
        if (neg_res != residue) {
            mark_residue_class_barrett(ws, b_start, p, neg_res, mu);
        }
    }

    for (int k = 0; k < INERT_PRIMES_COUNT; ++k) {
        const InertPrimeBarrettGPU entry = c_inert_barrett[k];
        const uint32_t p = static_cast<uint32_t>(entry.p);
        const uint32_t mu = entry.mu;

        if (barrett_euclidean_mod(a, p, mu) == 0) {
            mark_residue_class_barrett(ws, b_start, p, 0, mu);
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

__device__ void scatter_survivors_clamped(
    const uint32_t ws[BITMAP_WORDS_PER_ROW], uint32_t* cand_list,
    int offset, int max_count, int row) {
    int written = 0;
    #pragma unroll
    for (int w = 0; w < BITMAP_WORDS_PER_ROW && written < max_count; ++w) {
        uint32_t survivors = ~ws[w];
        if (w == (BITMAP_WORDS_PER_ROW - 1)) {
            survivors &= LAST_WORD_MASK;
        }
        while (survivors != 0u && written < max_count) {
            const int bit = __ffs(survivors) - 1;
            const int col = w * 32 + bit;
            if (col < SIDE_EXP) {
                cand_list[offset + written] = (static_cast<uint32_t>(row) << 16) | static_cast<uint32_t>(col);
                ++written;
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
        if (is_prime_norm_gpu(norm)) {
            gpu_bitmap_set_atomic_sieve(bitmap, cand_row, cand_col);
        }
    }
}
