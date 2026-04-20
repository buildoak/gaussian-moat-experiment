// apps/campaign_main.cpp
//
// Primary driver for the cpp-campaign-v2 reference build.
//
// Usage:
//   campaign_main --help
//   campaign_main --k-sq=N --r-inner=R1 --r-outer=R2 --region <spec> [--out <path>]
//
// `--region full-octant` is a CLI shortcut for the JSON { "full_octant": true }.
// Any other `--region <path>` argument is treated as a JSON file path and
// parsed per `Region::from_json_file`.
//
// Phase 1 scope (per execution plan §9 punch-list + task brief):
//   * --help works and exits 0.
//   * When supplied with --k-sq / --r-inner / --r-outer / --region, builds
//     the Grid and (for full-octant) resolves the Region and prints the
//     active tile count.
//   * `process_tile` and compositor are stubs, so no snapshot is written.
//     --out is parsed but ignored in Phase 1.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <string>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/region.h"

namespace {

void print_help(const char* prog) {
  std::cout
      << "Usage: " << prog << " [OPTIONS]\n"
      << "\n"
      << "Options:\n"
      << "  --help                 Show this help and exit\n"
      << "  --k-sq=N               Squared step bound K (must match compile-time K_SQ)\n"
      << "  --r-inner=R            Inner radius of the annulus (positive integer)\n"
      << "  --r-outer=R            Outer radius of the annulus (> R_inner)\n"
      << "  --region <spec>        'full-octant' or a path to a region JSON file\n"
      << "  --out <path>           (Phase 2) Output snapshot.bin path\n"
      << "\n"
      << "Compile-time constants (baked in at build):\n"
      << "  K_SQ           = " << campaign::k_sq_value << "\n"
      << "  S              = " << campaign::S << "\n"
      << "  TILEOP_SIZE    = " << campaign::TILEOP_SIZE << "\n"
      << "  C (collar)     = " << campaign::C << "\n"
      << "  offset         = (" << campaign::OFFSET_X << ", "
      << campaign::OFFSET_Y << ")\n"
      << "\n"
      << "Phase 1 status: grid enumeration + region parsing implemented.\n"
      << "                sieve / UF / tileop / compositor / snapshot are stubs.\n";
}

// Accept --k-sq=N, --k-sq N, --r-inner=N, etc. Returns (key, value) or empty.
struct Arg {
  std::string key;
  std::string value;
};

bool parse_uint64(const std::string& s, std::uint64_t& out) {
  try {
    std::size_t pos = 0;
    unsigned long long v = std::stoull(s, &pos);
    if (pos != s.size()) return false;
    out = static_cast<std::uint64_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  // Defaults
  std::optional<std::uint64_t> k_sq;
  std::optional<std::uint64_t> r_inner;
  std::optional<std::uint64_t> r_outer;
  std::optional<std::string> region_spec;
  std::optional<std::string> out_path;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      print_help(argv[0]);
      return 0;
    }

    // Allow --key=value or --key value
    auto take_val = [&](const std::string& flag, std::string& dst) -> bool {
      if (a.rfind(flag + "=", 0) == 0) {
        dst = a.substr(flag.size() + 1);
        return true;
      }
      if (a == flag) {
        if (i + 1 >= argc) {
          std::cerr << "Error: " << flag << " needs a value\n";
          return false;
        }
        dst = argv[++i];
        return true;
      }
      return false;
    };

    std::string val;
    if (take_val("--k-sq", val)) {
      std::uint64_t v;
      if (!parse_uint64(val, v)) {
        std::cerr << "Error: invalid --k-sq value: " << val << "\n";
        return 2;
      }
      k_sq = v;
    } else if (take_val("--r-inner", val)) {
      std::uint64_t v;
      if (!parse_uint64(val, v)) {
        std::cerr << "Error: invalid --r-inner value: " << val << "\n";
        return 2;
      }
      r_inner = v;
    } else if (take_val("--r-outer", val)) {
      std::uint64_t v;
      if (!parse_uint64(val, v)) {
        std::cerr << "Error: invalid --r-outer value: " << val << "\n";
        return 2;
      }
      r_outer = v;
    } else if (take_val("--region", val)) {
      region_spec = val;
    } else if (take_val("--out", val)) {
      out_path = val;
    } else {
      std::cerr << "Error: unknown argument: " << a << "\n";
      std::cerr << "Run " << argv[0] << " --help for usage.\n";
      return 2;
    }
  }

  // No core args? Show help and exit success (CI smoke test uses --help).
  if (!k_sq.has_value() && !r_inner.has_value() && !r_outer.has_value() &&
      !region_spec.has_value()) {
    print_help(argv[0]);
    return 0;
  }

  if (!k_sq.has_value()) {
    std::cerr << "Error: --k-sq is required\n";
    return 2;
  }
  if (!r_inner.has_value()) {
    std::cerr << "Error: --r-inner is required\n";
    return 2;
  }
  if (!r_outer.has_value()) {
    std::cerr << "Error: --r-outer is required\n";
    return 2;
  }
  if (!region_spec.has_value()) {
    std::cerr << "Error: --region is required\n";
    return 2;
  }

  if (static_cast<std::uint32_t>(*k_sq) !=
      static_cast<std::uint32_t>(campaign::k_sq_value)) {
    std::cerr << "Error: --k-sq=" << *k_sq
              << " does not match compile-time K_SQ=" << campaign::k_sq_value
              << "\n";
    return 2;
  }

  // Build constants + grid.
  campaign::CampaignConstants constants;
  try {
    constants = campaign::CampaignConstants::from_radii(
        *r_inner, *r_outer, static_cast<std::uint32_t>(*k_sq));
  } catch (const std::exception& e) {
    std::cerr << "Error building CampaignConstants: " << e.what() << "\n";
    return 3;
  }

  std::cout << "canonical_hash: " << constants.canonical_hash() << "\n";
  std::cout << "mr_witness_sha256: "
            << campaign::CampaignConstants::mr_witness_set_sha256() << "\n";
  std::cout << "annulus_thickness_ok: "
            << (constants.verify_annulus_thickness() ? "yes" : "no") << "\n";

  campaign::Grid grid;
  try {
    grid = campaign::Grid::build(*r_inner, *r_outer,
                                 static_cast<std::uint32_t>(*k_sq));
  } catch (const std::exception& e) {
    std::cerr << "Error building Grid: " << e.what() << "\n";
    return 3;
  }

  std::cout << "grid.i_min: " << grid.i_min << "\n";
  std::cout << "grid.i_max: " << grid.i_max << "\n";
  std::cout << "grid.total_tiles: " << grid.total_tiles << "\n";

  // Resolve region.
  campaign::Region region;
  if (*region_spec == "full-octant") {
    region = campaign::Region::full_octant(grid);
  } else {
    try {
      region = campaign::Region::from_json_file(*region_spec);
    } catch (const std::exception& e) {
      std::cerr << "Error parsing region: " << e.what() << "\n";
      return 4;
    }
    if (region.is_full_octant) {
      region = campaign::Region::full_octant(grid);
    }
  }

  const std::int64_t region_tiles = region.tile_count();
  std::cout << "region.tile_count: " << region_tiles << "\n";

  // Phase 1 sanity: enumerate active tiles from the grid and report count.
  const auto tiles = grid.enumerate_active_tiles();
  std::cout << "grid.enumerate_active_tiles.size: " << tiles.size() << "\n";

  if (out_path.has_value()) {
    std::cout << "(Phase 2) Would write snapshot to: " << *out_path << "\n";
  }

  std::cout << "Phase 1 enumeration complete.\n";
  return 0;
}
