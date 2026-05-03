#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "cuda_campaign/campaign_constants.cuh"
#include "cuda_campaign/gpu_math.cuh"

#ifndef MR_BLOCK_THREADS
#define MR_BLOCK_THREADS BLOCK_THREADS
#endif

namespace cuda_campaign {

namespace {

__device__ __forceinline__ void gpu_bitmap_set_global(std::uint32_t* bitmap,
                                                      int row,
                                                      int col) {
  atomicOr(&bitmap[row * BITMAP_WORDS_PER_ROW + (col >> 5)], 1U << (col & 31));
}

__device__ __forceinline__ bool in_octant(std::int64_t a, std::int64_t b) {
  return a >= 0 && b >= a;
}

__device__ __forceinline__ bool in_annulus(std::uint64_t norm) {
  return norm >= c_campaign_constants.R_inner_sq &&
         norm <= c_campaign_constants.R_outer_sq;
}

__global__ void kernel_mr(const campaign::TileCoord* __restrict__ coords,
                          const std::uint32_t* __restrict__ d_cand_list,
                          const std::uint32_t* __restrict__ d_total_cands,
                          std::uint32_t* __restrict__ d_bitmap,
                          const std::uint16_t* __restrict__ d_fj64_table,
                          int num_tiles) {
  const int tile_idx = static_cast<int>(blockIdx.x);
  if (tile_idx >= num_tiles) return;

  const int tid = static_cast<int>(threadIdx.x);
  const campaign::TileCoord coord = coords[tile_idx];
  const std::int64_t a_start = coord.a_lo - static_cast<std::int64_t>(C);
  const std::int64_t b_start = coord.b_lo - static_cast<std::int64_t>(C);

  const std::uint32_t* tile_cand_list =
      d_cand_list + static_cast<std::size_t>(tile_idx) * MAX_CANDIDATES_GPU;
  std::uint32_t* tile_bitmap =
      d_bitmap + static_cast<std::size_t>(tile_idx) * BITMAP_WORDS;

  for (int i = tid; i < BITMAP_WORDS; i += MR_BLOCK_THREADS) {
    tile_bitmap[i] = 0U;
  }
  __syncthreads();

  const int total_cands = static_cast<int>(d_total_cands[tile_idx]);

  for (int i = tid; i < total_cands; i += MR_BLOCK_THREADS) {
    const std::uint32_t packed = tile_cand_list[i];
    const int cand_row = static_cast<int>(packed >> 16);
    const int cand_col = static_cast<int>(packed & 0xFFFFU);
    const std::int64_t ca = a_start + cand_row;
    const std::int64_t cb = b_start + cand_col;

    if (!in_octant(ca, cb)) {
      continue;
    }

    const std::uint64_t ua = abs_i64_to_u64_gpu(ca);
    const std::uint64_t ub = abs_i64_to_u64_gpu(cb);
    const std::uint64_t norm = ua * ua + ub * ub;
    if (!in_annulus(norm)) {
      continue;
    }

    // K1 scans fixed-a rows; K3-K5 consume canonical row=b, col=a bitmaps.
    if (ca == 0 || cb == 0) {
      if (is_axis_gaussian_prime_gpu(ca, cb, d_fj64_table)) {
        gpu_bitmap_set_global(tile_bitmap, cand_col, cand_row);
      }
      continue;
    }

    if ((norm == 2ULL) ||
        (((norm & 3ULL) == 1ULL) &&
         is_prime_fj64_prefiltered_gpu(norm, d_fj64_table))) {
      gpu_bitmap_set_global(tile_bitmap, cand_col, cand_row);
    }
  }
}

}  // namespace

void launch_kernel_mr(const campaign::TileCoord* d_coords,
                      const std::uint32_t* d_cand_list,
                      const std::uint32_t* d_total_cands,
                      std::uint32_t* d_bitmap,
                      const std::uint16_t* d_fj64_table,
                      int num_tiles,
                      cudaStream_t stream) {
  kernel_mr<<<num_tiles, MR_BLOCK_THREADS, 0, stream>>>(
      d_coords, d_cand_list, d_total_cands, d_bitmap, d_fj64_table, num_tiles);
}

}  // namespace cuda_campaign
