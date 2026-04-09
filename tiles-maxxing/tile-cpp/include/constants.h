#pragma once
#include <cstdint>

// Tile geometry
constexpr int32_t TILE_SIDE   = 256;
constexpr int32_t COLLAR      = 7;
constexpr int32_t TILE_POINTS = TILE_SIDE + 1;            // 257 lattice points per axis
constexpr int32_t SIDE_EXP    = TILE_POINTS + 2 * COLLAR; // 271 = 257 tile points + 14 collar
constexpr int32_t K_SQ        = 40;                       // connectivity threshold (distance^2)

// Sieve parameters
constexpr uint32_t SIEVE_LIMIT = 10000;
constexpr uint32_t SIEVE_SQRT  = 100;     // sqrt(SIEVE_LIMIT)
constexpr int      SPLIT_PRIMES_COUNT = 609; // p = 1 mod 4 below SIEVE_LIMIT
constexpr int      INERT_PRIMES_COUNT = 619; // p = 3 mod 4 below SIEVE_LIMIT

// Bitmap dimensions
constexpr int BITMAP_BITS  = SIDE_EXP * SIDE_EXP;      // 73,441
constexpr int BITMAP_WORDS = (BITMAP_BITS + 31) / 32;  // 2,296

// TileOp encoding
constexpr int TILEOP_SIZE    = 128;
constexpr int PORTS_PER_FACE = 16;
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
