#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "cuda_campaign/gpu_math.cuh"

namespace cuda_campaign {

namespace {

__device__ void sieve_row_k1(std::uint32_t ws[BITMAP_WORDS_PER_ROW],
                             std::int32_t a,
                             std::int32_t b_start) {
  const std::uint32_t pattern =
      ((a ^ b_start) & 1) != 0 ? 0xAAAAAAAAU : 0x55555555U;
#pragma unroll
  for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
    ws[w] = pattern;
  }
  ws[BITMAP_WORDS_PER_ROW - 1] &= LAST_WORD_MASK;

  for (int k = 0; k < SPLIT_PRIMES_COUNT; ++k) {
    const SplitPrimeBarrettGPU entry = c_split_barrett[k];
    const std::uint32_t p = static_cast<std::uint32_t>(entry.p);
    const std::uint32_t root = static_cast<std::uint32_t>(entry.root);
    const std::uint32_t mu = entry.mu;

    const std::uint32_t a_mod =
        static_cast<std::uint32_t>(barrett_euclidean_mod(a, p, mu));
    const std::uint32_t product = a_mod * root;
    const std::int32_t residue =
        static_cast<std::int32_t>(barrett_mod_u32(product, p, mu));

    mark_residue_class_barrett(ws, b_start, p, residue, mu);

    const std::int32_t neg_res =
        (residue == 0) ? 0 : static_cast<std::int32_t>(p - static_cast<std::uint32_t>(residue));
    if (neg_res != residue) {
      mark_residue_class_barrett(ws, b_start, p, neg_res, mu);
    }
  }

  if (a == 0) {
    return;
  }

  for (int k = 0; k < INERT_PRIMES_COUNT; ++k) {
    const InertPrimeBarrettGPU entry = c_inert_barrett[k];
    const std::uint32_t p = static_cast<std::uint32_t>(entry.p);
    const std::uint32_t mu = entry.mu;

    if (barrett_euclidean_mod(a, p, mu) == 0) {
      mark_residue_class_barrett(ws, b_start, p, 0, mu);
    }
  }
}

__device__ std::uint32_t count_sieve_survivors_k1(
    const std::uint32_t ws[BITMAP_WORDS_PER_ROW]) {
  std::uint32_t count = 0;
#pragma unroll
  for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
    std::uint32_t survivors = ~ws[w];
    if (w == BITMAP_WORDS_PER_ROW - 1) {
      survivors &= LAST_WORD_MASK;
    }
    count += __popc(survivors);
  }
  return count;
}

__device__ void scatter_survivors_k1(const std::uint32_t ws[BITMAP_WORDS_PER_ROW],
                                     std::uint32_t* cand_list,
                                     int offset,
                                     int row) {
#pragma unroll
  for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
    std::uint32_t survivors = ~ws[w];
    if (w == BITMAP_WORDS_PER_ROW - 1) {
      survivors &= LAST_WORD_MASK;
    }
    while (survivors != 0U) {
      const int bit = __ffs(survivors) - 1;
      const int col = w * 32 + bit;
      if (col < SIDE_EXP) {
        cand_list[offset++] =
            (static_cast<std::uint32_t>(row) << 16) | static_cast<std::uint32_t>(col);
      }
      survivors &= survivors - 1U;
    }
  }
}

__device__ void scatter_survivors_clamped_k1(
    const std::uint32_t ws[BITMAP_WORDS_PER_ROW],
    std::uint32_t* cand_list,
    int offset,
    int max_count,
    int row) {
  int written = 0;
#pragma unroll
  for (int w = 0; w < BITMAP_WORDS_PER_ROW && written < max_count; ++w) {
    std::uint32_t survivors = ~ws[w];
    if (w == BITMAP_WORDS_PER_ROW - 1) {
      survivors &= LAST_WORD_MASK;
    }
    while (survivors != 0U && written < max_count) {
      const int bit = __ffs(survivors) - 1;
      const int col = w * 32 + bit;
      if (col < SIDE_EXP) {
        cand_list[offset + written] =
            (static_cast<std::uint32_t>(row) << 16) | static_cast<std::uint32_t>(col);
        ++written;
      }
      survivors &= survivors - 1U;
    }
  }
}

