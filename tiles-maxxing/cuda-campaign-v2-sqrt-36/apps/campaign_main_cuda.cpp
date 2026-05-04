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
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/region.h"
#include "campaign/snapshot.h"
#include "campaign/streaming_compositor.h"
#include "campaign/tileop.h"
#include "cuda_campaign/host_driver.h"

namespace {

using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;

inline constexpr std::size_t kDefaultChunkSize = 200000;
inline constexpr std::size_t kMaxOverflowDiagnostics = 10;

struct ColumnBatch {
  struct Column {
    std::int32_t i = 0;
    std::size_t tile_count = 0;
  };

  std::vector<campaign::TileCoord> tiles;
  std::vector<Column> columns;

  void clear() {
    tiles.clear();
    columns.clear();
  }
};

struct BatchDispatchResult {
  std::vector<ColumnBatch::Column> columns;
  std::vector<campaign::TileOp> tileops;
  cuda_campaign::DispatchStats dispatch;
  Duration cuda = Duration::zero();
  std::uint64_t produced_tiles = 0;
};

struct RunStats {
  std::uint64_t produced_tiles = 0;
  std::uint64_t ingested_tiles = 0;
  std::uint64_t ingested_columns = 0;
  std::uint64_t app_batches = 0;
  std::uint64_t total_chunk_overshoot_tiles = 0;
  std::uint64_t max_chunk_overshoot_tiles = 0;
  bool early_exit_taken = false;
  cuda_campaign::DispatchStats dispatch;
};

struct Timings {
  Duration grid = Duration::zero();
  Duration cuda = Duration::zero();
  Duration compositor = Duration::zero();
  Duration snapshot = Duration::zero();
  Duration total = Duration::zero();
};

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
      << "  --snapshot-out <path>  Optional output snapshot.bin path\n"
      << "  --out <path>           Alias for --snapshot-out\n"
      << "  --chunk-size=N         Max app-level tiles per column-complete batch\n"
      << "  --overlap-compositor   Overlap CUDA dispatch with CPU compositor "
         "ingestion\n"
      << "  --no-early-exit        Ingest all columns even after SPANNING is found\n"
      << "  --timing               Print timing and chunk accounting\n"
      << "  --profile <path>       Write JSON profile; implies --timing\n"
      << "  --trace-spanning       Print/profile first SPANNING provenance\n"
      << "  --overflow-diagnostics Capture first overflow tiles in profile output\n"
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
      << "Runs grid enumeration, CUDA K1-K5 TileOp processing, streaming\n"
      << "compositor ingestion, verdict production, and optional snapshot emission.\n";
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

double seconds(Duration duration) {
  return std::chrono::duration<double>(duration).count();
}

void accumulate_stage_timings(
    cuda_campaign::DispatchStats::StageTimings& dst,
    const cuda_campaign::DispatchStats::StageTimings& src) {
  dst.h2d_seconds += src.h2d_seconds;
  dst.k1_sieve_seconds += src.k1_sieve_seconds;
  dst.mr_seconds += src.mr_seconds;
  dst.compact_seconds += src.compact_seconds;
  dst.uf_seconds += src.uf_seconds;
  dst.face_encode_seconds += src.face_encode_seconds;
  dst.face_sort_pack_seconds += src.face_sort_pack_seconds;
  dst.overflow_summary_seconds += src.overflow_summary_seconds;
  dst.d2h_seconds += src.d2h_seconds;
}

std::int64_t milliseconds(Duration duration) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
      .count();
}

