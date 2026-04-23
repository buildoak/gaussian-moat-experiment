#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "cuda_campaign/campaign_constants.cuh"
#include "cuda_campaign/i128_sq_leq.cuh"

namespace cuda_campaign {
namespace {

inline void check_cuda(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    std::fprintf(stderr, "%s failed: %s\n", what, cudaGetErrorString(status));
    std::abort();
  }
}

static __device__ __forceinline__ bool abs_leq_u64(std::int64_t value,
                                                   std::uint64_t bound) {
  return abs_i64_as_u64(value) <= bound;
}

static __device__ __forceinline__ std::uint8_t geo_bits_for_norm_sq(
    std::uint64_t norm_sq,
    const DeviceCampaignConstants& constants) {
  std::uint8_t bits = 0;
  const std::int64_t norm = static_cast<std::int64_t>(norm_sq);
  const std::int64_t k = static_cast<std::int64_t>(constants.K_SQ_value);

  if (norm_sq >= constants.R_inner_sq) {
    const std::int64_t eps =
        norm - static_cast<std::int64_t>(constants.R_inner_sq) - k;
    if (abs_leq_u64(eps, constants.prefilter_inner) &&
        i128_sq_leq(eps, constants.four_rin_sq_k_hi,
                    constants.four_rin_sq_k_lo)) {
      bits |= 0x1U;
    }
  }

  if (norm_sq <= constants.R_outer_sq) {
    const std::int64_t eps =
        norm - static_cast<std::int64_t>(constants.R_outer_sq) - k;
    if (abs_leq_u64(eps, constants.prefilter_outer) &&
        i128_sq_leq(eps, constants.four_rout_sq_k_hi,
                    constants.four_rout_sq_k_lo)) {
      bits |= 0x2U;
    }
  }

  return bits;
}

static __device__ __forceinline__ std::uint8_t geo_bits_for_prime_pos(
    std::uint32_t packed_pos,
    std::int64_t a_lo,
    std::int64_t b_lo,
    const DeviceCampaignConstants& constants) {
  const std::int64_t row = static_cast<std::int64_t>(packed_pos / SIDE_EXP);
  const std::int64_t col = static_cast<std::int64_t>(packed_pos % SIDE_EXP);
  const std::int64_t a = a_lo + col - static_cast<std::int64_t>(C);
  const std::int64_t b = b_lo + row - static_cast<std::int64_t>(C);
  const std::uint64_t norm_sq =
      static_cast<std::uint64_t>(a * a) + static_cast<std::uint64_t>(b * b);
  return geo_bits_for_norm_sq(norm_sq, constants);
}

__global__ void kernel_geo_flags(const campaign::TileCoord* __restrict__ d_coords,
                                 const std::uint32_t* __restrict__ d_prime_pos,
                                 const std::uint32_t* __restrict__ d_prime_count,
                                 std::uint8_t* __restrict__ d_prime_geo_bits,
                                 int num_tiles) {
  const int tile_idx = static_cast<int>(blockIdx.x);
  if (tile_idx >= num_tiles) return;

  const int tid = static_cast<int>(threadIdx.x);
  const campaign::TileCoord coord = d_coords[tile_idx];
  const std::uint32_t* tile_prime_pos =
      d_prime_pos + static_cast<std::size_t>(tile_idx) * MAX_PRIMES_GPU;
  std::uint8_t* tile_geo_bits =
      d_prime_geo_bits + static_cast<std::size_t>(tile_idx) * MAX_PRIMES_GPU;

  const std::uint32_t raw_count = d_prime_count[tile_idx];
  const int bounded = raw_count < static_cast<std::uint32_t>(MAX_PRIMES_GPU)
                          ? static_cast<int>(raw_count)
                          : MAX_PRIMES_GPU;

  for (int i = tid; i < bounded; i += BLOCK_THREADS) {
    tile_geo_bits[i] = geo_bits_for_prime_pos(
        tile_prime_pos[i], coord.a_lo, coord.b_lo, c_campaign_constants);
  }
  for (int i = bounded + tid; i < MAX_PRIMES_GPU; i += BLOCK_THREADS) {
    tile_geo_bits[i] = 0;
  }
}

__global__ void kernel_geo_norm_sweep(const std::uint64_t* __restrict__ d_norm_sq,
                                      std::uint8_t* __restrict__ d_geo_bits,
                                      std::size_t count) {
  const std::size_t idx =
      static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (idx >= count) return;
  d_geo_bits[idx] = geo_bits_for_norm_sq(d_norm_sq[idx], c_campaign_constants);
}

}  // namespace

void launch_kernel_geo_flags(const campaign::TileCoord* d_coords,
                             const std::uint32_t* d_prime_pos,
                             const std::uint32_t* d_prime_count,
                             std::uint8_t* d_prime_geo_bits,
                             int num_tiles,
                             cudaStream_t stream) {
  kernel_geo_flags<<<num_tiles, BLOCK_THREADS, 0, stream>>>(
      d_coords, d_prime_pos, d_prime_count, d_prime_geo_bits, num_tiles);
}

void launch_kernel_geo_norm_sweep(const std::uint64_t* d_norm_sq,
                                  std::uint8_t* d_geo_bits,
                                  std::size_t count,
                                  cudaStream_t stream) {
  constexpr int kThreads = 256;
  const int blocks = static_cast<int>((count + kThreads - 1) / kThreads);
  kernel_geo_norm_sweep<<<blocks, kThreads, 0, stream>>>(
      d_norm_sq, d_geo_bits, count);
}

std::vector<std::uint8_t> debug_run_k4_geo_i128_sweep(
    const campaign::CampaignConstants& constants,
    const std::vector<std::uint64_t>& norm_sq_values) {
  if (norm_sq_values.empty()) {
    return {};
  }

  upload_cuda_constants(constants);

  std::uint64_t* d_norm_sq = nullptr;
  std::uint8_t* d_geo_bits = nullptr;
  const std::size_t norm_bytes = norm_sq_values.size() * sizeof(std::uint64_t);
  const std::size_t bits_bytes = norm_sq_values.size() * sizeof(std::uint8_t);

  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_norm_sq), norm_bytes),
             "cudaMalloc(d_norm_sq)");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_geo_bits), bits_bytes),
             "cudaMalloc(d_geo_bits)");
  check_cuda(cudaMemcpy(d_norm_sq, norm_sq_values.data(), norm_bytes,
                        cudaMemcpyHostToDevice),
             "cudaMemcpy(d_norm_sq)");

  launch_kernel_geo_norm_sweep(d_norm_sq, d_geo_bits, norm_sq_values.size());
  check_cuda(cudaGetLastError(), "kernel_geo_norm_sweep launch");
  check_cuda(cudaDeviceSynchronize(), "kernel_geo_norm_sweep sync");

  std::vector<std::uint8_t> out(norm_sq_values.size());
  check_cuda(cudaMemcpy(out.data(), d_geo_bits, bits_bytes,
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy(d_geo_bits)");
  check_cuda(cudaFree(d_geo_bits), "cudaFree(d_geo_bits)");
  check_cuda(cudaFree(d_norm_sq), "cudaFree(d_norm_sq)");
  return out;
}

}  // namespace cuda_campaign
