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

__device__ __forceinline__ int sort_bound_for_count(std::uint16_t count) {
  int bound = 1;
  while (bound < static_cast<int>(count)) {
    bound <<= 1;
  }
  return bound;
}

__device__ __forceinline__ void zero_tileop(TileOp* out) {
  std::uint8_t* bytes = reinterpret_cast<std::uint8_t*>(out);
  for (int i = 0; i < TILEOP_SIZE; ++i) {
    bytes[i] = 0;
  }
}

__device__ __forceinline__ void zero_sort_payload(TileOp* out) {
  for (int i = 0; i < NUM_FACES; ++i) {
    out->n[i] = 0;
  }
  for (int i = 0; i < MAX_PORTS_PER_TILE; ++i) {
    out->face_groups[i] = 0;
  }
  for (int i = 0; i < static_cast<int>(sizeof(out->reserved)); ++i) {
    out->reserved[i] = 0;
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
    zero_sort_payload(out);
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
      if (counts[face] > 255U) {
        overflow = 1;
      }
    }
    if (sum > MAX_PORTS_PER_TILE) {
      overflow = 1;
    }
    if (overflow != 0) {
      zero_tileop(out);
      out->tile_flags = OVERFLOW_BIT;
    }
  }
  __syncthreads();

  if (overflow != 0) return;

  if (warp < NUM_FACES) {
    const std::uint16_t count = counts[warp];
    const int sort_bound = sort_bound_for_count(count);
    const FaceRepresentative* face_reps =
        d_face_reps + static_cast<std::size_t>(tile_idx) * NUM_FACES *
                          FACE_REP_STRIDE +
        static_cast<std::size_t>(warp) * FACE_REP_STRIDE;

    for (int i = lane; i < sort_bound; i += 32) {
      sort_scratch[warp][i] =
          i < static_cast<int>(count) ? face_reps[i] : sentinel_rep();
    }
    __syncwarp();

    for (int size = 2; size <= sort_bound; size <<= 1) {
      for (int stride = size >> 1; stride > 0; stride >>= 1) {
        for (int i = lane; i < sort_bound; i += 32) {
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

namespace {

inline constexpr int SUMMARY_THREADS = 256;
inline constexpr int SUMMARY_COUNTERS = 4;

__global__ void kernel_overflow_summary(
    const std::uint32_t* __restrict__ d_k1_overflow,
    const std::uint32_t* __restrict__ d_prime_count,
    const std::uint8_t* __restrict__ d_remap_overflow,
    const std::uint16_t* __restrict__ d_face_rep_counts,
    unsigned long long* __restrict__ d_summary_counts,
    int num_tiles) {
  const int tid = static_cast<int>(threadIdx.x);
  const int tile_idx =
      static_cast<int>(blockIdx.x) * SUMMARY_THREADS + tid;

  __shared__ unsigned int block_counts[SUMMARY_THREADS][SUMMARY_COUNTERS];

  unsigned int k1_count = 0;
  unsigned int k4_prime_count = 0;
  unsigned int k4_group_count = 0;
  unsigned int k5_port_count = 0;

  if (tile_idx < num_tiles) {
    const bool k1_cand_overflow = d_k1_overflow[tile_idx] != 0U;
    const bool k4_prime_overflow =
        d_prime_count[tile_idx] > static_cast<std::uint32_t>(MAX_PRIMES_GPU);
    const bool k4_group_overflow =
        d_remap_overflow[tile_idx] != 0 && !k1_cand_overflow &&
        !k4_prime_overflow;

    std::uint32_t port_sum = 0;
    for (int face = 0; face < NUM_FACES; ++face) {
      port_sum +=
          d_face_rep_counts[static_cast<std::size_t>(tile_idx) *
                                FACE_COUNT_STRIDE +
                            static_cast<std::size_t>(face)];
    }
    const bool k5_port_overflow = port_sum > MAX_PORTS_PER_TILE;

    k1_count = k1_cand_overflow ? 1U : 0U;
    k4_prime_count = k4_prime_overflow ? 1U : 0U;
    k4_group_count = k4_group_overflow ? 1U : 0U;
    k5_port_count = k5_port_overflow ? 1U : 0U;
  }

  block_counts[tid][0] = k1_count;
  block_counts[tid][1] = k4_prime_count;
  block_counts[tid][2] = k4_group_count;
  block_counts[tid][3] = k5_port_count;
  __syncthreads();

  for (int stride = SUMMARY_THREADS / 2; stride > 0; stride >>= 1) {
    if (tid < stride) {
      block_counts[tid][0] += block_counts[tid + stride][0];
      block_counts[tid][1] += block_counts[tid + stride][1];
      block_counts[tid][2] += block_counts[tid + stride][2];
      block_counts[tid][3] += block_counts[tid + stride][3];
    }
    __syncthreads();
  }

  if (tid == 0) {
    atomicAdd(d_summary_counts + 0,
              static_cast<unsigned long long>(block_counts[0][0]));
    atomicAdd(d_summary_counts + 1,
              static_cast<unsigned long long>(block_counts[0][1]));
    atomicAdd(d_summary_counts + 2,
              static_cast<unsigned long long>(block_counts[0][2]));
    atomicAdd(d_summary_counts + 3,
              static_cast<unsigned long long>(block_counts[0][3]));
  }
}

}  // namespace

void launch_kernel_overflow_summary(
    const std::uint32_t* d_k1_overflow,
    const std::uint32_t* d_prime_count,
    const std::uint8_t* d_remap_overflow,
    const std::uint16_t* d_face_rep_counts,
    unsigned long long* d_summary_counts,
    int num_tiles,
    cudaStream_t stream) {
  const int blocks = (num_tiles + SUMMARY_THREADS - 1) / SUMMARY_THREADS;
  kernel_overflow_summary<<<blocks, SUMMARY_THREADS, 0, stream>>>(
      d_k1_overflow, d_prime_count, d_remap_overflow, d_face_rep_counts,
      d_summary_counts, num_tiles);
}

}  // namespace cuda_campaign
