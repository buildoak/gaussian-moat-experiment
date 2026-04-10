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

__constant__ SplitPrimeBarrettGPU c_split_barrett[SPLIT_PRIMES_COUNT];
__constant__ InertPrimeBarrettGPU c_inert_barrett[INERT_PRIMES_COUNT];
__constant__ uint64_t c_mr_witnesses[NUM_MR_WITNESSES];
__constant__ uint32_t c_trial_primes[NUM_TRIAL_PRIMES];
__constant__ int8_t   c_bk_dr[NUM_BACKWARD_OFFSETS];
__constant__ int8_t   c_bk_dc[NUM_BACKWARD_OFFSETS];

namespace {

// Sinclair deterministic 7-base set: covers all n < 2^64.
// See: miller-rabin.appspot.com, Wikipedia "Miller-Rabin primality test".
constexpr uint64_t kMrWitnesses[NUM_MR_WITNESSES] = {
    2ULL, 325ULL, 9375ULL, 28178ULL, 450775ULL, 9780504ULL, 1795265022ULL,
};

constexpr uint32_t kTrialPrimes[NUM_TRIAL_PRIMES] = {
    3U, 5U, 7U, 11U, 13U, 17U, 19U, 23U, 29U, 31U, 37U, 41U,
    43U, 47U, 53U, 59U, 61U, 67U, 71U, 73U, 79U, 83U, 89U, 97U,
};

constexpr int kBitmapWords = ACTIVE_ROWS * BITMAP_WORDS_PER_ROW;

// Sieve survivors (candidates) can exceed MAX_PRIMES_GPU because the sieve
// pass runs before the Miller-Rabin filter.  Empirically, tiles at radius
// ~600-860M produce up to ~5700 sieve survivors across 271 rows (~21/row).
// 8192 covers the worst observed case with comfortable headroom (~30/row).
constexpr int MAX_CANDIDATES_GPU = 6144;

static_assert(MAX_CANDIDATES_GPU >= MAX_PRIMES_GPU,
              "candidate list must hold at least MAX_PRIMES_GPU entries");

constexpr int kPhase1Words = MAX_CANDIDATES_GPU;
constexpr size_t kBitmapBytes = sizeof(uint32_t) * static_cast<size_t>(kBitmapWords);
constexpr size_t kPhase1Bytes = sizeof(uint32_t) * static_cast<size_t>(kPhase1Words);
constexpr size_t kPhase24Bytes =
    sizeof(uint16_t) * static_cast<size_t>(ACTIVE_ROWS + 1 + MAX_PRIMES_GPU) +
    sizeof(uint32_t) * static_cast<size_t>(MAX_PRIMES_GPU) +
    sizeof(FacePrimeGPU) * static_cast<size_t>(NUM_FACES * MAX_FACE_PRIMES_PER_FACE) +
    sizeof(uint32_t) * static_cast<size_t>(NUM_FACES);

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
#ifdef PROFILE_PHASES
                                     PhaseTimingGPU* __restrict__ phase_timings,
#endif
                                     int num_tiles) {
    const int tile_idx = static_cast<int>(blockIdx.x);
    if (tile_idx >= num_tiles) {
        return;
    }

    extern __shared__ uint32_t smem_words[];
    uint8_t* const smem = reinterpret_cast<uint8_t*>(smem_words);

    uint32_t* const bitmap = reinterpret_cast<uint32_t*>(smem);
    uint8_t* const overlay = smem + kBitmapBytes;

    uint32_t* const cand_list = reinterpret_cast<uint32_t*>(overlay);

    uint16_t* const row_prefix = reinterpret_cast<uint16_t*>(overlay);
    uint32_t* const prime_pos = reinterpret_cast<uint32_t*>(row_prefix + (ACTIVE_ROWS + 1));
    uint16_t* const parent = reinterpret_cast<uint16_t*>(prime_pos + MAX_PRIMES_GPU);
    FacePrimeGPU* const face_prime_lists = reinterpret_cast<FacePrimeGPU*>(parent + MAX_PRIMES_GPU);
    uint32_t* const face_prime_counts = reinterpret_cast<uint32_t*>(face_prime_lists + NUM_FACES * MAX_FACE_PRIMES_PER_FACE);
    FaceScratchGPU* const face_scratch = reinterpret_cast<FaceScratchGPU*>(smem);

    __shared__ uint32_t total_cands;
#ifdef PROFILE_PHASES
    __shared__ uint64_t phase_t0;
    __shared__ uint64_t phase_t1;
    __shared__ uint64_t phase_t2;
    __shared__ uint64_t phase_t3;
    __shared__ uint64_t phase_t4;
    __shared__ uint64_t phase_t5;
    __shared__ uint64_t phase_t6;
#endif

    const int tid = static_cast<int>(threadIdx.x);
    const TileCoord coord = coords[tile_idx];
    const int32_t a_start = static_cast<int32_t>(coord.a_lo - static_cast<int64_t>(COLLAR));
    const int32_t b_start = static_cast<int32_t>(coord.b_lo - static_cast<int64_t>(COLLAR));

    for (int i = tid; i < kBitmapWords; i += BLOCK_THREADS) {
        bitmap[i] = 0U;
    }
    __syncthreads();
#ifdef PROFILE_PHASES
    if (tid == 0) {
        phase_t0 = clock64();
    }
#endif

    if (tid == 0) {
        total_cands = 0U;
    }
    __syncthreads();

    if (tid < ACTIVE_ROWS) {
        uint32_t ws[BITMAP_WORDS_PER_ROW];
        const int32_t a = a_start + tid;
        sieve_row(ws, a, b_start);

        const uint32_t count = count_sieve_survivors(ws);
        if (count > 0U) {
            const uint32_t base = atomicAdd(&total_cands, count);
            if (base + count <= static_cast<uint32_t>(MAX_CANDIDATES_GPU)) {
                scatter_survivors(ws, cand_list, static_cast<int>(base), tid);
            } else if (base < static_cast<uint32_t>(MAX_CANDIDATES_GPU)) {
                scatter_survivors_clamped(ws, cand_list, static_cast<int>(base),
                                         static_cast<int>(static_cast<uint32_t>(MAX_CANDIDATES_GPU) - base), tid);
            }
        }
    }
    __syncthreads();

#ifdef PROFILE_PHASES
    if (tid == 0) {
        phase_t1 = clock64();
    }
#endif

    if (tid == 0) {
        if (total_cands > static_cast<uint32_t>(MAX_CANDIDATES_GPU)) {
            total_cands = static_cast<uint32_t>(MAX_CANDIDATES_GPU);
        }
    }
    __syncthreads();
#ifdef PROFILE_PHASES
    // phase1b is eliminated — set to zero for ABI stability
    if (tid == 0) {
        phase_t2 = clock64();
    }
#endif

    mr_test_candidates(cand_list, static_cast<int>(total_cands), a_start, b_start,
                       bitmap, tid, BLOCK_THREADS);
    __syncthreads();
#ifdef PROFILE_PHASES
    if (tid == 0) {
        phase_t3 = clock64();
    }
#endif

    (void)compact_row(bitmap, row_prefix, prime_pos, tid);
    __syncthreads();
#ifdef PROFILE_PHASES
    if (tid == 0) {
        phase_t4 = clock64();
    }
#endif

    const int prime_count = static_cast<int>(row_prefix[ACTIVE_ROWS]);
    if (prime_count > MAX_PRIMES_GPU) {
        if (tid == 0) {
            poison_tileop(&output[tile_idx]);
            if (prime_counts != nullptr) {
                prime_counts[tile_idx] = static_cast<uint32_t>(prime_count);
            }
#ifdef PROFILE_PHASES
            const uint64_t now = clock64();
            PhaseTimingGPU timing{};
            timing.phase1a_cycles = static_cast<int64_t>(phase_t1 - phase_t0);
            timing.phase1b_cycles = static_cast<int64_t>(phase_t2 - phase_t1);
            timing.phase1c_cycles = static_cast<int64_t>(phase_t3 - phase_t2);
            timing.phase2_cycles = static_cast<int64_t>(phase_t4 - phase_t3);
            timing.phase3_cycles = 0;
            timing.phase45_cycles = 0;
            timing.total_cycles = static_cast<int64_t>(now - phase_t0);
            timing.tile_idx = tile_idx;
            timing.prime_count = prime_count;
            phase_timings[tile_idx] = timing;
#endif
        }
        return;
    }

    if (tid < 32) {
        build_components_gpu(bitmap, row_prefix, prime_pos, prime_count, parent, tid);
    }
    __syncthreads();
#ifdef PROFILE_PHASES
    if (tid == 0) {
        phase_t5 = clock64();
    }
#endif

    __shared__ FaceDataGPU face_data;

    extract_faces_gpu_parallel(prime_pos, prime_count, parent, face_prime_lists, face_prime_counts, face_scratch, tid);
    prune_dead_ends_gpu(face_scratch, &face_data, tid);

    if (tid == 0) {
        encode_tileop_gpu(&face_data, &output[tile_idx]);
        if (prime_counts != nullptr) {
            prime_counts[tile_idx] = static_cast<uint32_t>(prime_count);
        }
#ifdef PROFILE_PHASES
        phase_t6 = clock64();

        PhaseTimingGPU timing{};
        timing.phase1a_cycles = static_cast<int64_t>(phase_t1 - phase_t0);
        timing.phase1b_cycles = static_cast<int64_t>(phase_t2 - phase_t1);
        timing.phase1c_cycles = static_cast<int64_t>(phase_t3 - phase_t2);
        timing.phase2_cycles = static_cast<int64_t>(phase_t4 - phase_t3);
        timing.phase3_cycles = static_cast<int64_t>(phase_t5 - phase_t4);
        timing.phase45_cycles = static_cast<int64_t>(phase_t6 - phase_t5);
        timing.total_cycles = static_cast<int64_t>(phase_t6 - phase_t0);
        timing.tile_idx = tile_idx;
        timing.prime_count = prime_count;
        phase_timings[tile_idx] = timing;
#endif
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
    check_cuda(cudaMemcpyToSymbol(c_mr_witnesses, kMrWitnesses, sizeof(kMrWitnesses)),
               "cudaMemcpyToSymbol(c_mr_witnesses)");
    check_cuda(cudaMemcpyToSymbol(c_trial_primes, kTrialPrimes, sizeof(kTrialPrimes)),
               "cudaMemcpyToSymbol(c_trial_primes)");
}

size_t tile_kernel_shared_bytes() {
    const size_t overlay_bytes = kPhase1Bytes > kPhase24Bytes ? kPhase1Bytes : kPhase24Bytes;
    return kBitmapBytes + overlay_bytes;
}
