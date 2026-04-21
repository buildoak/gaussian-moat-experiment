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

__device__ __forceinline__ void bit_set(std::uint8_t* flags,
                                        int group_label_1based) {
  const int g0 = group_label_1based - 1;
  flags[g0 >> 3] |= static_cast<std::uint8_t>(1u << (g0 & 7));
}

__device__ __forceinline__ void unpack_group_flags(
    TileOp* out,
    const std::uint8_t* __restrict__ tile_group_flags) {
  if (tile_group_flags == nullptr) return;
  if (threadIdx.x != 0) return;
  for (int g0 = 0; g0 < MAX_GROUPS_PER_TILE; ++g0) {
    const std::uint8_t bits = tile_group_flags[g0] & 0x3U;
    const int label = g0 + 1;
    if ((bits & 0x1U) != 0) {
      bit_set(out->inner_flags, label);
    }
    if ((bits & 0x2U) != 0) {
      bit_set(out->outer_flags, label);
    }
  }
}

__device__ __forceinline__ void zero_face_debug_counts(
    std::uint16_t* __restrict__ tile_face_counts,
    std::uint16_t* __restrict__ tile_face_rep_counts) {
  if (threadIdx.x < NUM_FACES) {
    if (tile_face_counts != nullptr) {
      tile_face_counts[threadIdx.x] = 0;
    }
    if (tile_face_rep_counts != nullptr) {
      tile_face_rep_counts[threadIdx.x] = 0;
    }
  }
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

__device__ __forceinline__ std::int64_t face_h_from_packed(std::uint32_t packed,
                                                           Face face) {
  const std::int64_t row = static_cast<std::int64_t>(packed / SIDE_EXP);
  const std::int64_t col = static_cast<std::int64_t>(packed % SIDE_EXP);
  const std::int64_t rel_row = row - C;
  const std::int64_t rel_col = col - C;

  switch (face) {
    case Face::I:
    case Face::O:
      return rel_col;
    case Face::L:
    case Face::R:
      return rel_row;
  }
  return 0;
}

__device__ __forceinline__ bool within_k_sq_packed(std::uint32_t lhs,
                                                   std::uint32_t rhs) {
  const std::int64_t lhs_row = static_cast<std::int64_t>(lhs / SIDE_EXP);
  const std::int64_t lhs_col = static_cast<std::int64_t>(lhs % SIDE_EXP);
  const std::int64_t rhs_row = static_cast<std::int64_t>(rhs / SIDE_EXP);
  const std::int64_t rhs_col = static_cast<std::int64_t>(rhs % SIDE_EXP);
  const std::int64_t dr = lhs_row - rhs_row;
  const std::int64_t dc = lhs_col - rhs_col;
  return dr * dr + dc * dc <= static_cast<std::int64_t>(k_sq_value);
}

__device__ __forceinline__ std::uint16_t face_dsu_find(
    std::uint16_t* parent,
    std::uint16_t x) {
  std::uint16_t p = x;
  while (p != parent[p]) {
    parent[p] = parent[parent[p]];
    p = parent[p];
  }
  return p;
}

__device__ __forceinline__ void face_dsu_unite(std::uint16_t* parent,
                                               std::uint16_t a,
                                               std::uint16_t b) {
  std::uint16_t ra = face_dsu_find(parent, a);
  std::uint16_t rb = face_dsu_find(parent, b);
  if (ra == rb) return;
  if (ra > rb) {
    const std::uint16_t tmp = ra;
    ra = rb;
    rb = tmp;
  }
  parent[rb] = ra;
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

__device__ void build_face_dsu_and_reps(
    const std::uint32_t* __restrict__ tile_prime_pos,
    const std::uint16_t* __restrict__ tile_parent,
    const std::uint16_t* __restrict__ tile_wire_label_by_raw_root,
    Face face,
    const std::uint16_t* __restrict__ face_indices,
    std::uint16_t face_count,
    std::uint16_t* __restrict__ face_roots,
    FaceRepresentative* __restrict__ face_reps,
    std::uint16_t* __restrict__ face_rep_count) {
  for (std::uint16_t i = 0; i < face_count; ++i) {
    face_roots[i] = i;
  }

  for (std::uint16_t i = 0; i < face_count; ++i) {
    for (std::uint16_t j = static_cast<std::uint16_t>(i + 1); j < face_count;
         ++j) {
      const std::uint32_t lhs = tile_prime_pos[face_indices[i]];
      const std::uint32_t rhs = tile_prime_pos[face_indices[j]];
      if (within_k_sq_packed(lhs, rhs)) {
        face_dsu_unite(face_roots, i, j);
      }
    }
  }

  for (std::uint16_t i = 0; i < face_count; ++i) {
    face_roots[i] = face_dsu_find(face_roots, i);
  }

  std::uint16_t rep_count = 0;
  for (std::uint16_t root = 0; root < face_count; ++root) {
    bool have_rep = false;
    std::int64_t best_h = 0;
    std::int64_t best_perp = 0;
    std::uint16_t best_prime_idx = 0;
    std::uint8_t label = 0;

    for (std::uint16_t k = 0; k < face_count; ++k) {
      if (face_roots[k] != root) continue;
      const std::uint16_t prime_idx = face_indices[k];
      const std::uint32_t packed = tile_prime_pos[prime_idx];
      const std::int64_t h = face_h_from_packed(packed, face);
      const std::int64_t p_perp = face_perp_from_packed(packed, face);
      if (!have_rep || h < best_h || (h == best_h && p_perp < best_perp)) {
        have_rep = true;
        best_h = h;
        best_perp = p_perp;
        best_prime_idx = prime_idx;
        label = 0;
        if (tile_parent != nullptr && tile_wire_label_by_raw_root != nullptr) {
          const std::uint16_t raw_root = tile_parent[prime_idx];
          const std::uint16_t wide_label =
              tile_wire_label_by_raw_root[raw_root];
          label = static_cast<std::uint8_t>(wide_label & 0xffU);
        }
      }
    }

    if (have_rep) {
      face_reps[rep_count] = FaceRepresentative{
          static_cast<std::int16_t>(best_h),
          static_cast<std::int16_t>(best_perp),
          best_prime_idx,
          label,
          0};
      ++rep_count;
    }
  }

  *face_rep_count = rep_count;
}

__global__ void kernel_face_encode_v2(
    const campaign::TileCoord* __restrict__ d_coords,
    const std::uint32_t* __restrict__ d_prime_pos,
    const std::uint32_t* __restrict__ d_prime_count,
    const std::uint8_t* __restrict__ d_remap_overflow,
    const std::uint16_t* __restrict__ d_parent,
    const std::uint16_t* __restrict__ d_wire_label_by_raw_root,
    const std::uint8_t* __restrict__ d_group_flags,
    TileOp* __restrict__ d_tileops,
    std::uint16_t* __restrict__ d_face_indices,
    std::uint16_t* __restrict__ d_face_counts,
    std::uint16_t* __restrict__ d_face_roots,
    FaceRepresentative* __restrict__ d_face_reps,
    std::uint16_t* __restrict__ d_face_rep_counts,
    int num_tiles) {
  const int tile_idx = static_cast<int>(blockIdx.x);
  if (tile_idx >= num_tiles) return;

  const int tid = static_cast<int>(threadIdx.x);
  const std::uint32_t prime_count = d_prime_count[tile_idx];
  const bool remap_overflow =
      d_remap_overflow != nullptr && d_remap_overflow[tile_idx] != 0;
  TileOp* out = d_tileops + static_cast<std::size_t>(tile_idx);
  const std::uint8_t* tile_group_flags =
      d_group_flags == nullptr
          ? nullptr
          : d_group_flags + static_cast<std::size_t>(tile_idx) * 256U;
  std::uint16_t* tile_face_counts =
      d_face_counts == nullptr
          ? nullptr
          : d_face_counts + static_cast<std::size_t>(tile_idx) *
                                FACE_COUNT_STRIDE;
  std::uint16_t* tile_face_rep_counts =
      d_face_rep_counts == nullptr
          ? nullptr
          : d_face_rep_counts + static_cast<std::size_t>(tile_idx) *
                                    FACE_COUNT_STRIDE;

  if (prime_count == 0) {
    zero_face_debug_counts(tile_face_counts, tile_face_rep_counts);
    if (tid == 0) {
      write_flag_tileop(out, EMPTY_BIT);
    }
    return;
  }

  if (remap_overflow) {
    zero_face_debug_counts(tile_face_counts, tile_face_rep_counts);
    if (tid == 0) {
      write_flag_tileop(out, OVERFLOW_BIT);
    }
    return;
  }

  if (tid == 0) {
    write_flag_tileop(out, 0);
  }
  __syncthreads();
  unpack_group_flags(out, tile_group_flags);

  const int bounded = prime_count < MAX_PRIMES_GPU
                          ? static_cast<int>(prime_count)
                          : MAX_PRIMES_GPU;
  const std::uint32_t* tile_prime_pos =
      d_prime_pos + static_cast<std::size_t>(tile_idx) * MAX_PRIMES_GPU;
  const std::uint16_t* tile_parent =
      d_parent == nullptr
          ? nullptr
          : d_parent + static_cast<std::size_t>(tile_idx) * MAX_PRIMES_GPU;
  const std::uint16_t* tile_wire_label_by_raw_root =
      d_wire_label_by_raw_root == nullptr
          ? nullptr
          : d_wire_label_by_raw_root + static_cast<std::size_t>(tile_idx) *
                                           MAX_PRIMES_GPU;
  std::uint16_t* tile_face_indices =
      d_face_indices == nullptr
          ? nullptr
          : d_face_indices + static_cast<std::size_t>(tile_idx) * NUM_FACES *
                                 FACE_INDEX_STRIDE;
  std::uint16_t* tile_face_roots =
      d_face_roots == nullptr
          ? nullptr
          : d_face_roots + static_cast<std::size_t>(tile_idx) * NUM_FACES *
                               FACE_ROOT_STRIDE;
  FaceRepresentative* tile_face_reps =
      d_face_reps == nullptr
          ? nullptr
          : d_face_reps + static_cast<std::size_t>(tile_idx) * NUM_FACES *
                              FACE_REP_STRIDE;

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
  __syncthreads();

  if (tid == 0 && tile_face_roots != nullptr && tile_face_reps != nullptr &&
      tile_face_rep_counts != nullptr) {
    for (int face_idx = 0; face_idx < NUM_FACES; ++face_idx) {
      build_face_dsu_and_reps(
          tile_prime_pos, tile_parent, tile_wire_label_by_raw_root,
          static_cast<Face>(face_idx),
          tile_face_indices + face_idx * FACE_INDEX_STRIDE,
          tile_face_counts[face_idx],
          tile_face_roots + face_idx * FACE_ROOT_STRIDE,
          tile_face_reps + face_idx * FACE_REP_STRIDE,
          tile_face_rep_counts + face_idx);
    }
  }

  (void)d_coords;
}

}  // namespace

void launch_kernel_face_encode_v2(const FaceEncodeBuffers& buffers,
                                  int num_tiles,
                                  cudaStream_t stream) {
  kernel_face_encode_v2<<<num_tiles, BLOCK_THREADS, 0, stream>>>(
      buffers.in.d_coords, buffers.in.d_prime_pos, buffers.in.d_prime_count,
      buffers.in.d_remap_overflow, buffers.in.d_parent,
      buffers.in.d_wire_label_by_raw_root, buffers.in.d_group_flags,
      buffers.d_tileops,
      buffers.debug.d_face_indices, buffers.debug.d_face_counts,
      buffers.debug.d_face_roots, buffers.debug.d_face_reps,
      buffers.debug.d_face_rep_counts, num_tiles);
}

}  // namespace cuda_campaign
