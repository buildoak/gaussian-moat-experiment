#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/geo_tests.h"
#include "campaign/grid.h"
#include "campaign/sieve.h"
#include "campaign/tileop.h"
#include "campaign/union_find.h"
#include "cuda_campaign/kernels.cuh"

namespace {

struct Options {
  std::uint64_t r_inner = 100;
  std::uint64_t r_outer = 500;
  std::size_t limit = 1;
  bool m4 = false;
  bool k5 = false;
};

struct M4ExpectedTile {
  std::vector<std::uint8_t> prime_geo_bits;
  std::vector<std::uint16_t> wire_label_by_raw_root;
  std::uint16_t max_label = 0;
  std::uint8_t overflow = 0;
  std::vector<std::uint8_t> group_flags;
};

void print_usage(const char* argv0) {
  std::cerr << "usage: " << argv0
            << " [--r-inner N] [--r-outer N] [--limit N] [--m4] [--k5]\n";
}

bool parse_u64(const char* text, std::uint64_t* out) {
  try {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 10);
    if (text[consumed] != '\0') return false;
    *out = value;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool parse_size(const char* text, std::size_t* out) {
  std::uint64_t value = 0;
  if (!parse_u64(text, &value)) return false;
  *out = static_cast<std::size_t>(value);
  return true;
}

bool parse_args(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (arg == "--m4") {
      options->m4 = true;
      continue;
    }
    if (arg == "--k5") {
      options->k5 = true;
      continue;
    }
    if (i + 1 >= argc) return false;
    if (arg == "--r-inner") {
      if (!parse_u64(argv[++i], &options->r_inner)) return false;
    } else if (arg == "--r-outer") {
      if (!parse_u64(argv[++i], &options->r_outer)) return false;
    } else if (arg == "--limit") {
      if (!parse_size(argv[++i], &options->limit)) return false;
    } else {
      return false;
    }
  }
  return true;
}

bool same_face_payload(const campaign::TileOp& lhs,
                       const campaign::TileOp& rhs) {
  return std::memcmp(lhs.n, rhs.n, sizeof(lhs.n)) == 0 &&
         std::memcmp(lhs.face_groups, rhs.face_groups,
                     sizeof(lhs.face_groups)) == 0;
}

int print_face_payload_diff(const campaign::TileOp& expected,
                            const campaign::TileOp& actual,
                            std::size_t tile_idx,
                            const campaign::TileCoord& coord) {
  for (std::size_t i = 0; i < sizeof(expected.n); ++i) {
    if (expected.n[i] == actual.n[i]) continue;
    std::cerr << "diff at tile " << tile_idx << " (i=" << coord.i
              << ", j=" << coord.j << "), n[" << i << "]: cpu="
              << +expected.n[i] << " cuda=" << +actual.n[i] << "\n";
    return 1;
  }
  for (std::size_t i = 0; i < sizeof(expected.face_groups); ++i) {
    if (expected.face_groups[i] == actual.face_groups[i]) continue;
    std::cerr << "diff at tile " << tile_idx << " (i=" << coord.i
              << ", j=" << coord.j << "), face_groups[" << i
              << "]: cpu=" << +expected.face_groups[i]
              << " cuda=" << +actual.face_groups[i] << "\n";
    return 1;
  }
  return 0;
}

bool within_k_sq(const campaign::Prime& lhs, const campaign::Prime& rhs) {
  const __int128 da =
      static_cast<__int128>(lhs.a) - static_cast<__int128>(rhs.a);
  const __int128 db =
      static_cast<__int128>(lhs.b) - static_cast<__int128>(rhs.b);
  return da * da + db * db <= static_cast<__int128>(campaign::k_sq_value);
}

std::vector<std::int32_t> cpu_parent_roots(
    const std::vector<campaign::Prime>& primes) {
  campaign::DSU dsu(static_cast<std::int32_t>(primes.size()));
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(primes.size()); ++i) {
    for (std::int32_t j = i + 1; j < static_cast<std::int32_t>(primes.size()); ++j) {
      if (within_k_sq(primes[static_cast<std::size_t>(i)],
                      primes[static_cast<std::size_t>(j)])) {
        dsu.unite(i, j);
      }
    }
  }

  std::vector<std::int32_t> roots;
  roots.reserve(primes.size());
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(primes.size()); ++i) {
    roots.push_back(dsu.find(i));
  }
  return roots;
}

