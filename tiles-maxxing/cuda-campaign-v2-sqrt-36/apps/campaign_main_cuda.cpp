// apps/campaign_main_cuda.cpp
//
// CUDA campaign driver for cuda-campaign-v2.
//
// Forked from cpp-campaign-v2/apps/campaign_main.cpp. Grid construction,
// region clipping, compositor ingestion, and snapshot writing stay on the CPU
// path; only TileOp production is replaced by the current K1-K5 CUDA host
// driver entry point.
//
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/compositor.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/region.h"
#include "campaign/snapshot.h"
#include "campaign/tileop.h"
#include "cuda_campaign/host_driver.h"
#include "cuda_campaign/kernels.cuh"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr std::size_t kDefaultChunkSize = 200000;

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
      << "  --out <path>           Output snapshot.bin path\n"
      << "  --chunk-size=N         Tiles per production host dispatch chunk\n"
      << "  --threads=N            Accepted for CPU CLI compatibility; ignored\n"
      << "\n"
      << "Compile-time constants (baked in at build):\n"
      << "  K_SQ           = " << campaign::k_sq_value << "\n"
      << "  S              = " << campaign::S << "\n"
      << "  TILEOP_SIZE    = " << campaign::TILEOP_SIZE << "\n"
      << "  C (collar)     = " << campaign::C << "\n"
      << "  offset         = (" << campaign::OFFSET_X << ", "
      << campaign::OFFSET_Y << ")\n"
      << "\n"
      << "Runs grid enumeration, CUDA K1-K5 TileOp processing, sequential\n"
      << "compositor ingestion, verdict production, and snapshot emission.\n";
}

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

std::uint64_t annulus_thickness_rhs(std::uint32_t k_sq) {
  const std::uint64_t s = static_cast<std::uint64_t>(campaign::S);
  const std::uint64_t ceil_sqrt_2k =
      static_cast<std::uint64_t>(campaign::ceil_isqrt(2 * k_sq));
  return 2ULL * s * s + 4ULL * s * ceil_sqrt_2k + 4ULL * k_sq;
}

bool annulus_thickness_ok(std::uint64_t r_inner,
                          std::uint64_t r_outer,
                          std::uint32_t k_sq) {
  const std::uint64_t delta = r_outer - r_inner;
  const unsigned __int128 lhs =
      static_cast<unsigned __int128>(delta) * delta;
  return lhs > static_cast<unsigned __int128>(annulus_thickness_rhs(k_sq));
}

campaign::Region resolve_region(const std::string& spec,
                                 const campaign::Grid& grid) {
  if (spec == "full-octant") {
    return campaign::Region::full_octant(grid);
  }

  campaign::Region region = campaign::Region::from_json_file(spec);
  if (region.is_full_octant) {
    return campaign::Region::full_octant(grid);
  }
  return region;
}

