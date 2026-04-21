#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
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
};

void print_usage(const char* argv0) {
  std::cerr << "usage: " << argv0
            << " [--r-inner N] [--r-outer N] [--limit N]\n";
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

    std::cout << "cuda_vs_cpu_diff: " << limit
              << " tile(s) matched K1-K4 parent parity\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "cuda_vs_cpu_diff: " << e.what() << "\n";
    return 2;
  }
}
