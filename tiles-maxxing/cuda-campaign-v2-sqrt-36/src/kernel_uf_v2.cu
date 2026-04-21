#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

#include "cuda_campaign/campaign_constants.cuh"
#include "cuda_campaign/gpu_math.cuh"
#include "cuda_campaign/i128_sq_leq.cuh"

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

static __device__ __forceinline__ bool abs_leq_u64_uf(std::int64_t value,
                                                      std::uint64_t bound) {
  return abs_i64_as_u64(value) <= bound;
}

static __device__ __forceinline__ std::uint8_t geo_bits_for_norm_sq_uf(
    std::uint64_t norm_sq,
    const DeviceCampaignConstants& constants) {
  std::uint8_t bits = 0;
  const std::int64_t norm = static_cast<std::int64_t>(norm_sq);
  const std::int64_t k = static_cast<std::int64_t>(constants.K_SQ_value);

  if (norm_sq >= constants.R_inner_sq) {
    const std::int64_t eps =
        norm - static_cast<std::int64_t>(constants.R_inner_sq) - k;
    if (abs_leq_u64_uf(eps, constants.prefilter_inner) &&
        i128_sq_leq(eps, constants.four_rin_sq_k_hi,
                    constants.four_rin_sq_k_lo)) {
      bits |= 0x1U;
    }
  }

  if (norm_sq <= constants.R_outer_sq) {
    const std::int64_t eps =
        norm - static_cast<std::int64_t>(constants.R_outer_sq) - k;
    if (abs_leq_u64_uf(eps, constants.prefilter_outer) &&
        i128_sq_leq(eps, constants.four_rout_sq_k_hi,
                    constants.four_rout_sq_k_lo)) {
      bits |= 0x2U;
    }
  }

  return bits;
}

static __device__ __forceinline__ std::uint8_t geo_bits_for_prime_pos_uf(
    std::uint32_t packed_pos,
    const campaign::TileCoord& coord,
    const DeviceCampaignConstants& constants) {
  const std::int64_t row = static_cast<std::int64_t>(packed_pos / SIDE_EXP);
  const std::int64_t col = static_cast<std::int64_t>(packed_pos % SIDE_EXP);
  const std::int64_t a = coord.a_lo + row - static_cast<std::int64_t>(C);
  const std::int64_t b = coord.b_lo + col - static_cast<std::int64_t>(C);
  const std::uint64_t norm_sq =
      static_cast<std::uint64_t>(a * a) + static_cast<std::uint64_t>(b * b);
  return geo_bits_for_norm_sq_uf(norm_sq, constants);
}

static __device__ __forceinline__ void atomic_or_u8(std::uint8_t* ptr,
                                                    std::uint8_t value) {
  auto* word_ptr = reinterpret_cast<unsigned int*>(
      reinterpret_cast<std::uintptr_t>(ptr) & ~std::uintptr_t{3});
  const unsigned int shift = static_cast<unsigned int>(
      (reinterpret_cast<std::uintptr_t>(ptr) & 3U) * 8U);
  const unsigned int mask = static_cast<unsigned int>(value) << shift;
  unsigned int old_value = *word_ptr;
  while (true) {
    const unsigned int assumed = old_value;
    const unsigned int desired = assumed | mask;
    if (desired == assumed) {
      return;
    }
    old_value = atomicCAS(word_ptr, assumed, desired);
    if (old_value == assumed) {
      return;
    }
  }
}

