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
  std::array<std::vector<std::uint16_t>, campaign::NUM_FACES> face_roots;
  std::array<std::uint16_t, campaign::NUM_FACES> face_rep_counts{};
  std::array<std::vector<cuda_campaign::FaceRepresentative>, campaign::NUM_FACES>
      face_reps;
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
  std::uint16_t* d_parent = nullptr;
  std::uint16_t* d_wire_label_by_raw_root = nullptr;
  cuda_campaign::TileOp* d_tileops = nullptr;
  std::uint16_t* d_face_indices = nullptr;
  std::uint16_t* d_face_counts = nullptr;
  std::uint16_t* d_face_roots = nullptr;
  cuda_campaign::FaceRepresentative* d_face_reps = nullptr;
  std::uint16_t* d_face_rep_counts = nullptr;

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
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_parent),
                        sizeof(std::uint16_t) *
                            cuda_campaign::MAX_PRIMES_GPU),
             "cudaMalloc parent");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_wire_label_by_raw_root),
                        sizeof(std::uint16_t) *
                            cuda_campaign::MAX_PRIMES_GPU),
             "cudaMalloc wire_label_by_raw_root");
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
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_face_roots),
                        sizeof(std::uint16_t) * campaign::NUM_FACES *
                            cuda_campaign::MAX_PRIMES_GPU),
             "cudaMalloc face_roots");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_face_reps),
                        sizeof(cuda_campaign::FaceRepresentative) *
                            campaign::NUM_FACES *
                            cuda_campaign::MAX_PRIMES_GPU),
             "cudaMalloc face_reps");
  check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_face_rep_counts),
                        sizeof(std::uint16_t) * campaign::NUM_FACES),
             "cudaMalloc face_rep_counts");

  std::array<std::uint32_t, cuda_campaign::MAX_PRIMES_GPU> padded_pos{};
  std::array<std::uint16_t, cuda_campaign::MAX_PRIMES_GPU> padded_parent{};
  std::array<std::uint16_t, cuda_campaign::MAX_PRIMES_GPU> padded_wire_label{};
  if (prime_pos.size() > padded_pos.size()) {
    throw std::runtime_error("fixture exceeds MAX_PRIMES_GPU");
  }
  std::copy(prime_pos.begin(), prime_pos.end(), padded_pos.begin());
  for (std::uint16_t i = 0; i < prime_pos.size(); ++i) {
    padded_parent[i] = i;
    padded_wire_label[i] =
        static_cast<std::uint16_t>((i % campaign::MAX_GROUPS_PER_TILE) + 1);
  }
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
  check_cuda(cudaMemcpy(d_parent, padded_parent.data(),
                        sizeof(std::uint16_t) * padded_parent.size(),
                        cudaMemcpyHostToDevice),
             "cudaMemcpy parent");
  check_cuda(cudaMemcpy(d_wire_label_by_raw_root, padded_wire_label.data(),
                        sizeof(std::uint16_t) * padded_wire_label.size(),
                        cudaMemcpyHostToDevice),
             "cudaMemcpy wire_label_by_raw_root");
  check_cuda(cudaMemset(d_tileops, 0xa5, sizeof(cuda_campaign::TileOp)),
             "cudaMemset tileops");
  check_cuda(cudaMemset(d_face_indices, 0, sizeof(std::uint16_t) *
                                           campaign::NUM_FACES *
                                           cuda_campaign::MAX_PRIMES_GPU),
             "cudaMemset face_indices");
  check_cuda(cudaMemset(d_face_counts, 0,
                        sizeof(std::uint16_t) * campaign::NUM_FACES),
             "cudaMemset face_counts");
  check_cuda(cudaMemset(d_face_roots, 0, sizeof(std::uint16_t) *
                                        campaign::NUM_FACES *
                                        cuda_campaign::MAX_PRIMES_GPU),
             "cudaMemset face_roots");
  check_cuda(cudaMemset(d_face_reps, 0,
                        sizeof(cuda_campaign::FaceRepresentative) *
                            campaign::NUM_FACES *
                            cuda_campaign::MAX_PRIMES_GPU),
             "cudaMemset face_reps");
  check_cuda(cudaMemset(d_face_rep_counts, 0,
                        sizeof(std::uint16_t) * campaign::NUM_FACES),
             "cudaMemset face_rep_counts");

  cuda_campaign::FaceEncodeBuffers buffers{};
  buffers.in.d_coords = d_coords;
  buffers.in.d_prime_pos = d_prime_pos;
  buffers.in.d_prime_count = d_prime_count;
  buffers.in.d_remap_overflow = d_remap_overflow;
  buffers.in.d_parent = d_parent;
  buffers.in.d_wire_label_by_raw_root = d_wire_label_by_raw_root;
  buffers.d_tileops = d_tileops;
  buffers.debug.d_face_indices = d_face_indices;
  buffers.debug.d_face_counts = d_face_counts;
  buffers.debug.d_face_roots = d_face_roots;
  buffers.debug.d_face_reps = d_face_reps;
  buffers.debug.d_face_rep_counts = d_face_rep_counts;
  cuda_campaign::launch_kernel_face_encode_v2(buffers, 1);
  check_cuda(cudaGetLastError(), "launch_kernel_face_encode_v2");
  check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize");

  cuda_campaign::TileOp gpu_tileop{};
  std::array<std::uint16_t, campaign::NUM_FACES> counts{};
  std::array<std::uint16_t,
             campaign::NUM_FACES * cuda_campaign::MAX_PRIMES_GPU>
      indices{};
  std::array<std::uint16_t,
             campaign::NUM_FACES * cuda_campaign::MAX_PRIMES_GPU>
      roots{};
  std::array<cuda_campaign::FaceRepresentative,
             campaign::NUM_FACES * cuda_campaign::MAX_PRIMES_GPU>
      reps{};
  std::array<std::uint16_t, campaign::NUM_FACES> rep_counts{};
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
  check_cuda(cudaMemcpy(roots.data(), d_face_roots,
                        sizeof(std::uint16_t) * roots.size(),
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy face_roots");
  check_cuda(cudaMemcpy(reps.data(), d_face_reps,
                        sizeof(cuda_campaign::FaceRepresentative) *
                            reps.size(),
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy face_reps");
  check_cuda(cudaMemcpy(rep_counts.data(), d_face_rep_counts,
                        sizeof(std::uint16_t) * rep_counts.size(),
                        cudaMemcpyDeviceToHost),
             "cudaMemcpy face_rep_counts");

  check_cuda(cudaFree(d_coords), "cudaFree coords");
  check_cuda(cudaFree(d_prime_pos), "cudaFree prime_pos");
  check_cuda(cudaFree(d_prime_count), "cudaFree prime_count");
  check_cuda(cudaFree(d_remap_overflow), "cudaFree remap_overflow");
  check_cuda(cudaFree(d_parent), "cudaFree parent");
  check_cuda(cudaFree(d_wire_label_by_raw_root),
             "cudaFree wire_label_by_raw_root");
  check_cuda(cudaFree(d_tileops), "cudaFree tileops");
  check_cuda(cudaFree(d_face_indices), "cudaFree face_indices");
  check_cuda(cudaFree(d_face_counts), "cudaFree face_counts");
  check_cuda(cudaFree(d_face_roots), "cudaFree face_roots");
  check_cuda(cudaFree(d_face_reps), "cudaFree face_reps");
  check_cuda(cudaFree(d_face_rep_counts), "cudaFree face_rep_counts");

  K5DebugResult out;
  out.tileop = as_cpu_tileop(gpu_tileop);
  out.face_counts = counts;
  out.face_rep_counts = rep_counts;
  for (int face = 0; face < campaign::NUM_FACES; ++face) {
    const std::uint16_t count = counts[face];
    out.face_indices[face].reserve(count);
    out.face_roots[face].reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
      out.face_indices[face].push_back(
          indices[face * cuda_campaign::FACE_INDEX_STRIDE + i]);
      out.face_roots[face].push_back(
          roots[face * cuda_campaign::FACE_ROOT_STRIDE + i]);
    }
    const std::uint16_t rep_count = rep_counts[face];
    out.face_reps[face].reserve(rep_count);
    for (std::uint16_t i = 0; i < rep_count; ++i) {
      out.face_reps[face].push_back(
          reps[face * cuda_campaign::FACE_REP_STRIDE + i]);
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

std::int64_t face_h(const campaign::Prime& p,
                    const campaign::TileCoord& coord,
                    int face) {
  switch (static_cast<campaign::Face>(face)) {
    case campaign::Face::I:
    case campaign::Face::O:
      return p.a - coord.a_lo;
    case campaign::Face::L:
    case campaign::Face::R:
      return p.b - coord.b_lo;
  }
  return 0;
}

bool within_k_sq(const campaign::Prime& lhs, const campaign::Prime& rhs) {
  const __int128 da = static_cast<__int128>(lhs.a) - rhs.a;
  const __int128 db = static_cast<__int128>(lhs.b) - rhs.b;
  return da * da + db * db <= static_cast<__int128>(campaign::k_sq_value);
}

std::uint16_t find_root(std::vector<std::uint16_t>* parent,
                        std::uint16_t x) {
  std::uint16_t p = x;
  while (p != (*parent)[p]) {
    (*parent)[p] = (*parent)[(*parent)[p]];
    p = (*parent)[p];
  }
  return p;
}

void unite(std::vector<std::uint16_t>* parent,
           std::uint16_t a,
           std::uint16_t b) {
  std::uint16_t ra = find_root(parent, a);
  std::uint16_t rb = find_root(parent, b);
  if (ra == rb) return;
  if (ra > rb) std::swap(ra, rb);
  (*parent)[rb] = ra;
}

std::array<std::vector<std::uint16_t>, campaign::NUM_FACES>
cpu_face_roots_for_order(
    const std::vector<campaign::Prime>& primes,
    const std::array<std::vector<std::uint16_t>, campaign::NUM_FACES>&
        face_indices) {
  std::array<std::vector<std::uint16_t>, campaign::NUM_FACES> out;
  for (int face = 0; face < campaign::NUM_FACES; ++face) {
    std::vector<std::uint16_t> parent(face_indices[face].size());
    for (std::uint16_t i = 0; i < parent.size(); ++i) {
      parent[i] = i;
    }
    for (std::uint16_t i = 0; i < parent.size(); ++i) {
      for (std::uint16_t j = static_cast<std::uint16_t>(i + 1);
           j < parent.size(); ++j) {
        if (within_k_sq(primes[face_indices[face][i]],
                        primes[face_indices[face][j]])) {
          unite(&parent, i, j);
        }
      }
    }
    out[face].reserve(parent.size());
    for (std::uint16_t i = 0; i < parent.size(); ++i) {
      out[face].push_back(find_root(&parent, i));
    }
  }
  return out;
}

std::array<std::vector<cuda_campaign::FaceRepresentative>, campaign::NUM_FACES>
cpu_face_reps_for_order(
    const std::vector<campaign::Prime>& primes,
    const campaign::TileCoord& coord,
    const std::array<std::vector<std::uint16_t>, campaign::NUM_FACES>&
        face_indices,
    const std::array<std::vector<std::uint16_t>, campaign::NUM_FACES>&
        face_roots,
    const std::vector<std::uint16_t>& wire_label_by_prime) {
  std::array<std::vector<cuda_campaign::FaceRepresentative>, campaign::NUM_FACES>
      out;
  for (int face = 0; face < campaign::NUM_FACES; ++face) {
    const std::uint16_t count =
        static_cast<std::uint16_t>(face_indices[face].size());
    for (std::uint16_t root = 0; root < count; ++root) {
      bool have_rep = false;
      std::int64_t best_h = 0;
      std::int64_t best_perp = 0;
      std::uint16_t best_prime_idx = 0;
      std::uint8_t label = 0;

      for (std::uint16_t k = 0; k < count; ++k) {
        if (face_roots[face][k] != root) continue;
        const std::uint16_t prime_idx = face_indices[face][k];
        const campaign::Prime& prime = primes[prime_idx];
        const std::int64_t h = face_h(prime, coord, face);
        const std::int64_t p_perp = face_perp(prime, coord, face);
        if (!have_rep || h < best_h || (h == best_h && p_perp < best_perp)) {
          have_rep = true;
          best_h = h;
          best_perp = p_perp;
          best_prime_idx = prime_idx;
          label = static_cast<std::uint8_t>(
              wire_label_by_prime[prime_idx] & 0xffU);
        }
      }

      if (have_rep) {
        out[face].push_back(cuda_campaign::FaceRepresentative{
            static_cast<std::int16_t>(best_h),
            static_cast<std::int16_t>(best_perp),
            best_prime_idx,
            label,
            0});
      }
    }
  }
  return out;
}

bool same_rep(const cuda_campaign::FaceRepresentative& lhs,
              const cuda_campaign::FaceRepresentative& rhs) {
  return lhs.h == rhs.h && lhs.p_perp == rhs.p_perp &&
         lhs.prime_index == rhs.prime_index &&
         lhs.global_wire_label == rhs.global_wire_label &&
         lhs.reserved == rhs.reserved;
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

    int checked_face_debug = 0;
    bool saw_pending_gpu = false;
    for (const auto& coord : coords) {
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

        const auto cpu_face_indices = cpu_face_indices_for_order(primes, coord);
        const auto cpu_face_roots =
            cpu_face_roots_for_order(primes, cpu_face_indices);
        std::vector<std::uint16_t> wire_label_by_prime(primes.size());
        for (std::uint16_t i = 0; i < primes.size(); ++i) {
          wire_label_by_prime[i] = static_cast<std::uint16_t>(
              (i % campaign::MAX_GROUPS_PER_TILE) + 1);
        }
        const auto cpu_face_reps =
            cpu_face_reps_for_order(primes, coord, cpu_face_indices,
                                    cpu_face_roots, wire_label_by_prime);
        const K5DebugResult gpu = run_k5_fixture(coord, prime_pos, false);
        for (int face = 0; face < campaign::NUM_FACES; ++face) {
          if (gpu.face_indices[face] != cpu_face_indices[face]) {
            std::cerr << "face index mismatch on face " << face << "\n";
            return 1;
          }
          if (gpu.face_roots[face] != cpu_face_roots[face]) {
            std::cerr << "face root mismatch on face " << face << "\n";
            return 1;
          }
          if (gpu.face_reps[face].size() != cpu_face_reps[face].size()) {
            std::cerr << "face representative count mismatch on face "
                      << face << "\n";
            return 1;
          }
          for (std::size_t i = 0; i < cpu_face_reps[face].size(); ++i) {
            if (!same_rep(gpu.face_reps[face][i], cpu_face_reps[face][i])) {
              std::cerr << "face representative mismatch on face " << face
                        << " index " << i << "\n";
              return 1;
            }
          }
        }
        ++checked_face_debug;
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
    if (checked_face_debug != 100) {
      std::cerr << "expected 100 non-empty face-debug tiles, got "
                << checked_face_debug << "\n";
      return 1;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_face_groups_parity: " << e.what() << "\n";
    return 1;
  }
}