void accumulate_dispatch_stats(cuda_campaign::DispatchStats& dst,
                               const cuda_campaign::DispatchStats& src) {
  dst.tiles += src.tiles;
  dst.chunks += src.chunks;
  dst.slabs += src.slabs;
  dst.host_chunk_tiles = std::max(dst.host_chunk_tiles, src.host_chunk_tiles);
  dst.device_slab_tiles =
      std::max(dst.device_slab_tiles, src.device_slab_tiles);
  dst.phase1_peak_bytes =
      std::max(dst.phase1_peak_bytes, src.phase1_peak_bytes);
  dst.phase2_peak_bytes =
      std::max(dst.phase2_peak_bytes, src.phase2_peak_bytes);
  dst.pinned_host_bytes =
      std::max(dst.pinned_host_bytes, src.pinned_host_bytes);
  dst.stream_count = std::max(dst.stream_count, src.stream_count);
  if (dst.device_name.empty() && !src.device_name.empty()) {
    dst.device_name = src.device_name;
  }
  dst.k1_cand_overflow_count += src.k1_cand_overflow_count;
  dst.k4_prime_overflow_count += src.k4_prime_overflow_count;
  dst.k4_group_overflow_count += src.k4_group_overflow_count;
  dst.k5_port_overflow_count += src.k5_port_overflow_count;
  accumulate_stage_timings(dst.stage_timings, src.stage_timings);

  for (const auto& diag : src.first_overflow_tiles) {
    if (dst.first_overflow_tiles.size() >= kMaxOverflowDiagnostics) break;
    dst.first_overflow_tiles.push_back(diag);
  }
}

std::size_t column_tile_count(const campaign::Grid& grid, std::int32_t i) {
  const auto [jlo, jhi] = grid.column_bounds(i);
  if (jhi < jlo) return 0;
  return static_cast<std::size_t>(jhi - jlo + 1);
}

void append_column_to_batch(ColumnBatch& batch,
                            const campaign::Grid& grid,
                            std::int32_t i,
                            std::size_t tile_count) {
  if (tile_count == 0) return;

  const auto [jlo, jhi] = grid.column_bounds(i);
  batch.columns.push_back(ColumnBatch::Column{i, tile_count});

  const std::int64_t a_lo = static_cast<std::int64_t>(campaign::OFFSET_X) +
                            static_cast<std::int64_t>(campaign::S) * i;
  for (std::int32_t j = jlo; j <= jhi; ++j) {
    campaign::TileCoord tc{};
    tc.i = i;
    tc.j = j;
    tc.a_lo = a_lo;
    tc.b_lo = static_cast<std::int64_t>(campaign::OFFSET_Y) +
              static_cast<std::int64_t>(campaign::S) * j;
    batch.tiles.push_back(tc);
  }
}

