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
#include "campaign/tileop.h"
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

const std::uint8_t* bytes(const campaign::TileOp& op) {
  return reinterpret_cast<const std::uint8_t*>(&op);
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
    for (std::size_t tile_idx = 0; tile_idx < limit; ++tile_idx) {
      const campaign::TileCoord& coord = coords[tile_idx];
      const campaign::TileOp cpu = campaign::process_tile(coord, constants, grid);
      const campaign::TileOp cuda = cuda_campaign::run_stub_passthrough(cpu);

      if (std::memcmp(&cpu, &cuda, sizeof(cpu)) != 0) {
        for (std::size_t byte_idx = 0; byte_idx < sizeof(cpu); ++byte_idx) {
          if (bytes(cpu)[byte_idx] != bytes(cuda)[byte_idx]) {
            std::cerr << "diff at tile " << tile_idx << " (i=" << coord.i
                      << ", j=" << coord.j << "), byte " << byte_idx
                      << ": cpu=" << static_cast<int>(bytes(cpu)[byte_idx])
                      << " cuda=" << static_cast<int>(bytes(cuda)[byte_idx])
                      << "\n";
            return 1;
          }
        }
      }
    }

    std::cout << "cuda_vs_cpu_diff: " << limit << " tile(s) matched\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "cuda_vs_cpu_diff: " << e.what() << "\n";
    return 2;
  }
}