__global__ void kernel_sieve(const campaign::TileCoord* __restrict__ coords,
                             std::uint32_t* __restrict__ d_cand_list,
                             std::uint32_t* __restrict__ d_total_cands,
                             std::uint32_t* __restrict__ d_raw_total_cands,
                             std::uint32_t* __restrict__ d_k1_overflow,
                             int num_tiles,
                             int candidate_capacity) {
  const int tile_idx = static_cast<int>(blockIdx.x);
  if (tile_idx >= num_tiles) return;

  const int tid = static_cast<int>(threadIdx.x);
  std::uint32_t capacity =
      candidate_capacity > 0
          ? static_cast<std::uint32_t>(candidate_capacity)
          : static_cast<std::uint32_t>(MAX_CANDIDATES_GPU);
  if (capacity > static_cast<std::uint32_t>(MAX_CANDIDATES_GPU)) {
    capacity = static_cast<std::uint32_t>(MAX_CANDIDATES_GPU);
  }
  const campaign::TileCoord coord = coords[tile_idx];
  const std::int32_t a_start =
      static_cast<std::int32_t>(coord.a_lo - static_cast<std::int64_t>(C));
  const std::int32_t b_start =
      static_cast<std::int32_t>(coord.b_lo - static_cast<std::int64_t>(C));

  std::uint32_t* tile_cand_list =
      d_cand_list + static_cast<std::size_t>(tile_idx) * MAX_CANDIDATES_GPU;
  __shared__ std::uint32_t total_cands;
  __shared__ std::uint32_t overflow;

  if (tid == 0) {
    total_cands = 0U;
    overflow = 0U;
    if (d_k1_overflow != nullptr) {
      d_k1_overflow[tile_idx] = 0U;
    }
  }
  __syncthreads();

  if (tid < ACTIVE_ROWS) {
    std::uint32_t ws[BITMAP_WORDS_PER_ROW];
    const std::int32_t a = a_start + tid;
    sieve_row_k1(ws, a, b_start);

    const std::uint32_t count = count_sieve_survivors_k1(ws);
    if (count > 0U) {
      const std::uint32_t base = atomicAdd(&total_cands, count);
      if (base + count <= capacity) {
        scatter_survivors_k1(ws, tile_cand_list, static_cast<int>(base), tid);
      } else {
        atomicExch(&overflow, 1U);
        if (d_k1_overflow != nullptr) {
          atomicExch(d_k1_overflow + tile_idx, 1U);
        }
      }
      if (base + count > capacity && base < capacity) {
        scatter_survivors_clamped_k1(
            ws, tile_cand_list, static_cast<int>(base),
            static_cast<int>(capacity - base),
            tid);
      }
    }
  }
  __syncthreads();

  if (tid == 0) {
    std::uint32_t final_count = total_cands;
    if (final_count > capacity) {
      final_count = capacity;
    }
    if (d_k1_overflow != nullptr &&
        (overflow != 0U || total_cands > capacity)) {
      d_k1_overflow[tile_idx] = 1U;
    }
    if (d_raw_total_cands != nullptr) {
      d_raw_total_cands[tile_idx] = total_cands;
    }
    d_total_cands[tile_idx] = final_count;
  }
}

}  // namespace

void launch_kernel_sieve(const campaign::TileCoord* d_coords,
                         std::uint32_t* d_cand_list,
                         std::uint32_t* d_total_cands,
                         std::uint32_t* d_raw_total_cands,
                         std::uint32_t* d_k1_overflow,
                         int num_tiles,
                         int candidate_capacity,
                         cudaStream_t stream) {
  kernel_sieve<<<num_tiles, BLOCK_THREADS, 0, stream>>>(
      d_coords, d_cand_list, d_total_cands, d_raw_total_cands, d_k1_overflow, num_tiles,
      candidate_capacity);
}

}  // namespace cuda_campaign