void write_profile_json(const std::filesystem::path& path,
                        const std::vector<std::string>& command,
                        std::uint64_t k_sq,
                        std::uint64_t r_inner,
                        std::uint64_t r_outer,
                        const std::string& region_spec,
                        std::size_t chunk_size,
                        std::uint64_t active_tiles,
                        const RunStats& stats,
                        const Timings& timings,
                        campaign::Verdict verdict,
                        bool snapshot_enabled,
                        bool early_exit_enabled,
                        bool trace_spanning,
                        const campaign::SpanningTrace& spanning_trace) {
  nlohmann::json overflow_diags = nlohmann::json::array();
  for (const auto& diag : stats.dispatch.first_overflow_tiles) {
    overflow_diags.push_back({
        {"tile", {{"i", diag.coord.i}, {"j", diag.coord.j}}},
        {"candidate_count", diag.candidate_count},
        {"prime_count", diag.prime_count},
        {"group_count", diag.group_count},
        {"port_counts",
         {diag.port_counts[0], diag.port_counts[1], diag.port_counts[2],
          diag.port_counts[3]}},
        {"overflow_types",
         {{"k1_cand", diag.k1_cand_overflow},
          {"k4_prime", diag.k4_prime_overflow},
          {"k4_group", diag.k4_group_overflow},
          {"k5_port", diag.k5_port_overflow}}},
    });
  }

  nlohmann::json profile = {
      {"schema_version", 1},
      {"command", command},
      {"radii", {{"k_sq", k_sq}, {"r_inner", r_inner}, {"r_outer", r_outer}}},
      {"region", region_spec},
      {"snapshot_enabled", snapshot_enabled},
      {"early_exit_enabled", early_exit_enabled},
      {"early_exit_taken", stats.early_exit_taken},
      {"verdict", verdict_name(verdict)},
      {"chunk",
       {{"target_tiles", chunk_size},
        {"app_batches", stats.app_batches},
        {"dispatcher_chunks", stats.dispatch.chunks},
        {"dispatcher_slabs", stats.dispatch.slabs},
        {"total_overshoot_tiles", stats.total_chunk_overshoot_tiles},
        {"max_overshoot_tiles", stats.max_chunk_overshoot_tiles}}},
      {"tiles",
       {{"active", active_tiles},
        {"produced", stats.produced_tiles},
        {"ingested", stats.ingested_tiles},
        {"columns_ingested", stats.ingested_columns}}},
      {"timings_seconds",
       {{"grid", seconds(timings.grid)},
        {"cuda_k1_k5", seconds(timings.cuda)},
        {"compositor", seconds(timings.compositor)},
        {"snapshot", seconds(timings.snapshot)},
        {"total", seconds(timings.total)}}},
      {"cuda_stage_timings_seconds",
       {{"h2d", stats.dispatch.stage_timings.h2d_seconds},
        {"k1_sieve", stats.dispatch.stage_timings.k1_sieve_seconds},
        {"mr", stats.dispatch.stage_timings.mr_seconds},
        {"compact", stats.dispatch.stage_timings.compact_seconds},
        {"uf", stats.dispatch.stage_timings.uf_seconds},
        {"face_encode", stats.dispatch.stage_timings.face_encode_seconds},
        {"face_sort_pack",
         stats.dispatch.stage_timings.face_sort_pack_seconds},
        {"overflow_summary",
         stats.dispatch.stage_timings.overflow_summary_seconds},
        {"d2h", stats.dispatch.stage_timings.d2h_seconds}}},
      {"overflow_counters",
       {{"k1_cand_overflow_count", stats.dispatch.k1_cand_overflow_count},
        {"k4_prime_overflow_count", stats.dispatch.k4_prime_overflow_count},
        {"k4_group_overflow_count", stats.dispatch.k4_group_overflow_count},
        {"k5_port_overflow_count", stats.dispatch.k5_port_overflow_count}}},
      {"device",
       {{"name", stats.dispatch.device_name},
        {"stream_count", stats.dispatch.stream_count},
        {"host_chunk_tiles", stats.dispatch.host_chunk_tiles},
        {"device_slab_tiles", stats.dispatch.device_slab_tiles},
        {"phase1_peak_bytes", stats.dispatch.phase1_peak_bytes},
        {"phase2_peak_bytes", stats.dispatch.phase2_peak_bytes},
        {"pinned_host_bytes", stats.dispatch.pinned_host_bytes}}},
      {"overflow_diagnostics", overflow_diags},
  };

  if (trace_spanning) {
    profile["spanning_trace"] = {
        {"detected", spanning_trace.detected},
        {"event", spanning_trace.event},
        {"column_i", spanning_trace.column_i},
        {"tile_j", spanning_trace.tile_j},
        {"tile_index", spanning_trace.tile_index},
        {"group_label", spanning_trace.group_label},
        {"lhs_tile_index", spanning_trace.lhs_tile_index},
        {"lhs_group_label", spanning_trace.lhs_group_label},
        {"rhs_tile_index", spanning_trace.rhs_tile_index},
        {"rhs_group_label", spanning_trace.rhs_group_label},
        {"component", spanning_trace.component},
        {"reach_before", spanning_trace.reach_before},
        {"reach_after", spanning_trace.reach_after},
        {"added_bits", spanning_trace.added_bits},
    };
  }

  std::ofstream out(path);
  if (!out.is_open()) {
    throw std::runtime_error("could not open profile output " + path.string());
  }
  out << profile.dump(2) << "\n";
  if (!out.good()) {
    throw std::runtime_error("failed while writing profile output " +
                             path.string());
  }
}

