#pragma once

#include <cstdint>

#include "campaign/constants.h"

namespace cuda_campaign {

inline constexpr int k_sq_value = campaign::k_sq_value;
inline constexpr int S = campaign::S;
inline constexpr int TILEOP_SIZE = campaign::TILEOP_SIZE;
inline constexpr int OFFSET_X = campaign::OFFSET_X;
inline constexpr int OFFSET_Y = campaign::OFFSET_Y;
// Override CPU's 6144 — with fixed K1, tiles can have up to ~8000 primes
inline constexpr int MAX_PRIMES_GPU = 8192;
inline constexpr int MAX_GROUPS_PER_TILE = campaign::MAX_GROUPS_PER_TILE;
inline constexpr int MAX_PORTS_PER_TILE = campaign::MAX_PORTS_PER_TILE;
inline constexpr int C = campaign::C;
inline constexpr int HALO = campaign::HALO;
inline constexpr int COLLAR = campaign::COLLAR;
inline constexpr int SIDE_EXP = campaign::SIDE_EXP;
inline constexpr int NUM_FACES = campaign::NUM_FACES;

inline constexpr int SPLIT_PRIMES_COUNT = 609;
inline constexpr int INERT_PRIMES_COUNT = 619;
inline constexpr int SIEVE_LIMIT = 10000;

inline constexpr int BITMAP_ROWS = SIDE_EXP;
inline constexpr int BITMAP_WORDS_PER_ROW = (SIDE_EXP + 31) / 32;
inline constexpr int LAST_WORD_VALID_BITS = SIDE_EXP % 32;
static_assert(LAST_WORD_VALID_BITS != 0,
              "SIDE_EXP divisible by 32 needs LAST_WORD_MASK=0xFFFFFFFF logic");
inline constexpr std::uint32_t LAST_WORD_MASK =
    (std::uint32_t{1} << LAST_WORD_VALID_BITS) - 1U;
inline constexpr int BITMAP_WORDS = BITMAP_ROWS * BITMAP_WORDS_PER_ROW;

inline constexpr int MAX_CANDIDATES_GPU = 16384;
inline constexpr int BLOCK_THREADS = 288;
inline constexpr int ACTIVE_ROWS = SIDE_EXP;

inline constexpr int NUM_TRIAL_PRIMES = 12;
inline constexpr int FJ64_TABLE_SIZE = 262144;

constexpr int count_backward_offsets(int k_sq) noexcept {
  const int reach = C;
  int count = 0;
  for (int dr = -reach; dr <= 0; ++dr) {
    for (int dc = -reach; dc <= reach; ++dc) {
      if ((dr > 0) || (dr == 0 && dc >= 0)) continue;
      if (dr * dr + dc * dc > k_sq) continue;
      ++count;
    }
  }
  return count;
}

inline constexpr int NUM_BACKWARD_OFFSETS = count_backward_offsets(k_sq_value);

inline constexpr std::uint8_t OVERFLOW_BIT = campaign::OVERFLOW_BIT;
inline constexpr std::uint8_t EMPTY_BIT = campaign::EMPTY_BIT;
inline constexpr std::uint8_t TOWER_CLOSING_BIT = campaign::TOWER_CLOSING_BIT;

enum class Face : std::uint8_t {
  I = 0,
  O = 1,
  L = 2,
  R = 3,
};

static_assert(k_sq_value != 36 || (C == 6 && SIDE_EXP == 269 &&
                                   NUM_BACKWARD_OFFSETS == 56),
              "K_SQ=36 CUDA constants mismatch");
static_assert(k_sq_value != 40 || (C == 6 && SIDE_EXP == 269 &&
                                   NUM_BACKWARD_OFFSETS == 64),
              "K_SQ=40 CUDA constants mismatch");
static_assert(COLLAR == C, "CUDA collar must use v2 floor_isqrt C");
static_assert(MAX_PRIMES_GPU >= 6144, "CUDA prime capacity must be at least 6144");
static_assert(MAX_CANDIDATES_GPU >= MAX_PRIMES_GPU,
              "K1 candidate capacity must cover at least compacted primes");

}  // namespace cuda_campaign
