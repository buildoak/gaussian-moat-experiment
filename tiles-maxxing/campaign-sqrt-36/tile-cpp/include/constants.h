#pragma once
#include <cstdint>

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
}  // namespace detail

// Tile geometry
constexpr int32_t TILE_SIDE   = 256;
constexpr int32_t K_SQ        = K_SQ_VAL;                        // connectivity threshold (distance^2)
constexpr int32_t COLLAR      = detail::ceil_isqrt(K_SQ);        // ceil(sqrt(K_SQ))
constexpr int32_t TILE_POINTS = TILE_SIDE + 1;                   // 257 lattice points per axis
constexpr int32_t SIDE_EXP    = TILE_POINTS + 2 * COLLAR;        // expanded domain side

// Sieve parameters
constexpr uint32_t SIEVE_LIMIT = 10000;
constexpr uint32_t SIEVE_SQRT  = 100;     // sqrt(SIEVE_LIMIT)
constexpr int      SPLIT_PRIMES_COUNT = 609; // p = 1 mod 4 below SIEVE_LIMIT
constexpr int      INERT_PRIMES_COUNT = 619; // p = 3 mod 4 below SIEVE_LIMIT

// Bitmap dimensions
constexpr int BITMAP_BITS  = SIDE_EXP * SIDE_EXP;      // 73,441
constexpr int BITMAP_WORDS = (BITMAP_BITS + 31) / 32;  // 2,296

// TileOp encoding
constexpr int TILEOP_SIZE          = 256;
constexpr int TILEOP_HEADER_BYTES  = 3;
constexpr int TILEOP_PAYLOAD_BYTES = TILEOP_SIZE - TILEOP_HEADER_BYTES;
constexpr uint8_t EMPTY_OFFSET     = 3;
constexpr uint8_t OVERFLOW_SENTINEL = 0xFF;

// Memory bounds
constexpr int MAX_PRIMES = 16384;  // safe upper bound per 271x271 bitmap at the operating point
constexpr int MAX_PORTS  = 2048;   // safe upper bound for raw face-prime clustering buffers

// Face indices
constexpr int FACE_I = 0;  // inner (bottom)
constexpr int FACE_O = 1;  // outer (top)
constexpr int FACE_L = 2;  // left
constexpr int FACE_R = 3;  // right
constexpr int NUM_FACES = 4;