bool ingest_empty_column(std::int32_t i,
                         campaign::StreamingCompositor& compositor,
                         campaign::SnapshotWriter* snapshot_writer,
                         Timings& timings,
                         RunStats& stats,
                         bool early_exit_enabled) {
  const std::span<const campaign::TileOp> empty;
  auto comp_start = Clock::now();
  compositor.ingest_column(i, empty);
  timings.compositor += Clock::now() - comp_start;
  stats.ingested_columns += 1;

  if (snapshot_writer != nullptr) {
    auto snapshot_start = Clock::now();
    snapshot_writer->append_column(i, empty);
    timings.snapshot += Clock::now() - snapshot_start;
  }

  if (early_exit_enabled && compositor.has_spanning()) {
    stats.early_exit_taken = true;
    return true;
  }
  return false;
}

bool flush_batch(ColumnBatch& batch,
                 cuda_campaign::TileBatchDispatcher& dispatcher,
                 campaign::StreamingCompositor& compositor,
                 campaign::SnapshotWriter* snapshot_writer,
                 Timings& timings,
                 RunStats& stats,
                 bool early_exit_enabled) {
  if (batch.tiles.empty()) {
    batch.clear();
    return false;
  }

  std::vector<campaign::TileOp> tileops(batch.tiles.size());
  cuda_campaign::DispatchStats batch_stats;
  auto cuda_start = Clock::now();
  dispatcher.dispatch(batch.tiles.data(), batch.tiles.size(), tileops.data(),
                      &batch_stats);
  timings.cuda += Clock::now() - cuda_start;

  stats.produced_tiles += static_cast<std::uint64_t>(batch.tiles.size());
  stats.app_batches += 1;
  accumulate_dispatch_stats(stats.dispatch, batch_stats);

  std::size_t offset = 0;
  for (const ColumnBatch::Column& column : batch.columns) {
    const std::span<const campaign::TileOp> column_tileops(
        tileops.data() + static_cast<std::ptrdiff_t>(offset),
        column.tile_count);

    auto comp_start = Clock::now();
    compositor.ingest_column(column.i, column_tileops);
    timings.compositor += Clock::now() - comp_start;
    stats.ingested_tiles += static_cast<std::uint64_t>(column.tile_count);
    stats.ingested_columns += 1;

    if (snapshot_writer != nullptr) {
      auto snapshot_start = Clock::now();
      snapshot_writer->append_column(column.i, column_tileops);
      timings.snapshot += Clock::now() - snapshot_start;
    }

    offset += column.tile_count;

    if (early_exit_enabled && compositor.has_spanning()) {
      stats.early_exit_taken = true;
      batch.clear();
      return true;
    }
  }

  batch.clear();
  return false;
}

BatchDispatchResult dispatch_batch(
    ColumnBatch batch,
    cuda_campaign::TileBatchDispatcher& dispatcher) {
  BatchDispatchResult result;
  result.produced_tiles = static_cast<std::uint64_t>(batch.tiles.size());
  result.columns = std::move(batch.columns);
  result.tileops.resize(batch.tiles.size());

  auto cuda_start = Clock::now();
  dispatcher.dispatch(batch.tiles.data(), batch.tiles.size(),
                      result.tileops.data(), &result.dispatch);
  result.cuda = Clock::now() - cuda_start;

  return result;
}

std::future<BatchDispatchResult> launch_dispatch(
    ColumnBatch& batch,
    cuda_campaign::TileBatchDispatcher& dispatcher) {
  ColumnBatch dispatch_batch_input;
  dispatch_batch_input.tiles = std::move(batch.tiles);
  dispatch_batch_input.columns = std::move(batch.columns);
  batch.clear();

  return std::async(std::launch::async,
                    [&dispatcher,
                     batch_input = std::move(dispatch_batch_input)]() mutable {
                      return dispatch_batch(std::move(batch_input), dispatcher);
                    });
}