campaign::Grid clip_grid_to_region(const campaign::Grid& grid,
                                   const campaign::Region& region) {
  campaign::Grid clipped{};
  clipped.R_inner = grid.R_inner;
  clipped.R_outer = grid.R_outer;
  clipped.R_inner_sq = grid.R_inner_sq;
  clipped.R_outer_sq = grid.R_outer_sq;
  clipped.K_SQ_value = grid.K_SQ_value;
  clipped.S_value = grid.S_value;
  clipped.C_value = grid.C_value;
  clipped.o_x = grid.o_x;
  clipped.o_y = grid.o_y;

  if (region.is_explicit_tile_list) {
    std::vector<campaign::TileCoord> tiles;
    for (const campaign::TileCoord& tile : region.tiles()) {
      if (grid.flat_index(tile.i, tile.j) >= 0) {
        tiles.push_back(tile);
      }
    }

    if (tiles.empty()) {
      clipped.i_min = 0;
      clipped.i_max = -1;
      clipped.total_tiles = 0;
      clipped.tower_offset = {0};
      return clipped;
    }

    clipped.i_min = tiles.front().i;
    clipped.i_max = tiles.back().i;
    const int n_cols = clipped.i_max - clipped.i_min + 1;
    clipped.j_low.assign(static_cast<std::size_t>(n_cols), 0);
    clipped.j_high.assign(static_cast<std::size_t>(n_cols), -1);
    clipped.tower_offset.assign(static_cast<std::size_t>(n_cols + 1), 0);

    std::int64_t running = 0;
    for (int i = clipped.i_min; i <= clipped.i_max; ++i) {
      const std::size_t k = static_cast<std::size_t>(i - clipped.i_min);
      clipped.tower_offset[k] = running;
      bool seen = false;
      for (const campaign::TileCoord& tile : tiles) {
        if (tile.i != i) continue;
        if (!seen) {
          clipped.j_low[k] = tile.j;
          clipped.j_high[k] = tile.j;
          seen = true;
        } else {
          clipped.j_low[k] = std::min(clipped.j_low[k], tile.j);
          clipped.j_high[k] = std::max(clipped.j_high[k], tile.j);
        }
        ++running;
      }
    }
    clipped.tower_offset[static_cast<std::size_t>(n_cols)] = running;
    clipped.total_tiles = running;
    clipped.explicit_tiles = std::move(tiles);
    return clipped;
  }

  int first_i = 0;
  int last_i = -1;
  const int i_lo = std::max(region.i_lo, grid.i_min);
  const int i_hi = std::min(region.i_hi, grid.i_max);
  for (int i = i_lo; i <= i_hi; ++i) {
    const campaign::JRange requested = region.column_slice(i);
    const auto [grid_lo, grid_hi] = grid.column_bounds(i);
    const int lo = std::max(requested.j_lo, grid_lo);
    const int hi = std::min(requested.j_hi, grid_hi);
    if (lo <= hi) {
      if (last_i < first_i) first_i = i;
      last_i = i;
    }
  }

  if (last_i < first_i) {
    clipped.i_min = 0;
    clipped.i_max = -1;
    clipped.total_tiles = 0;
    clipped.tower_offset = {0};
    return clipped;
  }

  clipped.i_min = first_i;
  clipped.i_max = last_i;
  const int n_cols = clipped.i_max - clipped.i_min + 1;
  clipped.j_low.assign(static_cast<std::size_t>(n_cols), 0);
  clipped.j_high.assign(static_cast<std::size_t>(n_cols), -1);
  clipped.tower_offset.assign(static_cast<std::size_t>(n_cols + 1), 0);

  std::int64_t running = 0;
  for (int i = clipped.i_min; i <= clipped.i_max; ++i) {
    const std::size_t k = static_cast<std::size_t>(i - clipped.i_min);
    const campaign::JRange requested = region.column_slice(i);
    const auto [grid_lo, grid_hi] = grid.column_bounds(i);
    const int lo = std::max(requested.j_lo, grid_lo);
    const int hi = std::min(requested.j_hi, grid_hi);
    clipped.tower_offset[k] = running;
    if (lo <= hi) {
      clipped.j_low[k] = lo;
      clipped.j_high[k] = hi;
      running += static_cast<std::int64_t>(hi - lo + 1);
    }
  }
  clipped.tower_offset[static_cast<std::size_t>(n_cols)] = running;
  clipped.total_tiles = running;
  return clipped;
}

