#pragma once

#include <cstdint>

#include "campaign/constants.h"

namespace cuda_campaign {

inline constexpr int k_sq_value = campaign::k_sq_value;
inline constexpr int S = campaign::S;
inline constexpr int TILEOP_SIZE = campaign::TILEOP_SIZE;
inline constexpr int OFFSET_X = campaign::OFFSET_X;
inline constexpr int OFFSET_Y = campaign::OFFSET_Y;
inline constexpr int MAX_PRIMES_GPU = campaign::MAX_PRIMES_GPU;
inline constexpr int MAX_GROUPS_PER_TILE = campaign::MAX_GROUPS_PER_TILE;
inline constexpr int MAX_PORTS_PER_TILE = campaign::MAX_PORTS_PER_TILE;
inline constexpr int C = campaign::C;
inline constexpr int HALO = campaign::HALO;
inline constexpr int COLLAR = campaign::COLLAR;
inline constexpr int SIDE_EXP = campaign::SIDE_EXP;
inline constexpr int NUM_FACES = campaign::NUM_FACES;

inline constexpr std::uint8_t OVERFLOW_BIT = campaign::OVERFLOW_BIT;
inline constexpr std::uint8_t EMPTY_BIT = campaign::EMPTY_BIT;
inline constexpr std::uint8_t TOWER_CLOSING_BIT = campaign::TOWER_CLOSING_BIT;

enum class Face : std::uint8_t {
  I = 0,
  O = 1,
  L = 2,
  R = 3,
};

}  // namespace cuda_campaign