bool ingest_dispatch_result(BatchDispatchResult result,
                            campaign::StreamingCompositor& compositor,
                            Timings& timings,
                            RunStats& stats,
                            bool early_exit_enabled) {
  timings.cuda += result.cuda;
  stats.produced_tiles += result.produced_tiles;
  stats.app_batches += 1;
  accumulate_dispatch_stats(stats.dispatch, result.dispatch);

  std::size_t offset = 0;
  for (const ColumnBatch::Column& column : result.columns) {
    const std::span<const campaign::TileOp> column_tileops(
        result.tileops.data() + static_cast<std::ptrdiff_t>(offset),
        column.tile_count);

    auto comp_start = Clock::now();
    compositor.ingest_column(column.i, column_tileops);
    timings.compositor += Clock::now() - comp_start;
    stats.ingested_tiles += static_cast<std::uint64_t>(column.tile_count);
    stats.ingested_columns += 1;

    offset += column.tile_count;

    if (early_exit_enabled && compositor.has_spanning()) {
      stats.early_exit_taken = true;
      return true;
    }
  }

  return false;
}

BatchDispatchResult take_in_flight_result(
    std::optional<std::future<BatchDispatchResult>>& in_flight) {
  std::future<BatchDispatchResult> ready = std::move(*in_flight);
  in_flight.reset();
  return ready.get();
}

void discard_in_flight_result(
    std::optional<std::future<BatchDispatchResult>>& in_flight) {
  if (!in_flight.has_value()) {
    return;
  }
  (void)take_in_flight_result(in_flight);
}

bool submit_batch_and_ingest_ready(
    ColumnBatch& batch,
    std::optional<std::future<BatchDispatchResult>>& in_flight,
    cuda_campaign::TileBatchDispatcher& dispatcher,
    campaign::StreamingCompositor& compositor,
    Timings& timings,
    RunStats& stats,
    bool early_exit_enabled) {
  if (batch.tiles.empty()) {
    batch.clear();
    return false;
  }

  if (!in_flight.has_value()) {
    in_flight = launch_dispatch(batch, dispatcher);
    return false;
  }

  BatchDispatchResult ready = take_in_flight_result(in_flight);
  in_flight = launch_dispatch(batch, dispatcher);
  if (ingest_dispatch_result(std::move(ready), compositor, timings, stats,
                             early_exit_enabled)) {
    discard_in_flight_result(in_flight);
    return true;
  }
  return false;
}

bool drain_in_flight_and_ingest(
    std::optional<std::future<BatchDispatchResult>>& in_flight,
    campaign::StreamingCompositor& compositor,
    Timings& timings,
    RunStats& stats,
    bool early_exit_enabled) {
  if (!in_flight.has_value()) {
    return false;
  }

  BatchDispatchResult ready = take_in_flight_result(in_flight);
  return ingest_dispatch_result(std::move(ready), compositor, timings, stats,
                                early_exit_enabled);
}

}  // namespace