std::vector<campaign::Prime> gpu_compact_primes(
    const campaign::TileCoord& coord,
    const cuda_campaign::K1K4DebugDownload& gpu,
    std::size_t tile_idx,
    std::size_t gpu_count) {
  std::vector<campaign::Prime> ordered;
  ordered.reserve(gpu_count);
  for (std::size_t prime_idx = 0; prime_idx < gpu_count; ++prime_idx) {
    const std::size_t offset =
            tile_idx * static_cast<std::size_t>(cuda_campaign::MAX_PRIMES_GPU) +
            prime_idx;
    const std::uint32_t packed_pos = gpu.prime_pos[offset];
    const std::int64_t row =
        static_cast<std::int64_t>(packed_pos / cuda_campaign::SIDE_EXP);
    const std::int64_t col =
        static_cast<std::int64_t>(packed_pos % cuda_campaign::SIDE_EXP);
    const std::int64_t a =
        coord.a_lo + row - static_cast<std::int64_t>(cuda_campaign::C);
    const std::int64_t b =
        coord.b_lo + col - static_cast<std::int64_t>(cuda_campaign::C);
    const std::uint64_t norm_sq =
        static_cast<std::uint64_t>(a * a) + static_cast<std::uint64_t>(b * b);
    ordered.push_back(campaign::Prime{a, b, norm_sq, packed_pos});
  }
  return ordered;
}

std::uint8_t prime_geo_bits(const campaign::Prime& prime,
                            const campaign::CampaignConstants& constants) {
  std::uint8_t bits = 0;
  if (campaign::is_inner_prime(static_cast<std::int64_t>(prime.norm_sq),
                               constants)) {
    bits |= 0x1U;
  }
  if (campaign::is_outer_prime(static_cast<std::int64_t>(prime.norm_sq),
                               constants)) {
    bits |= 0x2U;
  }
  return bits;
}

M4ExpectedTile build_m4_expected_tile(
    const std::vector<campaign::Prime>& primes,
    const std::vector<std::int32_t>& parent,
    const campaign::CampaignConstants& constants) {
  M4ExpectedTile out;
  out.prime_geo_bits.assign(cuda_campaign::MAX_PRIMES_GPU, 0);
  out.wire_label_by_raw_root.assign(cuda_campaign::MAX_PRIMES_GPU, 0);
  out.group_flags.assign(256U, 0);

  for (std::size_t i = 0; i < primes.size(); ++i) {
    out.prime_geo_bits[i] = prime_geo_bits(primes[i], constants);
  }

  std::uint16_t next_label = 1;
  for (std::size_t i = 0; i < parent.size(); ++i) {
    const std::int32_t raw_root = parent[i];
    if (raw_root < 0 ||
        raw_root >= static_cast<std::int32_t>(out.wire_label_by_raw_root.size())) {
      throw std::runtime_error("CPU parent root outside CUDA raw-root range");
    }
    std::uint16_t& label =
        out.wire_label_by_raw_root[static_cast<std::size_t>(raw_root)];
    if (label != 0) continue;
    if (next_label > cuda_campaign::MAX_GROUPS_PER_TILE) {
      out.overflow = 1;
      break;
    }
    label = next_label;
    ++next_label;
  }

  out.max_label = static_cast<std::uint16_t>(next_label - 1);
  if (out.overflow != 0) {
    return out;
  }

  for (std::size_t i = 0; i < parent.size(); ++i) {
    const std::uint16_t label =
        out.wire_label_by_raw_root[static_cast<std::size_t>(parent[i])];
    if (label == 0) continue;
    out.group_flags[static_cast<std::size_t>(label - 1)] |=
        out.prime_geo_bits[i] & 0x3U;
  }
  return out;
}

template <typename T>
int print_array_diff(const char* name,
                     const std::vector<T>& expected,
                     const std::vector<T>& actual,
                     std::size_t actual_offset,
                     std::size_t count,
                     std::size_t tile_idx,
                     const campaign::TileCoord& coord) {
  for (std::size_t i = 0; i < count; ++i) {
    if (expected[i] == actual[actual_offset + i]) continue;
    std::cerr << "diff at tile " << tile_idx << " (i=" << coord.i
              << ", j=" << coord.j << "), " << name << "[" << i
              << "]: cpu=" << +expected[i]
              << " cuda=" << +actual[actual_offset + i] << "\n";
    return 1;
  }
  return 0;
}

