// K3 Compact - bitmap -> row_prefix + prime_pos + prime_count.
// 288 threads/block, target <=32 registers.
// Reads bitmap, does popcount per row, prefix scan, scatter prime positions.

#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "cuda_campaign/compact_buffers.cuh"

namespace cuda_campaign {

namespace {

// ---- Exclusive prefix scan on uint16_t in shared memory ----

__device__ void block_exclusive_scan_u16_k3(std::uint16_t* data, int n, int tid) {
  const std::uint16_t original =
      tid < n ? data[tid] : static_cast<std::uint16_t>(0U);
  for (int offset = 1; offset < n; offset <<= 1) {
    std::uint16_t addend = 0U;
    if (tid < n && tid >= offset) {
      addend = data[tid - offset];
    }
    __syncthreads();
    if (tid < n) {
      data[tid] = static_cast<std::uint16_t>(data[tid] + addend);
    }
    __syncthreads();
  }

  if (tid < n) {
    data[tid] = static_cast<std::uint16_t>(data[tid] - original);
  }
  __syncthreads();
}

// ---- K3 Compact kernel ----

__global__ void kernel_compact(const std::uint32_t* __restrict__ d_bitmap,
                               std::uint16_t* __restrict__ d_row_prefix,
                               std::uint32_t* __restrict__ d_prime_pos,
                               std::uint32_t* __restrict__ d_prime_count,
                               int num_tiles) {
  const int tile_idx = static_cast<int>(blockIdx.x);
  if (tile_idx >= num_tiles) return;

  const int tid = static_cast<int>(threadIdx.x);

  // Per-tile pointers
  const std::uint32_t* tile_bitmap =
      d_bitmap + static_cast<std::size_t>(tile_idx) * BITMAP_WORDS;
  std::uint16_t* tile_row_prefix =
      d_row_prefix + static_cast<std::size_t>(tile_idx) * ROW_PREFIX_ENTRIES;
  std::uint32_t* tile_prime_pos =
      d_prime_pos + static_cast<std::size_t>(tile_idx) * MAX_PRIMES_GPU;

  // Shared memory for row_prefix during scan
  extern __shared__ std::uint16_t smem_row_prefix[];

  std::uint32_t row_count = 0;
  if (tid < ACTIVE_ROWS) {
#pragma unroll
    for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
      std::uint32_t word = tile_bitmap[tid * BITMAP_WORDS_PER_ROW + w];
      if (w == (BITMAP_WORDS_PER_ROW - 1)) {
        word &= LAST_WORD_MASK;
      }
      row_count += __popc(word);
    }
    smem_row_prefix[tid] = static_cast<std::uint16_t>(row_count);
  }
  __syncthreads();

  block_exclusive_scan_u16_k3(smem_row_prefix, ACTIVE_ROWS, tid);
  __syncthreads();

  if (tid == (ACTIVE_ROWS - 1)) {
    smem_row_prefix[ACTIVE_ROWS] =
        static_cast<std::uint16_t>(smem_row_prefix[ACTIVE_ROWS - 1] +
                                   row_count);
  }
  __syncthreads();

  // Write row_prefix to global memory
  if (tid <= ACTIVE_ROWS) {
    tile_row_prefix[tid] = smem_row_prefix[tid];
  }
  __syncthreads();

  // Scatter prime positions
  if (tid < ACTIVE_ROWS) {
    std::uint16_t offset = smem_row_prefix[tid];
#pragma unroll
    for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
      std::uint32_t word = tile_bitmap[tid * BITMAP_WORDS_PER_ROW + w];
      if (w == (BITMAP_WORDS_PER_ROW - 1)) {
        word &= LAST_WORD_MASK;
      }
      while (word != 0U) {
        const int bit = __ffs(word) - 1;
        if (offset < MAX_PRIMES_GPU) {
          tile_prime_pos[offset] =
              static_cast<std::uint32_t>(tid * SIDE_EXP + (w * 32 + bit));
        }
        offset = static_cast<std::uint16_t>(offset + 1U);
        word &= (word - 1U);
      }
    }
  }
  __syncthreads();

  // Write total prime count
  if (tid == 0) {
    d_prime_count[tile_idx] =
        static_cast<std::uint32_t>(smem_row_prefix[ACTIVE_ROWS]);
  }
}

}  // namespace

std::size_t kernel_compact_shared_bytes() {
  return ROW_PREFIX_BYTES_PER_TILE;
}

void launch_kernel_compact(const std::uint32_t* d_bitmap,
                           std::uint16_t* d_row_prefix,
                           std::uint32_t* d_prime_pos,
                           std::uint32_t* d_prime_count,
                           int num_tiles,
                           cudaStream_t stream) {
  kernel_compact<<<num_tiles, BLOCK_THREADS, kernel_compact_shared_bytes(),
                   stream>>>(d_bitmap, d_row_prefix, d_prime_pos, d_prime_count,
                             num_tiles);
}

void launch_kernel_compact(const std::uint32_t* d_bitmap,
                           const CompactBuffers& buffers,
                           int num_tiles,
                           cudaStream_t stream) {
  launch_kernel_compact(d_bitmap, buffers.d_row_prefix, buffers.d_prime_pos,
                        buffers.d_prime_count, num_tiles, stream);
}

}  // namespace cuda_campaign
