#include "cuda_campaign/face_sort_pack.cuh"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace cuda_campaign {
namespace {

inline constexpr int SORT_CAPACITY = 256;
static_assert(SORT_CAPACITY >= MAX_PORTS_PER_TILE,
              "sort capacity must cover TileOp face_groups capacity");

__device__ __forceinline__ FaceRepresentative sentinel_rep() {
  return FaceRepresentative{32767, 32767, 65535, 255, 0};
}

__device__ __forceinline__ bool rep_less(const FaceRepresentative& lhs,
                                         const FaceRepresentative& rhs) {
  if (lhs.h != rhs.h) return lhs.h < rhs.h;
  if (lhs.p_perp != rhs.p_perp) return lhs.p_perp < rhs.p_perp;
  return lhs.global_wire_label < rhs.global_wire_label;
}

__device__ __forceinline__ void swap_rep(FaceRepresentative& lhs,
                                         FaceRepresentative& rhs) {
  const FaceRepresentative tmp = lhs;
  lhs = rhs;
  rhs = tmp;
}

__device__ __forceinline__ void zero_tileop(TileOp* out) {
  std::uint8_t* bytes = reinterpret_cast<std::uint8_t*>(out);
  for (int i = 0; i < TILEOP_SIZE; ++i) {
    bytes[i] = 0;
  }
}

__global__ void kernel_face_sort_pack(
    const FaceRepresentative* __restrict__ d_face_reps,
    const std::uint16_t* __restrict__ d_face_rep_counts,
    TileOp* __restrict__ d_tileops,
    int num_tiles) {
  const int tile_idx = static_cast<int>(blockIdx.x);
  if (tile_idx >= num_tiles) return;

  const int tid = static_cast<int>(threadIdx.x);
  const int warp = tid >> 5;
  const int lane = tid & 31;

  __shared__ FaceRepresentative sort_scratch[NUM_FACES][SORT_CAPACITY];
  __shared__ std::uint16_t counts[NUM_FACES];
  __shared__ std::uint8_t overflow;

  TileOp* out = d_tileops + static_cast<std::size_t>(tile_idx);

  if (tid == 0) {
    zero_tileop(out);
    overflow = 0;
  }
  if (tid < NUM_FACES) {
    const std::uint16_t count =
        d_face_rep_counts[static_cast<std::size_t>(tile_idx) *
                              FACE_COUNT_STRIDE +
                          tid];
    counts[tid] = count;
  }
  __syncthreads();

  if (tid == 0) {
    std::uint32_t sum = 0;
    for (int face = 0; face < NUM_FACES; ++face) {
      sum += counts[face];
      if (counts[face] > MAX_PORTS_PER_TILE) {
        overflow = 1;
      }
    }
    if (sum > MAX_PORTS_PER_TILE) {
      overflow = 1;
    }
    if (overflow != 0) {
      out->tile_flags = OVERFLOW_BIT;
    }
  }
  __syncthreads();

  if (overflow != 0) return;

  if (warp < NUM_FACES) {
    const std::uint16_t count = counts[warp];
    const FaceRepresentative* face_reps =
        d_face_reps + static_cast<std::size_t>(tile_idx) * NUM_FACES *
                          FACE_REP_STRIDE +
        static_cast<std::size_t>(warp) * FACE_REP_STRIDE;

    for (int i = lane; i < SORT_CAPACITY; i += 32) {
      sort_scratch[warp][i] =
          i < static_cast<int>(count) ? face_reps[i] : sentinel_rep();
    }
    __syncwarp();

    for (int size = 2; size <= SORT_CAPACITY; size <<= 1) {
      for (int stride = size >> 1; stride > 0; stride >>= 1) {
        for (int i = lane; i < SORT_CAPACITY; i += 32) {
          const int partner = i ^ stride;
          if (partner > i) {
            FaceRepresentative& lhs = sort_scratch[warp][i];
            FaceRepresentative& rhs = sort_scratch[warp][partner];
            const bool ascending = (i & size) == 0;
            const bool out_of_order =
                ascending ? rep_less(rhs, lhs) : rep_less(lhs, rhs);
            if (out_of_order) {
              swap_rep(lhs, rhs);
            }
          }
        }
        __syncwarp();
      }
    }

    if (lane == 0) {
      out->n[warp] = static_cast<std::uint8_t>(count);
    }
  }
  __syncthreads();

  if (tid == 0) {
    int write_offset = 0;
    for (int face = 0; face < NUM_FACES; ++face) {
      for (int i = 0; i < static_cast<int>(counts[face]); ++i) {
        out->face_groups[write_offset] =
            sort_scratch[face][i].global_wire_label;
        ++write_offset;
      }
    }
  }
}

}  // namespace

void launch_kernel_face_sort_pack(const FaceSortPackBuffers& buffers,
                                  int num_tiles,
                                  cudaStream_t stream) {
  kernel_face_sort_pack<<<num_tiles, BLOCK_THREADS, 0, stream>>>(
      buffers.in.d_face_reps, buffers.in.d_face_rep_counts, buffers.d_tileops,
      num_tiles);
}

}  // namespace cuda_campaign
