#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <cuda_runtime.h>

#include "cuda_campaign/face_sort_pack.cuh"

namespace {

struct ExpectedFacePayload {
  std::array<std::uint8_t, cuda_campaign::NUM_FACES> n{};
  std::array<std::uint8_t, cuda_campaign::MAX_PORTS_PER_TILE> face_groups{};
};

void check_cuda(cudaError_t err, const char* what) {
  if (err != cudaSuccess) {
    throw std::runtime_error(std::string(what) + ": " +
                             cudaGetErrorString(err));
  }
}

bool rep_less(const cuda_campaign::FaceRepresentative& lhs,
              const cuda_campaign::FaceRepresentative& rhs) {
  if (lhs.h != rhs.h) return lhs.h < rhs.h;
  if (lhs.p_perp != rhs.p_perp) return lhs.p_perp < rhs.p_perp;
  return lhs.global_wire_label < rhs.global_wire_label;
}

std::uint32_t lcg_next(std::uint32_t* state) {
  *state = (*state * 1664525u) + 1013904223u;
  return *state;
}

ExpectedFacePayload build_expected(
    const std::vector<cuda_campaign::FaceRepresentative>& reps,
    const std::array<std::uint16_t, cuda_campaign::NUM_FACES>& counts,
    int tile_idx) {
  ExpectedFacePayload expected;
  int write_offset = 0;
  for (int face = 0; face < cuda_campaign::NUM_FACES; ++face) {
    const std::uint16_t count = counts[face];
    expected.n[face] = static_cast<std::uint8_t>(count);

    std::vector<cuda_campaign::FaceRepresentative> face_reps;
    face_reps.reserve(count);
    const std::size_t face_base =
        static_cast<std::size_t>(tile_idx) * cuda_campaign::NUM_FACES *
            cuda_campaign::FACE_REP_STRIDE +
        static_cast<std::size_t>(face) * cuda_campaign::FACE_REP_STRIDE;
    for (std::uint16_t i = 0; i < count; ++i) {
      face_reps.push_back(reps[face_base + i]);
    }
    std::sort(face_reps.begin(), face_reps.end(), rep_less);

    for (const auto& rep : face_reps) {
      expected.face_groups[write_offset] = rep.global_wire_label;
      ++write_offset;
    }
  }
  return expected;
}

void assert_payload_matches(const ExpectedFacePayload& expected,
                            const cuda_campaign::TileOp& actual,
                            int fixture_idx) {
  if (std::memcmp(expected.n.data(), actual.n, expected.n.size()) != 0) {
    std::cerr << "n[4] mismatch in fixture " << fixture_idx << "\n";
    throw std::runtime_error("n[4] mismatch");
  }
  if (std::memcmp(expected.face_groups.data(), actual.face_groups,
                  expected.face_groups.size()) != 0) {
    for (std::size_t i = 0; i < expected.face_groups.size(); ++i) {
      if (expected.face_groups[i] != actual.face_groups[i]) {
        std::cerr << "face_groups mismatch in fixture " << fixture_idx
                  << " at byte " << i << ": got "
                  << static_cast<int>(actual.face_groups[i]) << ", expected "
                  << static_cast<int>(expected.face_groups[i]) << "\n";
        break;
      }
    }
    throw std::runtime_error("face_groups mismatch");
  }

  int used = 0;
  for (std::uint8_t count : actual.n) {
    used += static_cast<int>(count);
  }
  for (int i = used; i < cuda_campaign::MAX_PORTS_PER_TILE; ++i) {
    if (actual.face_groups[i] != 0) {
      std::cerr << "non-zero padding in fixture " << fixture_idx
                << " at byte " << i << "\n";
      throw std::runtime_error("face_groups padding mismatch");
    }
  }
}

}  // namespace