template <typename T>
int print_scalar_diff(const char* name,
                      T expected,
                      T actual,
                      std::size_t tile_idx,
                      const campaign::TileCoord& coord) {
  if (expected == actual) return 0;
  std::cerr << "diff at tile " << tile_idx << " (i=" << coord.i
            << ", j=" << coord.j << "), " << name << ": cpu="
            << +expected << " cuda=" << +actual << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_args(argc, argv, &options)) {
    print_usage(argv[0]);
    return 2;
  }

  try {
    const auto constants = campaign::CampaignConstants::from_radii(
        options.r_inner, options.r_outer,
        static_cast<std::uint32_t>(campaign::k_sq_value));
    const auto grid = campaign::Grid::build(
        options.r_inner, options.r_outer,
        static_cast<std::uint32_t>(campaign::k_sq_value));
    const std::vector<campaign::TileCoord> coords = grid.enumerate_active_tiles();

    if (coords.empty()) {
      std::cerr << "grid has no active tiles\n";
      return 2;
    }

    const std::size_t limit = std::min(options.limit, coords.size());
    std::vector<campaign::TileCoord> batch(coords.begin(), coords.begin() + limit);

    if (options.k5) {
      const cuda_campaign::K1K5DebugDownload gpu =
          cuda_campaign::run_k1_to_k5_debug(batch, constants);

      for (std::size_t tile_idx = 0; tile_idx < limit; ++tile_idx) {
        const campaign::TileCoord& coord = coords[tile_idx];
        const campaign::TileOp cpu =
            campaign::process_tile(coord, constants, grid);
        const campaign::TileOp& cuda = gpu.tileops[tile_idx];
        if (!same_face_payload(cpu, cuda)) {
          return print_face_payload_diff(cpu, cuda, tile_idx, coord);
        }
      }

      std::cout << "cuda_vs_cpu_diff: " << limit
                << " tile(s) matched K1-K5 face_groups parity\n";
      return 0;
    }

    const cuda_campaign::K1K4DebugDownload gpu =
        cuda_campaign::run_k1_to_k4_debug(batch, constants);

    for (std::size_t tile_idx = 0; tile_idx < limit; ++tile_idx) {
      const campaign::TileCoord& coord = coords[tile_idx];
      const std::vector<campaign::Prime> primes =
          campaign::sieve_tile(coord, constants);
      const std::vector<std::int32_t> cpu_parent = cpu_parent_roots(primes);

      const std::uint32_t gpu_count = gpu.prime_count[tile_idx];
      if (gpu_count != cpu_parent.size()) {
        std::cerr << "diff at tile " << tile_idx << " (i=" << coord.i
                  << ", j=" << coord.j << "), prime count: cpu="
                  << cpu_parent.size() << " cuda=" << gpu_count << "\n";
        return 1;
      }

      if (options.m4) {
        const std::vector<campaign::Prime> gpu_ordered_primes =
            gpu_compact_primes(coord, gpu, tile_idx, gpu_count);
        const std::vector<std::int32_t> gpu_ordered_cpu_parent =
            cpu_parent_roots(gpu_ordered_primes);
        const M4ExpectedTile expected =
            build_m4_expected_tile(gpu_ordered_primes, gpu_ordered_cpu_parent,
                                   constants);
        const std::size_t prime_offset =
            tile_idx * static_cast<std::size_t>(cuda_campaign::MAX_PRIMES_GPU);
        const std::size_t group_offset = tile_idx * 256U;

        if (print_array_diff("prime_geo_bits", expected.prime_geo_bits,
                             gpu.prime_geo_bits, prime_offset,
                             expected.prime_geo_bits.size(), tile_idx, coord) != 0) {
          return 1;
        }
        if (print_array_diff("wire_label_by_raw_root",
                             expected.wire_label_by_raw_root,
                             gpu.wire_label_by_raw_root, prime_offset,
                             expected.wire_label_by_raw_root.size(), tile_idx,
                             coord) != 0) {
          return 1;
        }
        if (print_scalar_diff("max_label", expected.max_label,
                              gpu.max_label[tile_idx], tile_idx, coord) != 0) {
          return 1;
        }
        if (print_scalar_diff("overflow", expected.overflow,
                              gpu.overflow[tile_idx], tile_idx, coord) != 0) {
          return 1;
        }
        if (print_array_diff("group_flags", expected.group_flags,
                             gpu.group_flags, group_offset,
                             expected.group_flags.size(), tile_idx, coord) != 0) {
          return 1;
        }
        continue;
      }

      for (std::size_t prime_idx = 0; prime_idx < cpu_parent.size(); ++prime_idx) {
        const std::size_t offset =
            tile_idx * static_cast<std::size_t>(cuda_campaign::MAX_PRIMES_GPU) +
            prime_idx;
        const std::int32_t gpu_parent =
            static_cast<std::int32_t>(gpu.parent[offset]);
        if (cpu_parent[prime_idx] != gpu_parent) {
          std::cerr << "diff at tile " << tile_idx << " (i=" << coord.i
                    << ", j=" << coord.j << "), prime " << prime_idx
                    << ": cpu_parent=" << cpu_parent[prime_idx]
                    << " cuda_parent=" << gpu_parent << "\n";
          return 1;
        }
      }
    }

    std::cout << "cuda_vs_cpu_diff: " << limit << " tile(s) matched "
              << (options.m4 ? "M4 debug parity" : "K1-K4 parent parity")
              << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "cuda_vs_cpu_diff: " << e.what() << "\n";
    return 2;
  }
}