__global__ void kernel_uf_v2(const std::uint32_t* __restrict__ d_bitmap,
                             const std::uint16_t* __restrict__ d_row_prefix,
                             const std::uint32_t* __restrict__ d_prime_pos,
                             const std::uint32_t* __restrict__ d_prime_count,
                             const campaign::TileCoord* __restrict__ d_coords,
                             std::uint16_t* __restrict__ d_parent,
                             std::uint8_t* __restrict__ d_prime_geo_bits,
                             std::uint16_t* __restrict__ d_wire_label_by_raw_root,
                             std::uint16_t* __restrict__ d_max_label,
                             std::uint8_t* __restrict__ d_overflow,
                             std::uint8_t* __restrict__ d_group_flags,
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
  std::uint8_t* tile_prime_geo_bits =
      d_prime_geo_bits == nullptr
          ? nullptr
          : d_prime_geo_bits + static_cast<std::size_t>(tile_idx) *
                                   MAX_PRIMES_GPU;
  std::uint16_t* tile_wire_label_by_raw_root =
      d_wire_label_by_raw_root == nullptr
          ? nullptr
          : d_wire_label_by_raw_root + static_cast<std::size_t>(tile_idx) *
                                           MAX_PRIMES_GPU;
  std::uint8_t* tile_group_flags =
      d_group_flags == nullptr
          ? nullptr
          : d_group_flags + static_cast<std::size_t>(tile_idx) * 256U;
  const campaign::TileCoord coord =
      d_coords == nullptr ? campaign::TileCoord{} : d_coords[tile_idx];

  const int prime_count = static_cast<int>(d_prime_count[tile_idx]);
  if (tid == 0) {
    if (d_max_label != nullptr) d_max_label[tile_idx] = 0;
    if (d_overflow != nullptr) d_overflow[tile_idx] = 0;
  }
  if (tile_wire_label_by_raw_root != nullptr) {
    for (int i = tid; i < MAX_PRIMES_GPU; i += BLOCK_THREADS) {
      tile_wire_label_by_raw_root[i] = 0;
    }
  }
  if (tile_prime_geo_bits != nullptr) {
    for (int i = tid; i < MAX_PRIMES_GPU; i += BLOCK_THREADS) {
      tile_prime_geo_bits[i] = 0;
    }
  }
  if (tile_group_flags != nullptr) {
    for (int i = tid; i < 256; i += BLOCK_THREADS) {
      tile_group_flags[i] = 0;
    }
  }
  __syncthreads();

  if (prime_count > MAX_PRIMES_GPU) {
    if (tid == 0 && d_overflow != nullptr) {
      d_overflow[tile_idx] = 1;
    }
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

  for (int i = tid; i < bounded; i += BLOCK_THREADS) {
    const std::uint8_t bits =
        d_coords == nullptr
            ? 0
            : geo_bits_for_prime_pos_uf(
                  tile_prime_pos[i], coord, c_campaign_constants);
    if (tile_prime_geo_bits != nullptr) {
      tile_prime_geo_bits[i] = bits;
    }
  }
  __syncthreads();

  // CORRECTNESS: DO NOT PARALLELIZE. CPU dense-remap labels are assigned by
  // first appearance while scanning compressed roots in ascending prime index.
  // Root-ID sorting or atomic counter schemes permute TileOp group labels.
  if (tid == 0 && tile_wire_label_by_raw_root != nullptr) {
    std::uint16_t next_label = 1;
    bool overflow = false;
    for (int i = 0; i < bounded; ++i) {
      const std::uint16_t raw_root = tile_parent[i];
      if (tile_wire_label_by_raw_root[raw_root] != 0) {
        continue;
      }
      if (next_label > MAX_GROUPS_PER_TILE) {
        overflow = true;
        break;
      }
      tile_wire_label_by_raw_root[raw_root] = next_label;
      ++next_label;
    }
    if (d_max_label != nullptr) {
      d_max_label[tile_idx] = static_cast<std::uint16_t>(next_label - 1);
    }
    if (d_overflow != nullptr) {
      d_overflow[tile_idx] = overflow ? 1 : 0;
    }
  }
  __syncthreads();

  if (tile_wire_label_by_raw_root == nullptr || tile_group_flags == nullptr) {
    return;
  }
  if (d_overflow != nullptr && d_overflow[tile_idx] != 0) {
    return;
  }

  for (int i = tid; i < bounded; i += BLOCK_THREADS) {
    const std::uint16_t label = tile_wire_label_by_raw_root[tile_parent[i]];
    if (label == 0 || label > 256) {
      continue;
    }
    const std::uint8_t bits =
        tile_prime_geo_bits == nullptr ? 0 : (tile_prime_geo_bits[i] & 0x3U);
    if (bits != 0) {
      atomic_or_u8(tile_group_flags + (label - 1), bits);
    }
  }
}

}  // namespace

void launch_kernel_uf_v2(const UfBuffers& buffers,
                         int num_tiles,
                         cudaStream_t stream) {
  kernel_uf_v2<<<num_tiles, BLOCK_THREADS, 0, stream>>>(
      buffers.in.d_bitmap, buffers.in.d_row_prefix, buffers.in.d_prime_pos,
      buffers.in.d_prime_count, buffers.in.d_coords, buffers.out.d_parent,
      buffers.out.d_prime_geo_bits, buffers.out.d_wire_label_by_raw_root,
      buffers.out.d_max_label, buffers.out.d_overflow,
      buffers.out.d_group_flags, num_tiles);
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