int main() {
  try {
    constexpr int kFixtureCount = 101;
    const std::size_t reps_len =
        static_cast<std::size_t>(kFixtureCount) * cuda_campaign::NUM_FACES *
        cuda_campaign::FACE_REP_STRIDE;
    const std::size_t counts_len =
        static_cast<std::size_t>(kFixtureCount) * cuda_campaign::NUM_FACES;

    std::vector<cuda_campaign::FaceRepresentative> reps(reps_len);
    std::vector<std::uint16_t> counts_flat(counts_len);
    std::vector<ExpectedFacePayload> expected(kFixtureCount);

    for (int tile = 0; tile < kFixtureCount; ++tile) {
      std::array<std::uint16_t, cuda_campaign::NUM_FACES> counts{};
      if (tile == 0) {
        counts[0] = 4;
        const std::size_t base = 0;
        reps[base + 0] = cuda_campaign::FaceRepresentative{7, -2, 0, 11, 0};
        reps[base + 1] = cuda_campaign::FaceRepresentative{4, 0, 1, 9, 0};
        reps[base + 2] = cuda_campaign::FaceRepresentative{4, 0, 2, 3, 0};
        reps[base + 3] = cuda_campaign::FaceRepresentative{4, -1, 3, 12, 0};
      } else {
        std::uint32_t state = static_cast<std::uint32_t>(0x9e3779b9u + tile);
        int total = 0;
        for (int face = 0; face < cuda_campaign::NUM_FACES; ++face) {
          counts[face] = static_cast<std::uint16_t>(lcg_next(&state) % 17u);
          total += counts[face];
        }
        if (total > cuda_campaign::MAX_PORTS_PER_TILE) {
          throw std::runtime_error("fixture generator exceeded port budget");
        }

        for (int face = 0; face < cuda_campaign::NUM_FACES; ++face) {
          const std::size_t face_base =
              static_cast<std::size_t>(tile) * cuda_campaign::NUM_FACES *
                  cuda_campaign::FACE_REP_STRIDE +
              static_cast<std::size_t>(face) * cuda_campaign::FACE_REP_STRIDE;
          for (std::uint16_t i = 0; i < counts[face]; ++i) {
            const auto h = static_cast<std::int16_t>(
                static_cast<int>(lcg_next(&state) % 29u) - 14);
            const auto p_perp = static_cast<std::int16_t>(
                static_cast<int>(lcg_next(&state) % 15u) - 7);
            const auto label =
                static_cast<std::uint8_t>((lcg_next(&state) % 128u) + 1u);
            reps[face_base + i] =
                cuda_campaign::FaceRepresentative{h, p_perp, i, label, 0};
          }
        }
      }

      for (int face = 0; face < cuda_campaign::NUM_FACES; ++face) {
        counts_flat[static_cast<std::size_t>(tile) *
                        cuda_campaign::FACE_COUNT_STRIDE +
                    face] = counts[face];
      }
      expected[tile] = build_expected(reps, counts, tile);
    }

    cuda_campaign::FaceRepresentative* d_reps = nullptr;
    std::uint16_t* d_counts = nullptr;
    cuda_campaign::TileOp* d_tileops = nullptr;
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_reps),
                          sizeof(cuda_campaign::FaceRepresentative) *
                              reps.size()),
               "cudaMalloc reps");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_counts),
                          sizeof(std::uint16_t) * counts_flat.size()),
               "cudaMalloc counts");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_tileops),
                          sizeof(cuda_campaign::TileOp) * kFixtureCount),
               "cudaMalloc tileops");
    check_cuda(cudaMemcpy(d_reps, reps.data(),
                          sizeof(cuda_campaign::FaceRepresentative) *
                              reps.size(),
                          cudaMemcpyHostToDevice),
               "cudaMemcpy reps");
    check_cuda(cudaMemcpy(d_counts, counts_flat.data(),
                          sizeof(std::uint16_t) * counts_flat.size(),
                          cudaMemcpyHostToDevice),
               "cudaMemcpy counts");
    check_cuda(cudaMemset(d_tileops, 0xa5,
                          sizeof(cuda_campaign::TileOp) * kFixtureCount),
               "cudaMemset tileops");

    cuda_campaign::FaceSortPackBuffers buffers{};
    buffers.in.d_face_reps = d_reps;
    buffers.in.d_face_rep_counts = d_counts;
    buffers.d_tileops = d_tileops;
    cuda_campaign::launch_kernel_face_sort_pack(buffers, kFixtureCount);
    check_cuda(cudaGetLastError(), "launch_kernel_face_sort_pack");
    check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize");

    std::vector<cuda_campaign::TileOp> actual(kFixtureCount);
    check_cuda(cudaMemcpy(actual.data(), d_tileops,
                          sizeof(cuda_campaign::TileOp) * actual.size(),
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy tileops");

    check_cuda(cudaFree(d_reps), "cudaFree reps");
    check_cuda(cudaFree(d_counts), "cudaFree counts");
    check_cuda(cudaFree(d_tileops), "cudaFree tileops");

    for (int tile = 0; tile < kFixtureCount; ++tile) {
      assert_payload_matches(expected[tile], actual[tile], tile);
    }

    const std::array<std::uint8_t, 4> collision_labels = {
        actual[0].face_groups[0],
        actual[0].face_groups[1],
        actual[0].face_groups[2],
        actual[0].face_groups[3],
    };
    const std::array<std::uint8_t, 4> expected_collision = {12, 3, 9, 11};
    if (collision_labels != expected_collision) {
      std::cerr << "port sort collision labels were not tie-broken by "
                   "global_wire_label\n";
      return 1;
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_port_sort_collision: " << e.what() << "\n";
    return 1;
  }
}
