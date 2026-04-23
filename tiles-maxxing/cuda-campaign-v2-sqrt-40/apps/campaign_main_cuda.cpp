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
#include <future>
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

struct ColumnChunk {
  std::size_t tile_offset = 0;
  std::size_t tile_count = 0;
  std::vector<std::int32_t> columns;
  std::vector<std::size_t> column_offsets;
};

std::vector<ColumnChunk> plan_column_chunks(const campaign::Grid& grid,
                                            std::size_t chunk_size) {
  if (chunk_size == 0) {
    throw std::invalid_argument("--chunk-size must be greater than zero");
  }

  std::vector<ColumnChunk> chunks;
  ColumnChunk current;
  std::size_t next_tile_offset = 0;
  for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
    const auto column_tiles = grid.enumerate_column_tiles(i);
    if (column_tiles.empty()) continue;

    const std::size_t column_tile_count = column_tiles.size();
    if (current.tile_count != 0 &&
        current.tile_count + column_tile_count > chunk_size) {
      chunks.push_back(std::move(current));
      current = ColumnChunk{};
      current.tile_offset = next_tile_offset;
    }
    if (current.tile_count == 0) {
      current.tile_offset = next_tile_offset;
    }
    current.columns.push_back(i);
    current.column_offsets.push_back(current.tile_count);
    current.tile_count += column_tile_count;
    next_tile_offset += column_tile_count;
  }

  if (current.tile_count != 0) {
    chunks.push_back(std::move(current));
  }
  return chunks;
}

void merge_dispatch_stats(cuda_campaign::DispatchStats& total,
                          const cuda_campaign::DispatchStats& part) {
  total.tiles += part.tiles;
  total.chunks += part.chunks;
  total.slabs += part.slabs;
  total.host_chunk_tiles = std::max(total.host_chunk_tiles, part.host_chunk_tiles);
  total.device_slab_tiles =
      std::max(total.device_slab_tiles, part.device_slab_tiles);
  total.phase1_peak_bytes =
      std::max(total.phase1_peak_bytes, part.phase1_peak_bytes);
  total.phase2_peak_bytes =
      std::max(total.phase2_peak_bytes, part.phase2_peak_bytes);
  total.pinned_host_bytes =
      std::max(total.pinned_host_bytes, part.pinned_host_bytes);
  total.stream_count = std::max(total.stream_count, part.stream_count);
  if (total.device_name.empty()) {
    total.device_name = part.device_name;
  }
  total.k1_cand_overflow_count += part.k1_cand_overflow_count;
  total.k4_prime_overflow_count += part.k4_prime_overflow_count;
  total.k4_group_overflow_count += part.k4_group_overflow_count;
  total.k5_port_overflow_count += part.k5_port_overflow_count;
  for (const auto& diag : part.first_overflow_tiles) {
    if (total.first_overflow_tiles.size() >= 10) break;
    total.first_overflow_tiles.push_back(diag);
  }
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

  campaign::Compositor compositor;
  try {
    compositor.init(grid);
  } catch (const std::exception& e) {
    std::cerr << "Error initializing compositor: " << e.what() << "\n";
    return 5;
  }

  const auto encode_start = Clock::now();
  std::vector<campaign::TileOp> tileops(active_tiles.size());
  cuda_campaign::DispatchStats dispatch_stats;
  const std::vector<ColumnChunk> chunks = plan_column_chunks(grid, chunk_size);
  std::size_t max_chunk_tiles = 0;
  for (const ColumnChunk& chunk : chunks) {
    max_chunk_tiles = std::max(max_chunk_tiles, chunk.tile_count);
  }
  std::vector<campaign::TileOp> host_buffers[2] = {
      std::vector<campaign::TileOp>(max_chunk_tiles),
      std::vector<campaign::TileOp>(max_chunk_tiles),
  };
  cuda_campaign::DispatchConfig config;
  config.host_chunk_tiles = chunk_size;

  auto launch_dispatch =
      [&](std::size_t chunk_index,
          int buffer_index) -> std::future<cuda_campaign::DispatchStats> {
    const ColumnChunk& chunk = chunks[chunk_index];
    return std::async(std::launch::async, [&, chunk_index, buffer_index, chunk] {
      cuda_campaign::DispatchStats stats;
      cuda_campaign::dispatch_tile_batch(
          active_tiles.data() + static_cast<std::ptrdiff_t>(chunk.tile_offset),
          chunk.tile_count, constants, host_buffers[buffer_index].data(), config,
          &stats);
      return stats;
    });
  };

  const auto comp_start = Clock::now();
  try {
    if (!chunks.empty()) {
      std::size_t current_chunk = 0;
      int current_buffer = 0;
      auto pending_dispatch = launch_dispatch(current_chunk, current_buffer);

      while (true) {
        const cuda_campaign::DispatchStats chunk_stats = pending_dispatch.get();
        merge_dispatch_stats(dispatch_stats, chunk_stats);

        const std::size_t next_chunk = current_chunk + 1;
        std::optional<std::future<cuda_campaign::DispatchStats>> next_dispatch;
        int next_buffer = current_buffer;
        if (next_chunk < chunks.size()) {
          next_buffer = 1 - current_buffer;
          next_dispatch.emplace(launch_dispatch(next_chunk, next_buffer));
        }

        const ColumnChunk& chunk = chunks[current_chunk];
        std::copy_n(host_buffers[current_buffer].data(), chunk.tile_count,
                    tileops.data() +
                        static_cast<std::ptrdiff_t>(chunk.tile_offset));
        for (std::size_t column_index = 0; column_index < chunk.columns.size();
             ++column_index) {
          compositor.ingest_column(
              chunk.columns[column_index],
              host_buffers[current_buffer].data() +
                  static_cast<std::ptrdiff_t>(chunk.column_offsets[column_index]));
        }

        if (!next_dispatch.has_value()) {
          break;
        }
        pending_dispatch = std::move(*next_dispatch);
        current_chunk = next_chunk;
        current_buffer = next_buffer;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "Error in compositor: " << e.what() << "\n";
    return 5;
  }
  const auto encode_end = Clock::now();
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
            << "  k1_cand_overflow_count: "
            << dispatch_stats.k1_cand_overflow_count << "\n"
            << "  k4_prime_overflow_count: "
            << dispatch_stats.k4_prime_overflow_count << "\n"
            << "  k4_group_overflow_count: "
            << dispatch_stats.k4_group_overflow_count << "\n"
            << "  k5_port_overflow_count: "
            << dispatch_stats.k5_port_overflow_count << "\n"
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
  if (!dispatch_stats.first_overflow_tiles.empty()) {
    std::cout << "OVERFLOW_DIAGNOSTICS:\n";
    for (const auto& diag : dispatch_stats.first_overflow_tiles) {
      std::cout << "  tile=(" << diag.coord.i << "," << diag.coord.j << ")"
                << " candidate_count=" << diag.candidate_count
                << " prime_count=" << diag.prime_count
                << " group_count=" << diag.group_count
                << " ports=[" << diag.port_counts[0] << ","
                << diag.port_counts[1] << "," << diag.port_counts[2]
                << "," << diag.port_counts[3] << "]"
                << " overflow_types="
                << (diag.k1_cand_overflow ? "k1_cand " : "")
                << (diag.k4_prime_overflow ? "k4_prime " : "")
                << (diag.k4_group_overflow ? "k4_group " : "")
                << (diag.k5_port_overflow ? "k5_port " : "") << "\n";
    }
  }
  return 0;
}
