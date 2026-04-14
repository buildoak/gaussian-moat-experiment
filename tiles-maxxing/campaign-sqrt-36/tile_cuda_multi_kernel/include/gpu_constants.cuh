#pragma once

#include <cstdint>
#include <cuda_runtime.h>

// ---------------------------------------------------------------------------
// K_SQ parameterization: pass -DK_SQ_VAL=N at compile time, default 40
// All dependent geometry constants are auto-derived via constexpr.
// ---------------------------------------------------------------------------
#ifndef K_SQ_VAL
#define K_SQ_VAL 40
#endif

namespace detail {
constexpr int32_t ceil_isqrt(int32_t n) {
    if (n <= 0) return 0;
    int32_t x = 1;
    while (x * x < n) ++x;
    return x;
}
constexpr int count_backward_offsets(int32_t k_sq) {
    int32_t reach = ceil_isqrt(k_sq);
    int count = 0;
    for (int32_t dr = -reach; dr <= 0; ++dr)
        for (int32_t dc = -reach; dc <= reach; ++dc) {
            if ((dr > 0) || (dr == 0 && dc >= 0)) continue;
            if (dr * dr + dc * dc > k_sq) continue;
            ++count;
        }
    return count;
}
}  // namespace detail

constexpr int32_t TILE_SIDE = 256;
constexpr int32_t K_SQ = K_SQ_VAL;
constexpr int32_t COLLAR = detail::ceil_isqrt(K_SQ);
constexpr int32_t TILE_POINTS = TILE_SIDE + 1;
constexpr int32_t SIDE_EXP = TILE_POINTS + 2 * COLLAR;
static_assert(SIDE_EXP <= 512, "packed prime positions require SIDE_EXP <= 512");

constexpr int SPLIT_PRIMES_COUNT = 609;
constexpr int INERT_PRIMES_COUNT = 619;
constexpr uint32_t SIEVE_LIMIT = 10000U;
constexpr uint32_t SIEVE_SQRT = 100U;

constexpr int BITMAP_WORDS_PER_ROW = (SIDE_EXP + 31) / 32;
constexpr int BITMAP_ROWS = SIDE_EXP;
constexpr int LAST_WORD_VALID_BITS = SIDE_EXP % 32;
static_assert(LAST_WORD_VALID_BITS != 0, "SIDE_EXP divisible by 32 needs LAST_WORD_MASK=0xFFFFFFFF logic");
constexpr uint32_t LAST_WORD_MASK = (1u << LAST_WORD_VALID_BITS) - 1u;

constexpr int MAX_PRIMES_GPU = 2560;
constexpr int MAX_PORTS_GPU = 256;
constexpr int MAX_FACE_PRIMES_GPU = 900;
constexpr int MAX_FACE_PRIMES_PER_FACE = 256;
constexpr int MAX_FACE_PORTS_GPU = 48;
constexpr int MAX_TOTAL_PORTS_GPU = 192;
constexpr int MAX_GROUPS_GPU = 127;
constexpr int FACES_PER_PASS = 2;

constexpr int TILEOP_SIZE = 256;
constexpr int TILEOP_HEADER_BYTES = 3;
constexpr int TILEOP_PAYLOAD_BYTES = 253;
constexpr uint8_t EMPTY_OFFSET = 3;
constexpr uint8_t OVERFLOW_SENTINEL = 0xFFu;

constexpr int FACE_I = 0;
constexpr int FACE_O = 1;
constexpr int FACE_L = 2;
constexpr int FACE_R = 3;
constexpr int NUM_FACES = 4;

constexpr int BLOCK_THREADS = 288;
constexpr int ACTIVE_ROWS = SIDE_EXP;
constexpr int WARP_SIZE = 32;

constexpr int NUM_BACKWARD_OFFSETS = detail::count_backward_offsets(K_SQ);

constexpr int NUM_MR_WITNESSES = 7;
constexpr int NUM_TRIAL_PRIMES = 24;

// Multi-kernel: max sieve candidates per tile
constexpr int MAX_CANDIDATES_GPU = 6144;
static_assert(MAX_CANDIDATES_GPU >= MAX_PRIMES_GPU,
              "candidate list must hold at least MAX_PRIMES_GPU entries");

// Bitmap sizing
constexpr int BITMAP_WORDS = ACTIVE_ROWS * BITMAP_WORDS_PER_ROW;

// Compile-time sanity checks for known-good K_SQ configurations
static_assert(K_SQ != 40 || (COLLAR == 7 && SIDE_EXP == 271 && NUM_BACKWARD_OFFSETS == 64
              && LAST_WORD_VALID_BITS == 15), "K_SQ=40 derived constants mismatch");
static_assert(K_SQ != 36 || (COLLAR == 6 && SIDE_EXP == 269 && NUM_BACKWARD_OFFSETS == 56
              && LAST_WORD_VALID_BITS == 13), "K_SQ=36 derived constants mismatch");