const char* verdict_name(campaign::Verdict verdict) noexcept {
  switch (verdict) {
    case campaign::Verdict::kMoat:
      return "MOAT";
    case campaign::Verdict::kSpanning:
      return "SPANNING";
    case campaign::Verdict::kUnknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

std::int64_t elapsed_ms(Clock::time_point start,
                        Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
      .count();
}

std::vector<campaign::TileOp> process_tiles_cuda(
    const std::vector<campaign::TileCoord>& active_tiles,
    const campaign::CampaignConstants& constants,
    std::size_t chunk_size) {
  if (chunk_size == 0) {
    throw std::invalid_argument("--chunk-size must be greater than zero");
  }

  std::vector<campaign::TileOp> tileops(active_tiles.size());
  for (std::size_t offset = 0; offset < active_tiles.size();
       offset += chunk_size) {
    const std::size_t count =
        std::min(chunk_size, active_tiles.size() - offset);
    std::vector<campaign::TileCoord> chunk(
        active_tiles.begin() + static_cast<std::ptrdiff_t>(offset),
        active_tiles.begin() + static_cast<std::ptrdiff_t>(offset + count));

    cuda_campaign::K1K5DebugDownload gpu =
        cuda_campaign::run_k1_to_k5_debug(chunk, constants);
    if (gpu.tileops.size() != count) {
      throw std::runtime_error("K1-K5 debug dispatcher returned wrong count");
    }
    std::copy(gpu.tileops.begin(), gpu.tileops.end(),
              tileops.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return tileops;
}

}  // namespace

int main(int argc, char** argv) {
  const auto total_start = Clock::now();

  std::optional<std::uint64_t> k_sq;
  std::optional<std::uint64_t> r_inner;
  std::optional<std::uint64_t> r_outer;
  std::optional<std::string> region_spec;
  std::optional<std::string> out_path;
  std::size_t chunk_size = kDefaultChunkSize;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      print_help(argv[0]);
      return 0;
    }

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
    } else if (take_val("--chunk-size", val)) {
      std::uint64_t v;
      if (!parse_uint64(val, v) || v == 0) {
        std::cerr << "Error: invalid --chunk-size value: " << val << "\n";
        return 2;
      }
      chunk_size = static_cast<std::size_t>(v);
    } else if (take_val("--threads", val)) {
      std::uint64_t v;
      if (!parse_uint64(val, v) || v == 0) {
        std::cerr << "Error: invalid --threads value: " << val << "\n";
        return 2;
      }
    } else {
      std::cerr << "Error: unknown argument: " << a << "\n";
      std::cerr << "Run " << argv[0] << " --help for usage.\n";
      return 2;
    }
  }

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
  if (!out_path.has_value()) {
    std::cerr << "Error: --out is required\n";
    return 2;
  }

  if (static_cast<std::uint32_t>(*k_sq) !=
      static_cast<std::uint32_t>(campaign::k_sq_value)) {
    std::cerr << "Error: --k-sq=" << *k_sq
              << " does not match compile-time K_SQ=" << campaign::k_sq_value
              << "\n";
    return 2;
  }

  const auto k_sq_u32 = static_cast<std::uint32_t>(*k_sq);
  if (!annulus_thickness_ok(*r_inner, *r_outer, k_sq_u32)) {
    const std::uint64_t delta = *r_outer - *r_inner;
    const std::uint64_t bound =
        static_cast<std::uint64_t>(campaign::floor_isqrt(
            static_cast<std::int64_t>(annulus_thickness_rhs(k_sq_u32))));
    std::cerr << "ERROR: annulus too thin. R_outer - R_inner = " << delta
              << "; required > " << bound << ".\n"
              << "       Pipeline soundness requires (R_outer - R_inner) > "
              << "S*sqrt(2) + 2*sqrt(K).\n";
    return 1;
  }

  campaign::CampaignConstants constants;
  try {
    constants = campaign::CampaignConstants::from_radii(
        *r_inner, *r_outer, k_sq_u32);
  } catch (const std::exception& e) {
    std::cerr << "Error building CampaignConstants: " << e.what() << "\n";
    return 3;
  }

  const auto grid_start = Clock::now();
  campaign::Grid full_grid;
  try {
    full_grid = campaign::Grid::build(*r_inner, *r_outer, k_sq_u32);
  } catch (const std::exception& e) {
    std::cerr << "Error building Grid: " << e.what() << "\n";
    return 3;
  }

  {
    const std::string err = full_grid.verify_invariants();
    if (!err.empty()) {
      std::cerr << "ERROR: Grid invariants failed: " << err << "\n"
                << "       Blueprint §4.3 mandatory check at campaign init.\n";
      return 3;
    }
  }
  const auto grid_end = Clock::now();

  campaign::Region region;
  try {
    region = resolve_region(*region_spec, full_grid);
  } catch (const std::exception& e) {
    std::cerr << "Error parsing region: " << e.what() << "\n";
    return 4;
  }

  campaign::Grid grid = clip_grid_to_region(full_grid, region);
  std::vector<campaign::TileCoord> active_tiles = grid.enumerate_active_tiles();
  std::sort(active_tiles.begin(), active_tiles.end(),
            [](const campaign::TileCoord& a, const campaign::TileCoord& b) {
              if (a.i != b.i) return a.i < b.i;
              return a.j < b.j;
            });

  const auto encode_start = Clock::now();
  std::vector<campaign::TileOp> tileops;
  cuda_campaign::DispatchStats dispatch_stats;
  try {
    cuda_campaign::DispatchConfig config;
    config.host_chunk_tiles = chunk_size;
    tileops =
        cuda_campaign::dispatch_tile_batch(active_tiles, constants, config,
                                           &dispatch_stats);
  } catch (const std::exception& e) {
    std::cerr << "Error in CUDA TileOp processing: " << e.what() << "\n";
    return 5;
  }
  const auto encode_end = Clock::now();

  campaign::Compositor compositor;
  const auto comp_start = Clock::now();
  try {
    compositor.init(grid);
    for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
      const auto column_tiles = grid.enumerate_column_tiles(i);
      if (column_tiles.empty()) continue;
      const std::int64_t offset = grid.flat_index(i, column_tiles.front().j);
      if (offset < 0) {
        throw std::runtime_error("active column has no flat-index base");
      }
      compositor.ingest_column(
          i, tileops.data() + static_cast<std::size_t>(offset));
    }
  } catch (const std::exception& e) {
    std::cerr << "Error in compositor: " << e.what() << "\n";
    return 5;
  }
  const campaign::Verdict verdict = compositor.finalize();
  const auto comp_end = Clock::now();

  const auto snapshot_start = Clock::now();
  try {
    campaign::write_snapshot(std::filesystem::path(*out_path), grid, tileops,
                             constants);
  } catch (const std::exception& e) {
    std::cerr << "Error writing snapshot: " << e.what() << "\n";
    return 6;
  }
  const auto snapshot_end = Clock::now();
  const auto total_end = Clock::now();

  std::cout << "cuda-campaign-v2 v0.1\n"
            << "  K_SQ: " << *k_sq << "\n"
            << "  R_inner: " << *r_inner << ", R_outer: " << *r_outer << "\n"
            << "  offset: (" << campaign::OFFSET_X << ","
            << campaign::OFFSET_Y << ")\n"
            << "  active tiles: " << active_tiles.size() << "\n"
            << "  chunk-size: " << dispatch_stats.host_chunk_tiles << "\n"
            << "  constants_hash: " << constants.canonical_hash() << "\n"
            << "  mr_witness_sha256: "
            << campaign::CampaignConstants::mr_witness_set_sha256() << "\n"
            << "  grid-init:    " << std::setw(6)
            << elapsed_ms(grid_start, grid_end) << " ms\n"
            << "  cuda-k1-k5:   " << std::setw(6)
            << elapsed_ms(encode_start, encode_end) << " ms\n"
            << "  compositor:   " << std::setw(6)
            << elapsed_ms(comp_start, comp_end) << " ms\n"
            << "  snapshot:     " << std::setw(6)
            << elapsed_ms(snapshot_start, snapshot_end) << " ms\n"
            << "  total:        " << std::setw(6)
            << elapsed_ms(total_start, total_end) << " ms\n"
            << "TIMING: t_grid=" << (elapsed_ms(grid_start, grid_end) / 1000.0)
            << "s t_tile_loop="
            << (elapsed_ms(encode_start, encode_end) / 1000.0)
            << "s t_compositor=" << (elapsed_ms(comp_start, comp_end) / 1000.0)
            << "s t_snapshot="
            << (elapsed_ms(snapshot_start, snapshot_end) / 1000.0)
            << "s t_total=" << (elapsed_ms(total_start, total_end) / 1000.0)
            << "s n_tiles=" << active_tiles.size() << "\n"
            << "VERDICT: " << verdict_name(verdict) << "\n";
  return 0;
}
