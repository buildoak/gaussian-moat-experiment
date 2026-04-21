#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "cuda_campaign/constants.cuh"

namespace cuda_campaign {

struct TileOp {
  std::uint8_t n[4];
  std::uint8_t face_groups[192];
  std::uint8_t inner_flags[16];
  std::uint8_t outer_flags[16];
  std::uint8_t tile_flags;
  std::uint8_t reserved[27];
};

static_assert(std::is_standard_layout<TileOp>::value,
              "TileOp must be standard-layout");
static_assert(sizeof(TileOp) == TILEOP_SIZE,
              "TileOp must be exactly 256 bytes");
static_assert(sizeof(TileOp) == 256,
              "TileOp wire size is locked at 256");
static_assert(offsetof(TileOp, n) == 0, "n[4] offset must be 0");
static_assert(offsetof(TileOp, face_groups) == 4,
              "face_groups offset must be 4");
static_assert(offsetof(TileOp, inner_flags) == 196,
              "inner_flags offset must be 196");
static_assert(offsetof(TileOp, outer_flags) == 212,
              "outer_flags offset must be 212");
static_assert(offsetof(TileOp, tile_flags) == 228,
              "tile_flags offset must be 228");
static_assert(offsetof(TileOp, reserved) == 229,
              "reserved offset must be 229");

}  // namespace cuda_campaign
