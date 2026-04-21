#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "cuda_campaign/gpu_math.cuh"

namespace cuda_campaign {
namespace {

static __device__ __forceinline__ bool gpu_bitmap_test_uf(
    const std::uint32_t* bitmap, int row, int col) {
  return ((bitmap[row * BITMAP_WORDS_PER_ROW + (col >> 5)] >> (col & 31)) &
          1u) != 0u;
}

static __device__ __forceinline__ int gpu_uf_index(
    int row,
    int col,
    const std::uint32_t* bitmap,
    const std::uint16_t* row_prefix) {
  std::uint32_t idx = row_prefix[row];
  const int base = row * BITMAP_WORDS_PER_ROW;
  const int full_words = col >> 5;

  if (full_words > 0) idx += __popc(bitmap[base + 0]);
  if (full_words > 1) idx += __popc(bitmap[base + 1]);
  if (full_words > 2) idx += __popc(bitmap[base + 2]);
  if (full_words > 3) idx += __popc(bitmap[base + 3]);
  if (full_words > 4) idx += __popc(bitmap[base + 4]);
  if (full_words > 5) idx += __popc(bitmap[base + 5]);
  if (full_words > 6) idx += __popc(bitmap[base + 6]);
  if (full_words > 7) idx += __popc(bitmap[base + 7]);

  const std::uint32_t bit_mask =
      (col & 31) == 0 ? 0u : ((1u << (col & 31)) - 1u);
  idx += __popc(bitmap[base + full_words] & bit_mask);
  return static_cast<int>(idx);
}

static __device__ __forceinline__ uint16_t atomic_find_root(uint16_t* parent, uint16_t x) {
  uint16_t r = x;
  while (true) {
    uint16_t p = parent[r];
    if (p == r) break;
    // Path splitting: point to grandparent
    uint16_t gp = parent[p];
    // Use atomicCAS for safe path compression
    atomicCAS(reinterpret_cast<unsigned short*>(&parent[r]), r, gp);
    r = p;
  }
  return r;
}

static __device__ __forceinline__ bool atomic_union(uint16_t* parent, uint16_t x, uint16_t y) {
  while (true) {
    uint16_t rx = atomic_find_root(parent, x);
    uint16_t ry = atomic_find_root(parent, y);
    if (rx == ry) return false;  // already same component

    // Always make smaller root the parent (deterministic)
    if (rx > ry) {
      uint16_t tmp = rx; rx = ry; ry = tmp;
    }

    // Try to set parent[ry] = rx
    uint16_t old = atomicCAS(reinterpret_cast<unsigned short*>(&parent[ry]),
                             static_cast<unsigned short>(ry),
                             static_cast<unsigned short>(rx));
    if (old == ry) return true;  // success
    // If failed, someone else modified parent[ry]; retry
  }
}

__global__ void kernel_uf_v2(const std::uint32_t* __restrict__ d_bitmap,
                             const std::uint16_t* __restrict__ d_row_prefix,
                             const std::uint32_t* __restrict__ d_prime_pos,
                             const std::uint32_t* __restrict__ d_prime_count,
                             std::uint16_t* __restrict__ d_parent,
                             int num_tiles) {
  const int tile_idx = static_cast<int>(blockIdx.x);
  if (tile_idx >= num_tiles) return;

  const int tid = static_cast<int>(threadIdx.x);

  const std::uint32_t* tile_bitmap =
      d_bitmap + static_cast<std::size_t>(tile_idx) * BITMAP_WORDS;
  const std::uint16_t* tile_row_prefix =
      d_row_prefix + static_cast<std::size_t>(tile_idx) * (ACTIVE_ROWS + 1);
  const std::uint32_t* tile_prime_pos =
      d_prime_pos + static_cast<std::size_t>(tile_idx) * MAX_PRIMES_GPU;
  std::uint16_t* tile_parent =
      d_parent + static_cast<std::size_t>(tile_idx) * MAX_PRIMES_GPU;

  const int prime_count = static_cast<int>(d_prime_count[tile_idx]);
  if (prime_count > MAX_PRIMES_GPU) {
    // K5 will conservatively encode overflow once the M4 overflow buffer is
    // wired. Phase A/B has no valid bounded parent array to emit here.
    return;
  }
  const int bounded =
      prime_count < MAX_PRIMES_GPU ? prime_count : MAX_PRIMES_GPU;

  for (int i = tid; i < bounded; i += BLOCK_THREADS) {
    tile_parent[i] = static_cast<std::uint16_t>(i);
  }
  __syncthreads();

  for (int i = tid; i < bounded; i += BLOCK_THREADS) {
    const std::uint32_t packed = tile_prime_pos[i];
    const int row = static_cast<int>(packed / SIDE_EXP);
    const int col = static_cast<int>(packed % SIDE_EXP);

    for (int k = 0; k < NUM_BACKWARD_OFFSETS; ++k) {
      const int nr = row + static_cast<int>(c_bk_dr[k]);
      const int nc = col + static_cast<int>(c_bk_dc[k]);
      if (nr < 0 || nr >= SIDE_EXP || nc < 0 || nc >= SIDE_EXP) {
        continue;
      }
      if (!gpu_bitmap_test_uf(tile_bitmap, nr, nc)) {
        continue;
      }

      const int j = gpu_uf_index(nr, nc, tile_bitmap, tile_row_prefix);
      if (j >= 0 && j < bounded) {
        atomic_union(tile_parent, static_cast<std::uint16_t>(i),
                     static_cast<std::uint16_t>(j));
      }
    }
  }
  __syncthreads();

  for (int i = tid; i < bounded; i += BLOCK_THREADS) {
    tile_parent[i] = atomic_find_root(tile_parent, static_cast<std::uint16_t>(i));
  }
  __syncthreads();

  // TODO(M4): Phase B.5 geo flag staging, Phase C serial dense-remap, and
  // Phase D group-flag accumulation start here after compressed parents are
  // visible to all threads.
}

}  // namespace

void launch_kernel_uf_v2(const UfBuffers& buffers,
                         int num_tiles,
                         cudaStream_t stream) {
  kernel_uf_v2<<<num_tiles, BLOCK_THREADS, 0, stream>>>(
      buffers.in.d_bitmap, buffers.in.d_row_prefix, buffers.in.d_prime_pos,
      buffers.in.d_prime_count, buffers.out.d_parent, num_tiles);
}

void launch_kernel_uf_v2(const std::uint32_t* d_bitmap,
                         const std::uint16_t* d_row_prefix,
                         const std::uint32_t* d_prime_pos,
                         const std::uint32_t* d_prime_count,
                         std::uint16_t* d_parent,
                         int num_tiles,
                         cudaStream_t stream) {
  UfBuffers buffers{};
  buffers.in.d_bitmap = d_bitmap;
  buffers.in.d_row_prefix = d_row_prefix;
  buffers.in.d_prime_pos = d_prime_pos;
  buffers.in.d_prime_count = d_prime_count;
  buffers.out.d_parent = d_parent;
  launch_kernel_uf_v2(buffers, num_tiles, stream);
}

}  // namespace cuda_campaign
