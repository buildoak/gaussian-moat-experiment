#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <cuda_runtime.h>

#include "campaign/constants.h"
#include "cuda_campaign/kernels.cuh"
#include "support/k5_parity_support.h"

namespace {

struct K5DebugResult {
  campaign::TileOp tileop{};
  std::array<std::uint16_t, campaign::NUM_FACES> face_counts{};
  std::array<std::vector<std::uint16_t>, campaign::NUM_FACES> face_indices;
};

void check_cuda(cudaError_t err, const char* what) {
  if (err != cudaSuccess) {
    throw std::runtime_error(std::string(what) + ": " +
                             cudaGetErrorString(err));
  }
}

campaign::TileOp as_cpu_tileop(const cuda_campaign::TileOp& in) {
  campaign::TileOp out{};
  static_assert(sizeof(out) == sizeof(in));
  std::memcpy(&out, &in, sizeof(out));
  return out;
}

K5DebugResult run_k5_fixture(const campaign::TileCoord& coord,
                             const std::vector<std::uint32_t>& prime_pos,
                             bool remap_overflow) {
  campaign::TileCoord* d_coords = nullptr;
  std::uint32_t* d_prime_pos = nullptr;
  std::uint32_t* d_prime_count = nullptr;
  std::uint8_t* d_remap_overflow = nullptr;
  cuda_campaign::TileOp* d_tileops = nullptr;
  std::uint16_t* d_face_indices = nullptr;
  std::uint16_t* d_face_counts = nullptr;

  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_coords),
                        sizeof(campaign::TileCoord)),
             "cudaMalloc coords");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_prime_pos),
                        sizeof(std::uint32_t) * cuda_campaign::MAX_PRIMES_GPU),
             "cudaMalloc prime_pos");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_prime_count),
                        sizeof(std::uint32_t)),
             "cudaMalloc prime_count");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_remap_overflow),
                        sizeof(std::uint8_t)),
             "cudaMalloc remap_overflow");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_tileops),
                        sizeof(cuda_campaign::TileOp)),
             "cudaMalloc tileops");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_face_indices),
                        sizeof(std::uint16_t) * campaign::NUM_FACES *
                            cuda_campaign::MAX_PRIMES_GPU),
             "cudaMalloc face_indices");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_face_counts),
                        sizeof(std::uint16_t) * campaign::NUM_FACES),
             "cudaMalloc face_counts");

  std::array<std::uint32_t, cuda_campaign::MAX_PRIMES_GPU> padded_pos{};
  if (prime_pos.size() > padded_pos.size()) {
    throw std::runtime_error("fixture exceeds MAX_PRIMES_GPU");
  }
  std::copy(prime_pos.begin(), prime_pos.end(), padded_pos.begin());
  const std::uint32_t prime_count = static_cast<std::uint32_t>(prime_pos.size());
  const std::uint8_t overflow = remap_overflow ? 1 : 0;

  check_cuda(cudaMemcpy(d_coords, &coord, sizeof(coord), cudaMemcpyHostToDevice),
             "cudaMemcpy coords");
  check_cuda(cudaMemcpy(d_prime_pos, padded_pos.data(),
                        sizeof(std::uint32_t) * padded_pos.size(),
                        cudaMemcpyHostToDevice),
             "cudaMemcpy prime_pos");
  check_cuda(cudaMemcpy(d_prime_count, &prime_count, sizeof(prime_count),
                        cudaMemcpyHostToDevice),
             "cudaMemcpy prime_count");
  check_cuda(cudaMemcpy(d_remap_overflow, &overflow, sizeof(overflow),
                        cudaMemcpyHostToDevice),
             "cudaMemcpy remap_overflow");
  check_cuda(cudaMemset(d_tileops, 0xa5, sizeof(cuda_campaign::TileOp)),
             "cudaMemset tileops");
  check_cuda(cudaMemset(d_face_indices, 0, sizeof(std::uint16_t) *
                                           campaign::NUM_FACES *
                                           cuda_campaign::MAX_PRIMES_GPU),
             "cudaMemset face_indices");
  check_cuda(cudaMemset(d_face_counts, 0,
                        sizeof(std::uint16_t) * campaign::NUM_FACES),
             "cudaMemset face_counts");

  cuda_campaign::FaceEncodeBuffers buffers{};
  buffers.in.d_coords = d_coords;
  buffers.in.d_prime_pos = d_prime_pos;
  buffers.in.d_prime_count = d_prime_count;
  buffers.in.d_remap_overflow = d_remap_overflow;
  buffers.d_tileops = d_tileops;
  buffers.debug.d_face_indices = d_face_indices;
  buffers.debug.d_face_counts = d_face_counts;
  cuda_campaign::launch_kernel_face_encode_v2(buffers, 1);
  check_cuda(cudaGetLastError(), "launch_kernel_face_encode_v2");
  check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize");

  cuda_campaign::TileOp gpu_tileop{};
  std::array<std::uint16_t, campaign::NUM_FACES> counts{};
  std::array<std::uint16_t,
             campaign::NUM_FACES * cuda_campaign::MAX_PRIMES_GPU>
      indices{};
  check_cuda(cudaMemcpy(&gpu_tileop, d_tileops, sizeof(gpu_tileop),
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy tileop");
  check_cuda(cudaMemcpy(counts.data(), d_face_counts,
                        sizeof(std::uint16_t) * counts.size(),
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy face_counts");
  check_cuda(cudaMemcpy(indices.data(), d_face_indices,
                        sizeof(std::uint16_t) * indices.size(),
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy face_indices");

  check_cuda(cudaFree(d_coords), "cudaFree coords");
  check_cuda(cudaFree(d_prime_pos), "cudaFree prime_pos");
  check_cuda(cudaFree(d_prime_count), "cudaFree prime_count");
  check_cuda(cudaFree(d_remap_overflow), "cudaFree remap_overflow");
  check_cuda(cudaFree(d_tileops), "cudaFree tileops");
  check_cuda(cudaFree(d_face_indices), "cudaFree face_indices");
  check_cuda(cudaFree(d_face_counts), "cudaFree face_counts");

  K5DebugResult out;
  out.tileop = as_cpu_tileop(gpu_tileop);
  out.face_counts = counts;
  for (int face = 0; face < campaign::NUM_FACES; ++face) {
    const std::uint16_t count = counts[face];
    out.face_indices[face].reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
      out.face_indices[face].push_back(
          indices[face * cuda_campaign::FACE_INDEX_STRIDE + i]);
    }
  }
  return out;
}

std::int64_t face_perp(const campaign::Prime& p,
                       const campaign::TileCoord& coord,
                       int face) {
  switch (static_cast<campaign::Face>(face)) {
    case campaign::Face::I:
      return p.b - coord.b_lo;
    case campaign::Face::O:
      return p.b - coord.b_lo - campaign::S;
    case campaign::Face::L:
      return p.a - coord.a_lo;
    case campaign::Face::R:
      return p.a - coord.a_lo - campaign::S;
  }
  return 0;
}

std::array<std::vector<std::uint16_t>, campaign::NUM_FACES>
cpu_face_indices_for_order(const std::vector<campaign::Prime>& primes,
                           const campaign::TileCoord& coord) {
  std::array<std::vector<std::uint16_t>, campaign::NUM_FACES> out;
  for (std::uint16_t i = 0; i < primes.size(); ++i) {
    for (int face = 0; face < campaign::NUM_FACES; ++face) {
      const std::int64_t perp = face_perp(primes[i], coord, face);
      if (-static_cast<std::int64_t>(campaign::C) <= perp &&
          perp <= static_cast<std::int64_t>(campaign::C)) {
        out[face].push_back(i);
      }
    }
  }
  return out;
}

}  // namespace

int main() {
  try {
    const campaign::TileCoord synthetic_coord{
        0, 0, campaign::OFFSET_X, campaign::OFFSET_Y};
    const K5DebugResult empty_gpu = run_k5_fixture(synthetic_coord, {}, false);
    if (!k5_parity::same_tileop_bytes(k5_parity::empty_cpu_fixture(),
                                      empty_gpu.tileop)) {
      std::cerr << "empty TileOp mismatch: "
                << k5_parity::tileop_mismatch_summary(
                       k5_parity::empty_cpu_fixture(), empty_gpu.tileop)
                << "\n";
      return 1;
    }

    const K5DebugResult overflow_gpu =
        run_k5_fixture(synthetic_coord, {0}, true);
    if (!k5_parity::same_tileop_bytes(k5_parity::overflow_cpu_fixture(),
                                      overflow_gpu.tileop)) {
      std::cerr << "overflow TileOp mismatch: "
                << k5_parity::tileop_mismatch_summary(
                       k5_parity::overflow_cpu_fixture(), overflow_gpu.tileop)
                << "\n";
      return 1;
    }

    const auto constants = k5_parity::make_constants(10000, 11000);
    const auto coords = k5_parity::first_active_tiles(10000, 11000, 100);
    if (coords.size() != 100) {
      std::cerr << "expected 100 active tiles, got " << coords.size() << "\n";
      return 1;
    }

    bool checked_face_debug = false;
    bool saw_pending_gpu = false;
    for (const auto& coord : coords) {
      if (!checked_face_debug) {
        std::vector<campaign::Prime> primes =
            campaign::sieve_tile(coord, constants);
        if (!primes.empty()) {
          std::sort(primes.begin(), primes.end(),
                    [](const campaign::Prime& lhs,
                       const campaign::Prime& rhs) {
                      return lhs.packed_pos < rhs.packed_pos;
                    });
          std::vector<std::uint32_t> prime_pos;
          prime_pos.reserve(primes.size());
          for (const campaign::Prime& prime : primes) {
            prime_pos.push_back(prime.packed_pos);
          }

          const auto cpu_face_indices =
              cpu_face_indices_for_order(primes, coord);
          const K5DebugResult gpu = run_k5_fixture(coord, prime_pos, false);
          for (int face = 0; face < campaign::NUM_FACES; ++face) {
            if (gpu.face_indices[face] != cpu_face_indices[face]) {
              std::cerr << "face index mismatch on face " << face << "\n";
              return 1;
            }
          }
          checked_face_debug = true;
        }
      }

      const campaign::TileOp cpu = k5_parity::cpu_tileop(coord, constants);
      if (!k5_parity::face_group_padding_zero(cpu)) {
        std::cerr << "CPU oracle emitted non-zero face_groups padding\n";
        return 1;
      }

      const k5_parity::GpuTileOpResult gpu =
          k5_parity::gpu_tileop_or_pending(coord, constants);
      if (!gpu.available) {
        saw_pending_gpu = true;
        continue;
      }
      if (!k5_parity::same_face_payload(cpu, gpu.tileop)) {
        std::cerr << k5_parity::tileop_mismatch_summary(cpu, gpu.tileop)
                  << "\n";
        return 1;
      }
      if (!k5_parity::face_group_padding_zero(gpu.tileop)) {
        std::cerr << "GPU emitted non-zero face_groups padding\n";
        return 1;
      }
    }

    if (saw_pending_gpu) {
      std::cout << "pending GPU: K5 face group parity compared CPU oracle only\n";
    }
    if (!checked_face_debug) {
      std::cerr << "no non-empty tile found for face-index debug check\n";
      return 1;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_face_groups_parity: " << e.what() << "\n";
    return 1;
  }
}
