#include "gpu_constants.cuh"
#include "gpu_types.cuh"
#include "gpu_math.cuh"
#include "gpu_sieve.cuh"
#include "gpu_compact.cuh"
#include "gpu_union_find.cuh"
#include "gpu_face_encode.cuh"

#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>

__constant__ uint32_t c_split_table[SPLIT_PRIMES_COUNT];
__constant__ uint16_t c_inert_primes[INERT_PRIMES_COUNT];
__constant__ uint64_t c_mr_witnesses[NUM_MR_WITNESSES];
__constant__ uint32_t c_trial_primes[NUM_TRIAL_PRIMES];
__constant__ int8_t   c_bk_dr[NUM_BACKWARD_OFFSETS];
__constant__ int8_t   c_bk_dc[NUM_BACKWARD_OFFSETS];

namespace {

constexpr uint64_t kMrWitnesses[NUM_MR_WITNESSES] = {
    2ULL, 3ULL, 5ULL, 7ULL, 11ULL, 13ULL, 17ULL, 19ULL, 23ULL, 29ULL, 31ULL, 37ULL,
};

constexpr uint32_t kTrialPrimes[NUM_TRIAL_PRIMES] = {
    3U, 5U, 7U, 11U, 13U, 17U, 19U, 23U, 29U, 31U, 37U, 41U,
    43U, 47U, 53U, 59U, 61U, 67U, 71U, 73U, 79U, 83U, 89U, 97U,
};

constexpr int kBitmapWords = ACTIVE_ROWS * BITMAP_WORDS_PER_ROW;
constexpr int kPhase1Words = BLOCK_THREADS + (BLOCK_THREADS + 1) + MAX_PRIMES_GPU;
constexpr int kPhase24Words = (ACTIVE_ROWS + 1) + MAX_PRIMES_GPU + MAX_PRIMES_GPU;

__device__ void poison_tileop(TileOp* tileop) {
    for (int i = 0; i < TILEOP_SIZE; ++i) {
        tileop->bytes[i] = OVERFLOW_SENTINEL;
    }
}

inline void check_cuda(cudaError_t status, const char* what) {
    if (status != cudaSuccess) {
        fprintf(stderr, "%s failed: %s\n", what, cudaGetErrorString(status));
        std::abort();
    }
}

}  // namespace

__global__ void process_tiles_kernel(const TileCoord* __restrict__ coords,
                                     TileOp* __restrict__ output,
                                     uint32_t* __restrict__ prime_counts,
                                     int num_tiles) {
    const int tile_idx = static_cast<int>(blockIdx.x);
    if (tile_idx >= num_tiles) {
        return;
    }

    extern __shared__ uint32_t smem[];

    uint32_t* const bitmap = smem;
    uint32_t* const overlay = bitmap + kBitmapWords;

    uint32_t* const cand_counts = overlay;
    uint32_t* const cand_prefix = cand_counts + BLOCK_THREADS;
    uint32_t* const cand_list = cand_prefix + (BLOCK_THREADS + 1);

    uint32_t* const row_prefix = overlay;
    uint32_t* const prime_pos = row_prefix + (ACTIVE_ROWS + 1);
    uint32_t* const parent = prime_pos + MAX_PRIMES_GPU;

    __shared__ uint32_t total_cands;

    const int tid = static_cast<int>(threadIdx.x);
    const TileCoord coord = coords[tile_idx];
    const int32_t a_start = static_cast<int32_t>(coord.a_lo - static_cast<int64_t>(COLLAR));
    const int32_t b_start = static_cast<int32_t>(coord.b_lo - static_cast<int64_t>(COLLAR));

    for (int i = tid; i < kBitmapWords; i += BLOCK_THREADS) {
        bitmap[i] = 0U;
    }
    __syncthreads();

    if (tid < ACTIVE_ROWS) {
        uint32_t ws[BITMAP_WORDS_PER_ROW];
        const int32_t a = a_start + tid;
        sieve_row(ws, a, b_start);

        cand_counts[tid] = count_sieve_survivors(ws);
        cand_prefix[tid] = cand_counts[tid];
    } else {
        cand_counts[tid] = 0U;
        if (tid == ACTIVE_ROWS) {
            cand_prefix[ACTIVE_ROWS] = 0U;
        }
    }
    __syncthreads();

    block_exclusive_scan(cand_prefix, ACTIVE_ROWS, tid);
    __syncthreads();

    if (tid < ACTIVE_ROWS) {
        uint32_t ws[BITMAP_WORDS_PER_ROW];
        const int32_t a = a_start + tid;
        sieve_row(ws, a, b_start);
        scatter_survivors(ws, cand_list, static_cast<int>(cand_prefix[tid]), tid);
    }
    __syncthreads();

    if (tid == 0) {
        total_cands = cand_prefix[ACTIVE_ROWS - 1] + cand_counts[ACTIVE_ROWS - 1];
    }
    __syncthreads();

    mr_test_candidates(cand_list, static_cast<int>(total_cands), a_start, b_start,
                       bitmap, tid, BLOCK_THREADS);
    __syncthreads();

    if (tid < ACTIVE_ROWS) {
        (void)compact_row(bitmap, row_prefix, prime_pos, tid);
    }
    __syncthreads();

    const int prime_count = static_cast<int>(row_prefix[ACTIVE_ROWS]);
    if (prime_count > MAX_PRIMES_GPU) {
        if (tid == 0) {
            poison_tileop(&output[tile_idx]);
            if (prime_counts != nullptr) {
                prime_counts[tile_idx] = static_cast<uint32_t>(prime_count);
            }
        }
        return;
    }

    if (tid < 32) {
        build_components_gpu(bitmap, row_prefix, prime_pos, prime_count, parent, tid);
    }
    __syncthreads();

    if (tid == 0) {
        FaceDataGPU face_data{};
        extract_faces_gpu(bitmap, row_prefix, prime_pos, prime_count, parent, &face_data);
        prune_dead_ends_gpu(&face_data);
        encode_tileop_gpu(&face_data, &output[tile_idx]);
        if (prime_counts != nullptr) {
            prime_counts[tile_idx] = static_cast<uint32_t>(prime_count);
        }
    }
}

void upload_sieve_tables(const SieveTables& tables) {
    check_cuda(cudaMemcpyToSymbol(c_split_table, tables.split_table,
                                  sizeof(uint32_t) * SPLIT_PRIMES_COUNT),
               "cudaMemcpyToSymbol(c_split_table)");
    check_cuda(cudaMemcpyToSymbol(c_inert_primes, tables.inert_primes,
                                  sizeof(uint16_t) * INERT_PRIMES_COUNT),
               "cudaMemcpyToSymbol(c_inert_primes)");
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
    check_cuda(cudaMemcpyToSymbol(c_mr_witnesses, kMrWitnesses, sizeof(kMrWitnesses)),
               "cudaMemcpyToSymbol(c_mr_witnesses)");
    check_cuda(cudaMemcpyToSymbol(c_trial_primes, kTrialPrimes, sizeof(kTrialPrimes)),
               "cudaMemcpyToSymbol(c_trial_primes)");
}

size_t tile_kernel_shared_bytes() {
    const size_t overlay_words = static_cast<size_t>(kPhase1Words > kPhase24Words
                                                         ? kPhase1Words
                                                         : kPhase24Words);
    return sizeof(uint32_t) * (static_cast<size_t>(kBitmapWords) + overlay_words);
}
