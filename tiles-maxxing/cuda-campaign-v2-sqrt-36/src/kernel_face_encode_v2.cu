#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace cuda_campaign {
namespace {

__device__ __forceinline__ void write_flag_tileop(TileOp* out,
                                                  std::uint8_t flag) {
  std::uint8_t* bytes = reinterpret_cast<std::uint8_t*>(out);
  for (int i = 0; i < TILEOP_SIZE; ++i) {
    bytes[i] = 0;
  }
  out->tile_flags = flag;
}

__device__ __forceinline__ std::int64_t face_perp_from_packed(
    std::uint32_t packed,
    Face face) {
  const std::int64_t row = static_cast<std::int64_t>(packed / SIDE_EXP);
  const std::int64_t col = static_cast<std::int64_t>(packed % SIDE_EXP);
  const std::int64_t rel_row = row - C;
  const std::int64_t rel_col = col - C;

  switch (face) {
    case Face::I:
      return rel_row;
    case Face::O:
      return rel_row - S;
    case Face::L:
      return rel_col;
    case Face::R:
      return rel_col - S;
  }
  return 0;
}

__device__ __forceinline__ bool on_face_strip(std::uint32_t packed,
                                              Face face) {
  const std::int64_t p_perp = face_perp_from_packed(packed, face);
  return -static_cast<std::int64_t>(C) <= p_perp &&
         p_perp <= static_cast<std::int64_t>(C);
}

__device__ void build_face_indices_warp(
    const std::uint32_t* __restrict__ tile_prime_pos,
    int prime_count,
    Face face,
    std::uint16_t* __restrict__ face_indices,
    std::uint16_t* __restrict__ face_count) {
  const int lane = static_cast<int>(threadIdx.x) & 31;
  int count = 0;

  for (int base = 0; base < prime_count; base += 32) {
    const int idx = base + lane;
    const bool keep = idx < prime_count &&
                      on_face_strip(tile_prime_pos[idx], face);
    const unsigned mask = __ballot_sync(0xffffffffu, keep);
    if (keep) {
      const unsigned lane_mask = (1u << lane) - 1u;
      face_indices[count + __popc(mask & lane_mask)] =
          static_cast<std::uint16_t>(idx);
    }
    count += __popc(mask);
  }

  if (lane == 0) {
    *face_count = static_cast<std::uint16_t>(count);
  }
}

__global__ void kernel_face_encode_v2(
    const campaign::TileCoord* __restrict__ d_coords,
    const std::uint32_t* __restrict__ d_prime_pos,
    const std::uint32_t* __restrict__ d_prime_count,
    const std::uint8_t* __restrict__ d_remap_overflow,
    TileOp* __restrict__ d_tileops,
    std::uint16_t* __restrict__ d_face_indices,
    std::uint16_t* __restrict__ d_face_counts,
    int num_tiles) {
  const int tile_idx = static_cast<int>(blockIdx.x);
  if (tile_idx >= num_tiles) return;

  const int tid = static_cast<int>(threadIdx.x);
  const std::uint32_t prime_count = d_prime_count[tile_idx];
  const bool remap_overflow =
      d_remap_overflow != nullptr && d_remap_overflow[tile_idx] != 0;
  TileOp* out = d_tileops + static_cast<std::size_t>(tile_idx);

  if (prime_count == 0) {
    if (tid == 0) {
      write_flag_tileop(out, EMPTY_BIT);
    }
    return;
  }

  if (remap_overflow) {
    if (tid == 0) {
      write_flag_tileop(out, OVERFLOW_BIT);
    }
    return;
  }

  if (tid == 0) {
    write_flag_tileop(out, 0);
  }

  const int bounded = prime_count < MAX_PRIMES_GPU
                          ? static_cast<int>(prime_count)
                          : MAX_PRIMES_GPU;
  const std::uint32_t* tile_prime_pos =
      d_prime_pos + static_cast<std::size_t>(tile_idx) * MAX_PRIMES_GPU;
  std::uint16_t* tile_face_indices =
      d_face_indices == nullptr
          ? nullptr
          : d_face_indices + static_cast<std::size_t>(tile_idx) * NUM_FACES *
                                 FACE_INDEX_STRIDE;
  std::uint16_t* tile_face_counts =
      d_face_counts == nullptr
          ? nullptr
          : d_face_counts + static_cast<std::size_t>(tile_idx) *
                                FACE_COUNT_STRIDE;

  if (tile_face_indices == nullptr || tile_face_counts == nullptr) {
    return;
  }

  const int warp = tid >> 5;
  if (warp < NUM_FACES) {
    build_face_indices_warp(
        tile_prime_pos, bounded, static_cast<Face>(warp),
        tile_face_indices + warp * FACE_INDEX_STRIDE,
        tile_face_counts + warp);
  }

  (void)d_coords;
}

}  // namespace

void launch_kernel_face_encode_v2(const FaceEncodeBuffers& buffers,
                                  int num_tiles,
                                  cudaStream_t stream) {
  kernel_face_encode_v2<<<num_tiles, BLOCK_THREADS, 0, stream>>>(
      buffers.in.d_coords, buffers.in.d_prime_pos, buffers.in.d_prime_count,
      buffers.in.d_remap_overflow, buffers.d_tileops,
      buffers.debug.d_face_indices, buffers.debug.d_face_counts, num_tiles);
}

}  // namespace cuda_campaign