int main(int argc, char** argv) {
  const auto total_start = Clock::now();

  std::vector<std::string> command;
  command.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    command.emplace_back(argv[i]);
  }

  std::optional<std::uint64_t> k_sq;
  std::optional<std::uint64_t> r_inner;
  std::optional<std::uint64_t> r_outer;
  std::optional<std::string> region_spec;
  std::optional<std::string> snapshot_out_path;
  std::optional<std::string> out_alias_path;
  std::optional<std::string> profile_path;
  std::size_t chunk_size = kDefaultChunkSize;
  bool no_early_exit = false;
  bool overlap_compositor = false;
  bool overflow_diagnostics = false;
  bool trace_spanning = false;
  bool timing = false;

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
    } else if (take_val("--snapshot-out", val)) {
      snapshot_out_path = val;
    } else if (take_val("--out", val)) {
      out_alias_path = val;
    } else if (take_val("--chunk-size", val)) {
      std::uint64_t v;
      if (!parse_uint64(val, v) || v == 0 ||
          v > std::numeric_limits<std::size_t>::max()) {
        std::cerr << "Error: invalid --chunk-size value: " << val << "\n";
        return 2;
      }
      chunk_size = static_cast<std::size_t>(v);
    } else if (a == "--no-early-exit") {
      no_early_exit = true;
    } else if (a == "--overlap-compositor") {
      overlap_compositor = true;
    } else if (a == "--overflow-diagnostics") {
      overflow_diagnostics = true;
    } else if (a == "--trace-spanning") {
      trace_spanning = true;
    } else if (a == "--timing") {
      timing = true;
    } else if (take_val("--profile", val)) {
      profile_path = val;
      timing = true;
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
      !region_spec.has_value() && !snapshot_out_path.has_value() &&
      !out_alias_path.has_value() && !profile_path.has_value()) {
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

  if (snapshot_out_path.has_value() && out_alias_path.has_value() &&
      *snapshot_out_path != *out_alias_path) {
    std::cerr << "Error: --out and --snapshot-out differ\n";
    return 2;
  }
  const std::optional<std::string> snapshot_path =
      snapshot_out_path.has_value() ? snapshot_out_path : out_alias_path;

  if (overlap_compositor && snapshot_path.has_value()) {
    std::cerr << "Error: --overlap-compositor cannot be used with "
              << "--snapshot-out/--out; snapshot mode is serial\n";
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

  if (region.is_explicit_tile_list) {
    std::cerr << "Error: explicit sparse tile-list regions are not supported "
              << "by campaign_main_cuda streaming mode\n";
    return 4;
  }

  campaign::Grid grid = clip_grid_to_region(full_grid, region);
  if (grid.is_sparse()) {
    std::cerr << "Error: sparse grids are not supported by "
              << "campaign_main_cuda streaming mode\n";
    return 4;
  }

  Timings timings;
  timings.grid = grid_end - grid_start;
  RunStats run_stats;
  const bool snapshot_enabled = snapshot_path.has_value();
  const bool early_exit_enabled = !snapshot_enabled && !no_early_exit;
  campaign::Verdict verdict = campaign::Verdict::kUnknown;
  campaign::SpanningTrace spanning_trace;

  try {
    cuda_campaign::DispatchConfig config;
    config.host_chunk_tiles = chunk_size;
    config.overflow_diagnostics = overflow_diagnostics;
    config.collect_stage_timings = profile_path.has_value();
    config.max_overflow_diagnostics = kMaxOverflowDiagnostics;

    cuda_campaign::TileBatchDispatcher dispatcher(constants, config);
    campaign::StreamingCompositor compositor;
    compositor.init(grid);

    std::unique_ptr<campaign::SnapshotWriter> snapshot_writer;
    if (snapshot_enabled) {
      const auto snapshot_start = Clock::now();
      snapshot_writer = std::make_unique<campaign::SnapshotWriter>(
          std::filesystem::path(*snapshot_path), grid, constants);
      timings.snapshot += Clock::now() - snapshot_start;
    }

    ColumnBatch batch;
    if (overlap_compositor) {
      std::optional<std::future<BatchDispatchResult>> in_flight;
      try {
        for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
          const std::size_t column_tiles = column_tile_count(grid, i);
          if (column_tiles == 0) {
            if (submit_batch_and_ingest_ready(batch, in_flight, dispatcher,
                                              compositor, timings, run_stats,
                                              early_exit_enabled)) {
              break;
            }
            if (drain_in_flight_and_ingest(in_flight, compositor, timings,
                                           run_stats, early_exit_enabled)) {
              break;
            }
            if (ingest_empty_column(i, compositor, nullptr, timings, run_stats,
                                    early_exit_enabled)) {
              break;
            }
            continue;
          }

          if (!batch.tiles.empty() &&
              batch.tiles.size() + column_tiles > chunk_size) {
            if (submit_batch_and_ingest_ready(batch, in_flight, dispatcher,
                                              compositor, timings, run_stats,
                                              early_exit_enabled)) {
              break;
            }
          }

          if (batch.tiles.empty() && column_tiles > chunk_size) {
            const std::uint64_t overshoot =
                static_cast<std::uint64_t>(column_tiles - chunk_size);
            run_stats.total_chunk_overshoot_tiles += overshoot;
            run_stats.max_chunk_overshoot_tiles =
                std::max(run_stats.max_chunk_overshoot_tiles, overshoot);
          }
          append_column_to_batch(batch, grid, i, column_tiles);
        }

        if (!run_stats.early_exit_taken) {
          if (submit_batch_and_ingest_ready(batch, in_flight, dispatcher,
                                            compositor, timings, run_stats,
                                            early_exit_enabled)) {
            // SPANNING was observed while ingesting the penultimate result.
          }
        }
        if (!run_stats.early_exit_taken) {
          if (drain_in_flight_and_ingest(in_flight, compositor, timings,
                                         run_stats, early_exit_enabled)) {
            // SPANNING was observed after the final dispatched column.
          }
        }
      } catch (...) {
        if (in_flight.has_value()) {
          try {
            discard_in_flight_result(in_flight);
          } catch (const std::exception& e) {
            std::cerr << "Error in speculative CUDA dispatch during cleanup: "
                      << e.what() << "\n";
          } catch (...) {
            std::cerr << "Error in speculative CUDA dispatch during cleanup\n";
          }
        }
        throw;
      }
    } else {
      for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
        const std::size_t column_tiles = column_tile_count(grid, i);
        if (column_tiles == 0) {
          if (flush_batch(batch, dispatcher, compositor, snapshot_writer.get(),
                          timings, run_stats, early_exit_enabled)) {
            break;
          }
          if (ingest_empty_column(i, compositor, snapshot_writer.get(), timings,
                                  run_stats, early_exit_enabled)) {
            break;
          }
          continue;
        }

        if (!batch.tiles.empty() &&
            batch.tiles.size() + column_tiles > chunk_size) {
          if (flush_batch(batch, dispatcher, compositor, snapshot_writer.get(),
                          timings, run_stats, early_exit_enabled)) {
            break;
          }
        }

        if (batch.tiles.empty() && column_tiles > chunk_size) {
          const std::uint64_t overshoot =
              static_cast<std::uint64_t>(column_tiles - chunk_size);
          run_stats.total_chunk_overshoot_tiles += overshoot;
          run_stats.max_chunk_overshoot_tiles =
              std::max(run_stats.max_chunk_overshoot_tiles, overshoot);
        }
        append_column_to_batch(batch, grid, i, column_tiles);
      }

      if (!run_stats.early_exit_taken) {
        if (flush_batch(batch, dispatcher, compositor, snapshot_writer.get(),
                        timings, run_stats, early_exit_enabled)) {
          // SPANNING was observed after the final dispatched column.
        }
      }
    }

    if (snapshot_writer != nullptr) {
      const auto snapshot_start = Clock::now();
      snapshot_writer->close();
      timings.snapshot += Clock::now() - snapshot_start;
    }

    verdict = run_stats.early_exit_taken ? campaign::Verdict::kSpanning
                                         : compositor.finalize();
    spanning_trace = compositor.spanning_trace();
  } catch (const std::exception& e) {
    std::cerr << "Error in CUDA streaming campaign: " << e.what() << "\n";
    return 5;
  }
  const auto total_end = Clock::now();
  timings.total = total_end - total_start;

  if (profile_path.has_value()) {
    try {
      write_profile_json(std::filesystem::path(*profile_path), command, *k_sq,
                         *r_inner, *r_outer, *region_spec, chunk_size,
                         static_cast<std::uint64_t>(grid.total_tiles),
                         run_stats, timings, verdict, snapshot_enabled,
                         early_exit_enabled, trace_spanning, spanning_trace);
    } catch (const std::exception& e) {
      std::cerr << "Error writing profile: " << e.what() << "\n";
      return 7;
    }
  }

  std::cout << "cuda-campaign-v2 v0.1\n"
            << "  K_SQ: " << *k_sq << "\n"
            << "  R_inner: " << *r_inner << ", R_outer: " << *r_outer << "\n"
            << "  offset: (" << campaign::OFFSET_X << ","
            << campaign::OFFSET_Y << ")\n"
            << "  active tiles: " << grid.total_tiles << "\n"
            << "  produced tiles: " << run_stats.produced_tiles << "\n"
            << "  ingested tiles: " << run_stats.ingested_tiles << "\n"
            << "  ingested columns: " << run_stats.ingested_columns << "\n"
            << "  chunk-size: " << chunk_size << "\n"
            << "  app batches: " << run_stats.app_batches << "\n"
            << "  chunk_overshoot_tiles: "
            << run_stats.max_chunk_overshoot_tiles << "\n"
            << "  total_chunk_overshoot_tiles: "
            << run_stats.total_chunk_overshoot_tiles << "\n"
            << "  snapshot: "
            << (snapshot_enabled ? *snapshot_path : std::string("disabled"))
            << "\n"
            << "  early-exit: "
            << (early_exit_enabled ? "enabled" : "disabled")
            << (run_stats.early_exit_taken ? " (taken)" : "") << "\n"
            << "  k1_cand_overflow_count: "
            << run_stats.dispatch.k1_cand_overflow_count << "\n"
            << "  k4_prime_overflow_count: "
            << run_stats.dispatch.k4_prime_overflow_count << "\n"
            << "  k4_group_overflow_count: "
            << run_stats.dispatch.k4_group_overflow_count << "\n"
            << "  k5_port_overflow_count: "
            << run_stats.dispatch.k5_port_overflow_count << "\n"
            << "  constants_hash: " << constants.canonical_hash() << "\n"
            << "  mr_witness_sha256: "
            << campaign::CampaignConstants::mr_witness_set_sha256() << "\n";

  if (timing) {
    std::cout << "  grid-init:    " << std::setw(6)
              << milliseconds(timings.grid) << " ms\n"
              << "  cuda-k1-k5:   " << std::setw(6)
              << milliseconds(timings.cuda) << " ms\n"
              << "  compositor:   " << std::setw(6)
              << milliseconds(timings.compositor) << " ms\n"
              << "  snapshot:     "
              << (snapshot_enabled
                      ? (std::to_string(milliseconds(timings.snapshot)) +
                         " ms")
                      : std::string("disabled"))
              << "\n"
              << "  total:        " << std::setw(6)
              << milliseconds(timings.total) << " ms\n"
              << "TIMING: t_grid=" << seconds(timings.grid)
              << "s t_tile_loop=" << seconds(timings.cuda)
              << "s t_compositor=" << seconds(timings.compositor)
              << "s t_snapshot=" << seconds(timings.snapshot)
              << "s t_total=" << seconds(timings.total)
              << "s n_tiles=" << grid.total_tiles
              << " produced_tiles=" << run_stats.produced_tiles
              << " ingested_tiles=" << run_stats.ingested_tiles
              << " app_batches=" << run_stats.app_batches
              << " dispatcher_chunks=" << run_stats.dispatch.chunks
              << " dispatcher_slabs=" << run_stats.dispatch.slabs
              << " chunk_overshoot_tiles="
              << run_stats.max_chunk_overshoot_tiles
              << " total_chunk_overshoot_tiles="
              << run_stats.total_chunk_overshoot_tiles << "\n";
  }

  std::cout << "VERDICT: " << verdict_name(verdict) << "\n";
  if (trace_spanning) {
    std::cout << "SPANNING_TRACE:"
              << " detected=" << (spanning_trace.detected ? 1 : 0)
              << " event=" << spanning_trace.event
              << " column_i=" << spanning_trace.column_i
              << " tile_j=" << spanning_trace.tile_j
              << " tile_index=" << spanning_trace.tile_index
              << " group_label=" << spanning_trace.group_label
              << " lhs_tile_index=" << spanning_trace.lhs_tile_index
              << " lhs_group_label=" << spanning_trace.lhs_group_label
              << " rhs_tile_index=" << spanning_trace.rhs_tile_index
              << " rhs_group_label=" << spanning_trace.rhs_group_label
              << " component=" << spanning_trace.component
              << " reach_before=" << static_cast<int>(spanning_trace.reach_before)
              << " reach_after=" << static_cast<int>(spanning_trace.reach_after)
              << " added_bits=" << static_cast<int>(spanning_trace.added_bits)
              << "\n";
  }
  if (profile_path.has_value() &&
      !run_stats.dispatch.first_overflow_tiles.empty()) {
    std::cout << "OVERFLOW_DIAGNOSTICS:\n";
    for (const auto& diag : run_stats.dispatch.first_overflow_tiles) {
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
