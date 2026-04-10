#pragma once

#include <cstdint>
#include <cuda_runtime.h>

constexpr int32_t TILE_SIDE = 256;
constexpr int32_t COLLAR = 7;
constexpr int32_t TILE_POINTS = TILE_SIDE + 1;
constexpr int32_t SIDE_EXP = TILE_POINTS + 2 * COLLAR;
constexpr int32_t K_SQ = 40;
static_assert(SIDE_EXP <= 512, "packed prime positions require SIDE_EXP <= 512");

constexpr int SPLIT_PRIMES_COUNT = 609;
constexpr int INERT_PRIMES_COUNT = 619;
constexpr uint32_t SIEVE_LIMIT = 10000U;
constexpr uint32_t SIEVE_SQRT = 100U;

constexpr int BITMAP_WORDS_PER_ROW = 9;
constexpr int BITMAP_ROWS = SIDE_EXP;
constexpr int LAST_WORD_VALID_BITS = 15;
constexpr uint32_t LAST_WORD_MASK = (1u << LAST_WORD_VALID_BITS) - 1u;

constexpr int MAX_PRIMES_GPU = 3072;
constexpr int MAX_PORTS_GPU = 256;
constexpr int MAX_FACE_PRIMES_GPU = 900;
constexpr int MAX_FACE_PORTS_GPU = 32;
constexpr int MAX_TOTAL_PORTS_GPU = 128;
constexpr int MAX_GROUPS_GPU = 127;

constexpr int TILEOP_SIZE = 128;
constexpr int TILEOP_HEADER_BYTES = 3;
constexpr int TILEOP_PAYLOAD_BYTES = 125;
constexpr uint8_t EMPTY_OFFSET = 3;
constexpr uint8_t OVERFLOW_SENTINEL = 0xFFu;

constexpr int FACE_I = 0;
constexpr int FACE_O = 1;
constexpr int FACE_L = 2;
constexpr int FACE_R = 3;
constexpr int NUM_FACES = 4;

constexpr int BLOCK_THREADS = 288;
constexpr int ACTIVE_ROWS = 271;
constexpr int WARP_SIZE = 32;

constexpr int NUM_BACKWARD_OFFSETS = 64;

constexpr int NUM_MR_WITNESSES = 12;
constexpr int NUM_TRIAL_PRIMES = 24;
