#pragma once

#include <cstdint>

#include "campaign/grid.h"
#include "cuda_campaign/constants.cuh"
#include "cuda_campaign/tileop.cuh"

namespace cuda_campaign {

inline constexpr int FACE_INDEX_STRIDE = MAX_PRIMES_GPU;
inline constexpr int FACE_COUNT_STRIDE = NUM_FACES;

struct FaceEncodeInputBuffers {
  const campaign::TileCoord* d_coords = nullptr;       // [N]
  const std::uint32_t* d_prime_pos = nullptr;           // [N * MAX_PRIMES_GPU]
  const std::uint32_t* d_prime_count = nullptr;         // [N]
  const std::uint8_t* d_remap_overflow = nullptr;       // [N], optional
};

struct FaceEncodeDebugBuffers {
  std::uint16_t* d_face_indices = nullptr;  // [N * NUM_FACES * MAX_PRIMES_GPU]
  std::uint16_t* d_face_counts = nullptr;   // [N * NUM_FACES]
};

struct FaceEncodeBuffers {
  FaceEncodeInputBuffers in;
  TileOp* d_tileops = nullptr;  // [N]
  FaceEncodeDebugBuffers debug;
};

}  // namespace cuda_campaign
