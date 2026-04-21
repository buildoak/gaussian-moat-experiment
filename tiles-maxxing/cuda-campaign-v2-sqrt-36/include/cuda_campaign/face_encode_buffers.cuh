#pragma once

#include <cstdint>

#include "campaign/grid.h"
#include "cuda_campaign/constants.cuh"
#include "cuda_campaign/tileop.cuh"

namespace cuda_campaign {

inline constexpr int FACE_INDEX_STRIDE = MAX_PRIMES_GPU;
inline constexpr int FACE_COUNT_STRIDE = NUM_FACES;
inline constexpr int FACE_ROOT_STRIDE = MAX_PRIMES_GPU;
inline constexpr int FACE_REP_STRIDE = MAX_PRIMES_GPU;

struct FaceRepresentative {
  std::int16_t h = 0;
  std::int16_t p_perp = 0;
  std::uint16_t prime_index = 0;
  std::uint8_t global_wire_label = 0;
  std::uint8_t reserved = 0;
};

static_assert(sizeof(FaceRepresentative) == 8,
              "FaceRepresentative debug ABI must stay compact");

struct FaceEncodeInputBuffers {
  const campaign::TileCoord* d_coords = nullptr;       // [N]
  const std::uint32_t* d_prime_pos = nullptr;           // [N * MAX_PRIMES_GPU]
  const std::uint32_t* d_prime_count = nullptr;         // [N]
  const std::uint8_t* d_remap_overflow = nullptr;       // [N], optional
  const std::uint16_t* d_parent = nullptr;              // [N * MAX_PRIMES_GPU], optional
  const std::uint16_t* d_wire_label_by_raw_root = nullptr; // [N * MAX_PRIMES_GPU], optional
};

struct FaceEncodeDebugBuffers {
  std::uint16_t* d_face_indices = nullptr;  // [N * NUM_FACES * MAX_PRIMES_GPU]
  std::uint16_t* d_face_counts = nullptr;   // [N * NUM_FACES]
  std::uint16_t* d_face_roots = nullptr;    // [N * NUM_FACES * MAX_PRIMES_GPU]
  FaceRepresentative* d_face_reps = nullptr; // [N * NUM_FACES * MAX_PRIMES_GPU]
  std::uint16_t* d_face_rep_counts = nullptr; // [N * NUM_FACES]
};

struct FaceEncodeBuffers {
  FaceEncodeInputBuffers in;
  TileOp* d_tileops = nullptr;  // [N]
  FaceEncodeDebugBuffers debug;
};

}  // namespace cuda_campaign
