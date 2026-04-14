// K1 Sieve — Barrett-reduction sieve against split+inert primes.
// 288 threads/block, target <=40 registers.
// Each thread handles one row (271 active rows).
// Sieve survivors scatter to d_cand_list via atomicAdd on d_total_cands.

#include "gpu_constants.cuh"
#include "gpu_types.cuh"
#include "gpu_math.cuh"

#include <cstdio>
#include <cstdint>

// ---- Constant memory definitions (primary TU) ----
__constant__ SplitPrimeBarrettGPU c_split_barrett[SPLIT_PRIMES_COUNT];
__constant__ InertPrimeBarrettGPU c_inert_barrett[INERT_PRIMES_COUNT];
__constant__ uint64_t c_mr_witnesses[NUM_MR_WITNESSES];
__constant__ uint32_t c_trial_primes[NUM_TRIAL_PRIMES];
__constant__ int8_t   c_bk_dr[NUM_BACKWARD_OFFSETS];
__constant__ int8_t   c_bk_dc[NUM_BACKWARD_OFFSETS];

// ---- Device helpers ----

__device__ void sieve_row_k1(uint32_t ws[BITMAP_WORDS_PER_ROW], int32_t a, int32_t b_start) {
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

__device__ uint32_t count_sieve_survivors_k1(const uint32_t ws[BITMAP_WORDS_PER_ROW]) {
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

__device__ void scatter_survivors_k1(
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

__device__ void scatter_survivors_clamped_k1(
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

// ---- K1 Sieve kernel ----

__global__ void kernel_sieve(
    const TileCoord* __restrict__ coords,
    uint32_t* __restrict__ d_cand_list,
    uint32_t* __restrict__ d_total_cands,
    int num_tiles) {
    const int tile_idx = static_cast<int>(blockIdx.x);
    if (tile_idx >= num_tiles) return;

    const int tid = static_cast<int>(threadIdx.x);
    const TileCoord coord = coords[tile_idx];
    const int32_t a_start = static_cast<int32_t>(coord.a_lo - static_cast<int64_t>(COLLAR));
    const int32_t b_start = static_cast<int32_t>(coord.b_lo - static_cast<int64_t>(COLLAR));

    // Per-tile candidate list in global memory
    uint32_t* tile_cand_list = d_cand_list + static_cast<size_t>(tile_idx) * MAX_CANDIDATES_GPU;
    __shared__ uint32_t total_cands;

    if (tid == 0) {
        total_cands = 0U;
    }
    __syncthreads();

    if (tid < ACTIVE_ROWS) {
        uint32_t ws[BITMAP_WORDS_PER_ROW];
        const int32_t a = a_start + tid;
        sieve_row_k1(ws, a, b_start);

        const uint32_t count = count_sieve_survivors_k1(ws);
        if (count > 0U) {
            const uint32_t base = atomicAdd(&total_cands, count);
            if (base + count <= static_cast<uint32_t>(MAX_CANDIDATES_GPU)) {
                scatter_survivors_k1(ws, tile_cand_list, static_cast<int>(base), tid);
            } else if (base < static_cast<uint32_t>(MAX_CANDIDATES_GPU)) {
                scatter_survivors_clamped_k1(ws, tile_cand_list, static_cast<int>(base),
                                             static_cast<int>(static_cast<uint32_t>(MAX_CANDIDATES_GPU) - base), tid);
            }
        }
    }
    __syncthreads();

    if (tid == 0) {
        uint32_t final_count = total_cands;
        if (final_count > static_cast<uint32_t>(MAX_CANDIDATES_GPU)) {
            final_count = static_cast<uint32_t>(MAX_CANDIDATES_GPU);
        }
        d_total_cands[tile_idx] = final_count;
    }
}

// ---- Host-side constant memory upload functions ----

namespace {
inline void check_cuda(cudaError_t status, const char* what) {
    if (status != cudaSuccess) {
        fprintf(stderr, "%s failed: %s\n", what, cudaGetErrorString(status));
        std::abort();
    }
}
}

void upload_sieve_tables(const SieveTablesBarrett& tables) {
    check_cuda(cudaMemcpyToSymbol(c_split_barrett, tables.split_table,
                                  sizeof(SplitPrimeBarrettGPU) * SPLIT_PRIMES_COUNT),
               "cudaMemcpyToSymbol(c_split_barrett)");
    check_cuda(cudaMemcpyToSymbol(c_inert_barrett, tables.inert_primes,
                                  sizeof(InertPrimeBarrettGPU) * INERT_PRIMES_COUNT),
               "cudaMemcpyToSymbol(c_inert_barrett)");
}

void upload_backward_offsets(const int8_t* bk_dr, const int8_t* bk_dc, int count) {
    if (count != NUM_BACKWARD_OFFSETS) {
        fprintf(stderr, "backward offset count mismatch: expected %d got %d\n",
                NUM_BACKWARD_OFFSETS, count);
        std::abort();
    }
    check_cuda(cudaMemcpyToSymbol(c_bk_dr, bk_dr, sizeof(int8_t) * NUM_BACKWARD_OFFSETS),
               "cudaMemcpyToSymbol(c_bk_dr)");
    check_cuda(cudaMemcpyToSymbol(c_bk_dc, bk_dc, sizeof(int8_t) * NUM_BACKWARD_OFFSETS),
               "cudaMemcpyToSymbol(c_bk_dc)");
}

void upload_constants() {
    constexpr uint64_t kMrWitnesses[NUM_MR_WITNESSES] = {
        2ULL, 325ULL, 9375ULL, 28178ULL, 450775ULL, 9780504ULL, 1795265022ULL,
    };
    constexpr uint32_t kTrialPrimes[NUM_TRIAL_PRIMES] = {
        3U, 5U, 7U, 11U, 13U, 17U, 19U, 23U, 29U, 31U, 37U, 41U,
        43U, 47U, 53U, 59U, 61U, 67U, 71U, 73U, 79U, 83U, 89U, 97U,
    };
    check_cuda(cudaMemcpyToSymbol(c_mr_witnesses, kMrWitnesses, sizeof(kMrWitnesses)),
               "cudaMemcpyToSymbol(c_mr_witnesses)");
    check_cuda(cudaMemcpyToSymbol(c_trial_primes, kTrialPrimes, sizeof(kTrialPrimes)),
               "cudaMemcpyToSymbol(c_trial_primes)");
}
