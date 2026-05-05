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
#include <array>
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
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/geo_tests.h"
#include "campaign/grid.h"
#include "campaign/region.h"
#include "campaign/sieve.h"
#include "campaign/snapshot.h"
#include "campaign/streaming_compositor.h"
#include "campaign/tileop.h"
#include "cuda_campaign/host_driver.h"
#include "tileop_internal.h"

#ifndef GAUSSIAN_MOAT_GIT_COMMIT
#define GAUSSIAN_MOAT_GIT_COMMIT "unknown"
#endif

#ifndef GAUSSIAN_MOAT_CUDA_ARCH
#define GAUSSIAN_MOAT_CUDA_ARCH "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;

inline constexpr std::size_t kDefaultChunkSize = 200000;
inline constexpr std::size_t kMaxOverflowDiagnostics = 10;
inline constexpr std::size_t kDefaultAuditTileSampleTarget = 512;
inline constexpr std::size_t kLegacyStatsTileSampleTarget = 512;
inline constexpr std::size_t kStatsV2HighPressureLimit = 32;

enum class TelemetryLevel {
  kNone,
  kProfile,
  kAudit,
  kFull,
};

enum class SampleClass : std::size_t {
  kGeoI = 0,
  kGeoO,
  kAxis,
  kDiagonal,
  kHighPressure,
  kDeterministicRandom,
  kCount,
};

constexpr std::array<SampleClass, static_cast<std::size_t>(SampleClass::kCount)>
    kSampleClassOrder = {
        SampleClass::kGeoI,
        SampleClass::kGeoO,
        SampleClass::kAxis,
        SampleClass::kDiagonal,
        SampleClass::kHighPressure,
        SampleClass::kDeterministicRandom,
};

struct BzRecord {
  bool checked = false;
  bool clean = false;
  bool override_used = false;
  std::uint64_t sqrt_k = 0;
  std::uint64_t bz_i_candidate_count = 0;
  std::uint64_t bz_o_candidate_count = 0;
  std::uint64_t bad_norm_count = 0;
};

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
  std::vector<campaign::TileCoord> coords;
  std::vector<campaign::TileOp> tileops;
  cuda_campaign::DispatchStats dispatch;
  Duration cuda = Duration::zero();
  std::uint64_t produced_tiles = 0;
  std::uint64_t emitted_overflow_tileops = 0;
};

struct ScalarDistribution {
  std::uint64_t count = 0;
  std::uint64_t sum = 0;
  std::uint64_t min = 0;
  std::uint64_t max = 0;
  std::map<std::uint64_t, std::uint64_t> histogram;

  void observe(std::uint64_t value) {
    if (count == 0) {
      min = value;
      max = value;
    } else {
      min = std::min(min, value);
      max = std::max(max, value);
    }
    ++count;
    sum += value;
    ++histogram[value];
  }

  std::uint64_t percentile(int pct) const {
    if (count == 0) return 0;
    const std::uint64_t rank =
        (count * static_cast<std::uint64_t>(pct) + 99ULL) / 100ULL;
    std::uint64_t seen = 0;
    for (const auto& [value, bucket_count] : histogram) {
      seen += bucket_count;
      if (seen >= rank) return value;
    }
    return max;
  }

  nlohmann::json json() const {
    if (count == 0) return nullptr;
    nlohmann::json buckets = nlohmann::json::array();
    for (const auto& [value, bucket_count] : histogram) {
      buckets.push_back({{"value", value}, {"count", bucket_count}});
    }
    return {
        {"buckets", buckets},
        {"observed_min", min},
        {"observed_max", max},
        {"sample_count", count},
        {"total_count", count},
    };
  }
};

struct HighPressureTile {
  std::uint64_t flat_index = 0;
  campaign::TileCoord coord{};
  std::uint64_t pressure_score = 0;
  std::uint64_t group_count = 0;
  std::uint64_t total_port_count = 0;
  std::uint64_t max_face_port_count = 0;
  std::array<std::uint64_t, 4> face_port_counts{};
  bool geo_i = false;
  bool geo_o = false;
  bool overflow = false;
};

bool pressure_tile_better(const HighPressureTile& lhs,
                          const HighPressureTile& rhs) {
  if (lhs.pressure_score != rhs.pressure_score) {
    return lhs.pressure_score > rhs.pressure_score;
  }
  if (lhs.total_port_count != rhs.total_port_count) {
    return lhs.total_port_count > rhs.total_port_count;
  }
  if (lhs.group_count != rhs.group_count) {
    return lhs.group_count > rhs.group_count;
  }
  if (lhs.max_face_port_count != rhs.max_face_port_count) {
    return lhs.max_face_port_count > rhs.max_face_port_count;
  }
  if (lhs.coord.i != rhs.coord.i) return lhs.coord.i < rhs.coord.i;
  return lhs.coord.j < rhs.coord.j;
}

struct AnalyticalTileStats {
  ScalarDistribution group_counts;
  ScalarDistribution total_port_counts;
  ScalarDistribution max_face_port_counts;
  std::uint64_t geo_i_port_population = 0;
  std::uint64_t geo_o_port_population = 0;
  std::vector<HighPressureTile> high_pressure_tiles;

  void consider(const HighPressureTile& tile) {
    group_counts.observe(tile.group_count);
    total_port_counts.observe(tile.total_port_count);
    max_face_port_counts.observe(tile.max_face_port_count);
    geo_i_port_population += tile.face_port_counts[0];
    geo_o_port_population += tile.face_port_counts[1];

    if (tile.pressure_score == 0) return;
    if (high_pressure_tiles.size() < kStatsV2HighPressureLimit) {
      high_pressure_tiles.push_back(tile);
      return;
    }
    auto worst = std::max_element(high_pressure_tiles.begin(),
                                  high_pressure_tiles.end(),
                                  pressure_tile_better);
    if (worst != high_pressure_tiles.end() &&
        pressure_tile_better(tile, *worst)) {
      *worst = tile;
    }
  }

  nlohmann::json high_pressure_json() const {
    std::vector<HighPressureTile> sorted = high_pressure_tiles;
    std::sort(sorted.begin(), sorted.end(), pressure_tile_better);
    nlohmann::json out = nlohmann::json::array();
    for (const HighPressureTile& tile : sorted) {
      nlohmann::json classes = nlohmann::json::array();
      if (tile.group_count > 0) classes.push_back("group");
      if (tile.total_port_count > 0) classes.push_back("total_port");
      if (tile.max_face_port_count > 0) classes.push_back("max_face_port");
      if (tile.geo_i) classes.push_back("geo_i");
      if (tile.geo_o) classes.push_back("geo_o");
      if (tile.overflow) classes.push_back("overflow");
      out.push_back({
          {"tile_index", tile.flat_index},
          {"tile_i", tile.coord.i},
          {"tile_j", tile.coord.j},
          {"score", tile.pressure_score},
          {"pressure_score", tile.pressure_score},
          {"group_count", tile.group_count},
          {"total_port_count", tile.total_port_count},
          {"max_face_port_count", tile.max_face_port_count},
          {"face_port_counts",
           {tile.face_port_counts[0], tile.face_port_counts[1],
            tile.face_port_counts[2], tile.face_port_counts[3]}},
          {"geo_i", tile.geo_i},
          {"geo_o", tile.geo_o},
          {"overflow", tile.overflow},
          {"classes", classes},
      });
    }
    return out;
  }
};

nlohmann::json component_entry_json(
    const campaign::ComponentCensusEntry& entry,
    std::size_t rank) {
  return {
      {"rank", rank},
      {"component_id", entry.component},
      {"tile_count", 0},
      {"group_count", entry.live_group_nodes},
      {"geo_i", (entry.reach & 0x1U) != 0},
      {"geo_o", (entry.reach & 0x2U) != 0},
  };
}

nlohmann::json component_entry_array_json(
    const std::vector<campaign::ComponentCensusEntry>& entries) {
  nlohmann::json out = nlohmann::json::array();
  for (std::size_t idx = 0; idx < entries.size(); ++idx) {
    out.push_back(component_entry_json(entries[idx], idx + 1));
  }
  return out;
}

nlohmann::json component_census_json(
    const campaign::ComponentCensus& census) {
  return {
      {"initialized", census.initialized},
      {"spanning_detected", census.spanning_detected},
      {"live_frontier_only", census.live_frontier_only},
      {"total_components", census.live_components},
      {"i_only_components", census.inner_only_components},
      {"o_only_components", census.outer_only_components},
      {"i_and_o_components", census.inner_outer_components},
      {"neither_components", census.neutral_components},
      {"live_group_nodes", census.live_group_nodes},
      {"largest_component_sizes",
       component_entry_array_json(census.largest_components)},
      {"largest_boundary_touching_components",
       component_entry_array_json(census.largest_boundary_touching_components)},
  };
}

struct RunStats {
  std::uint64_t produced_tiles = 0;
  std::uint64_t ingested_tiles = 0;
  std::uint64_t ingested_columns = 0;
  std::uint64_t app_batches = 0;
  std::uint64_t total_chunk_overshoot_tiles = 0;
  std::uint64_t max_chunk_overshoot_tiles = 0;
  std::uint64_t emitted_overflow_tileops = 0;
  std::uint64_t geo_i_tiles = 0;
  std::uint64_t geo_o_tiles = 0;
  std::uint64_t tile_samples_written = 0;
  bool early_exit_taken = false;
  AnalyticalTileStats analytical;
  cuda_campaign::DispatchStats dispatch;
};

struct SampleCandidate {
  SampleClass sample_class = SampleClass::kDeterministicRandom;
  std::uint64_t rank_hash = 0;
  std::uint64_t pressure_score = 0;
  campaign::TileCoord coord{};
  campaign::TileOp tileop{};
};

struct SampleClassCompare {
  bool operator()(const SampleCandidate& lhs,
                  const SampleCandidate& rhs) const {
    if (lhs.sample_class == SampleClass::kHighPressure ||
        rhs.sample_class == SampleClass::kHighPressure) {
      if (lhs.pressure_score != rhs.pressure_score) {
        return lhs.pressure_score > rhs.pressure_score;
      }
    }
    if (lhs.rank_hash != rhs.rank_hash) return lhs.rank_hash < rhs.rank_hash;
    if (lhs.coord.i != rhs.coord.i) return lhs.coord.i < rhs.coord.i;
    return lhs.coord.j < rhs.coord.j;
  }
};

class SampleReservoir {
 public:
  explicit SampleReservoir(std::size_t capacity) : capacity_(capacity) {}

  void consider(const SampleCandidate& candidate) {
    if (capacity_ == 0) return;
    if (heap_.size() < capacity_) {
      heap_.push_back(candidate);
      std::push_heap(heap_.begin(), heap_.end(), SampleClassCompare{});
      return;
    }
    SampleClassCompare better;
    if (!better(candidate, heap_.front())) return;
    std::pop_heap(heap_.begin(), heap_.end(), SampleClassCompare{});
    heap_.back() = candidate;
    std::push_heap(heap_.begin(), heap_.end(), SampleClassCompare{});
  }

  std::vector<SampleCandidate> sorted() const {
    std::vector<SampleCandidate> out = heap_;
    std::sort(out.begin(), out.end(), SampleClassCompare{});
    return out;
  }

 private:
  std::size_t capacity_ = 0;
  std::vector<SampleCandidate> heap_;
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
      << "  --telemetry <level>    none, profile, audit, or full\n"
      << "  --trace-spanning       Print/profile first SPANNING provenance\n"
      << "  --trace-spanning-path  Reconstruct first SPANNING stitch path\n"
      << "  --stats-level <level>  Legacy alias: profile maps to telemetry profile\n"
      << "  --emit-span-cert <p>   Emit coordinate span cert from reconstructed trace\n"
      << "  --sample-manifest <p>  Record production sample manifest path\n"
      << "  --tile-sample-out <p>  Write sampled production TileOps as JSONL\n"
      << "  --tile-sample-count=N  Stratified audit sample target; default 512\n"
      << "  --sample-seed=N        Override deterministic sample seed\n"
      << "  --allow-uncertified-boundary-band\n"
      << "                         Allow accepted output without clean BZ record\n"
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

bool is_square_u64(std::uint64_t n, std::uint64_t& root) {
  root = static_cast<std::uint64_t>(campaign::floor_isqrt(
      static_cast<std::int64_t>(n)));
  return root * root == n;
}

bool in_bz_i(std::uint64_t n, std::uint64_t r, std::uint64_t sqrt_k) {
  const unsigned __int128 upper =
      static_cast<unsigned __int128>(r + sqrt_k) * (r + sqrt_k);
  if (static_cast<unsigned __int128>(n) > upper) return false;
  const unsigned __int128 base =
      static_cast<unsigned __int128>(r) * r - 1 +
      static_cast<unsigned __int128>(sqrt_k) * sqrt_k;
  if (static_cast<unsigned __int128>(n) <= base) return false;
  const unsigned __int128 x = static_cast<unsigned __int128>(n) - base;
  return x * x >
         4 * static_cast<unsigned __int128>(sqrt_k) * sqrt_k *
             (static_cast<unsigned __int128>(r) * r - 1);
}

bool in_bz_o(std::uint64_t n, std::uint64_t r, std::uint64_t sqrt_k) {
  const unsigned __int128 lower =
      static_cast<unsigned __int128>(r - sqrt_k) * (r - sqrt_k);
  if (static_cast<unsigned __int128>(n) < lower) return false;
  const unsigned __int128 base =
      static_cast<unsigned __int128>(r) * r + 1 +
      static_cast<unsigned __int128>(sqrt_k) * sqrt_k;
  if (static_cast<unsigned __int128>(n) >= base) return false;
  const unsigned __int128 x = base - static_cast<unsigned __int128>(n);
  return x * x >
         4 * static_cast<unsigned __int128>(sqrt_k) * sqrt_k *
             (static_cast<unsigned __int128>(r) * r + 1);
}

BzRecord compute_bz_record(std::uint64_t k_sq,
                           std::uint64_t r_inner,
                           std::uint64_t r_outer,
                           bool override_used) {
  BzRecord record;
  record.override_used = override_used;
  if (!is_square_u64(k_sq, record.sqrt_k)) {
    record.checked = false;
    record.clean = false;
    return record;
  }
  record.checked = true;
  const std::uint64_t inner_upper = (r_inner + record.sqrt_k) *
                                    (r_inner + record.sqrt_k);
  for (std::uint64_t n = inner_upper - 8; n <= inner_upper; ++n) {
    if (!in_bz_i(n, r_inner, record.sqrt_k)) continue;
    ++record.bz_i_candidate_count;
    if (campaign::is_gaussian_prime_norm(n)) ++record.bad_norm_count;
    if (n == std::numeric_limits<std::uint64_t>::max()) break;
  }
  const std::uint64_t outer_lower = (r_outer - record.sqrt_k) *
                                    (r_outer - record.sqrt_k);
  for (std::uint64_t n = outer_lower; n <= outer_lower + 8; ++n) {
    if (!in_bz_o(n, r_outer, record.sqrt_k)) continue;
    ++record.bz_o_candidate_count;
    if (campaign::is_gaussian_prime_norm(n)) ++record.bad_norm_count;
  }
  record.clean = record.bad_norm_count == 0;
  return record;
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

const char* face_name(campaign::Face face) noexcept {
  switch (face) {
    case campaign::Face::I:
      return "I";
    case campaign::Face::O:
      return "O";
    case campaign::Face::L:
      return "L";
    case campaign::Face::R:
      return "R";
  }
  return "?";
}

const char* telemetry_level_name(TelemetryLevel level) noexcept {
  switch (level) {
    case TelemetryLevel::kNone:
      return "none";
    case TelemetryLevel::kProfile:
      return "profile";
    case TelemetryLevel::kAudit:
      return "audit";
    case TelemetryLevel::kFull:
      return "full";
  }
  return "none";
}

bool parse_telemetry_level(const std::string& s, TelemetryLevel& out) {
  if (s == "none") {
    out = TelemetryLevel::kNone;
    return true;
  }
  if (s == "profile") {
    out = TelemetryLevel::kProfile;
    return true;
  }
  if (s == "audit") {
    out = TelemetryLevel::kAudit;
    return true;
  }
  if (s == "full") {
    out = TelemetryLevel::kFull;
    return true;
  }
  return false;
}

bool telemetry_writes_samples(TelemetryLevel level) noexcept {
  return level == TelemetryLevel::kAudit || level == TelemetryLevel::kFull;
}

const char* sample_class_name(SampleClass sample_class) noexcept {
  switch (sample_class) {
    case SampleClass::kGeoI:
      return "geo_I";
    case SampleClass::kGeoO:
      return "geo_O";
    case SampleClass::kAxis:
      return "axis";
    case SampleClass::kDiagonal:
      return "diagonal";
    case SampleClass::kHighPressure:
      return "high_pressure";
    case SampleClass::kDeterministicRandom:
      return "deterministic_random";
    case SampleClass::kCount:
      break;
  }
  return "deterministic_random";
}

nlohmann::json stitch_vertex_json(
    const campaign::SpanningStitchVertex& vertex) {
  return {
      {"tile_index", vertex.tile_index},
      {"group_label", vertex.group_label},
  };
}

nlohmann::json stitch_edge_json(const campaign::SpanningStitchEdge& edge) {
  return {
      {"event", edge.event},
      {"lhs_tile_index", edge.lhs.tile_index},
      {"lhs_group_label", edge.lhs.group_label},
      {"lhs_face", face_name(edge.lhs_face)},
      {"lhs_ordinal", static_cast<int>(edge.lhs_ordinal)},
      {"rhs_tile_index", edge.rhs.tile_index},
      {"rhs_group_label", edge.rhs.group_label},
      {"rhs_face", face_name(edge.rhs_face)},
      {"rhs_ordinal", static_cast<int>(edge.rhs_ordinal)},
  };
}

nlohmann::json stitch_edges_json(
    const std::vector<campaign::SpanningStitchEdge>& edges) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& edge : edges) out.push_back(stitch_edge_json(edge));
  return out;
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

std::uint64_t count_emitted_overflow_tileops(
    std::span<const campaign::TileOp> tileops) {
  std::uint64_t count = 0;
  for (const campaign::TileOp& op : tileops) {
    if ((op.tile_flags & campaign::OVERFLOW_BIT) != 0) ++count;
  }
  return count;
}

bool tile_has_any_flag(const std::uint8_t* flags) {
  for (int i = 0; i < 16; ++i) {
    if (flags[i] != 0) return true;
  }
  return false;
}

void mark_flag_labels(const std::uint8_t* flags,
                      std::array<bool, 129>& seen_labels) {
  for (int byte_idx = 0; byte_idx < 16; ++byte_idx) {
    const std::uint8_t value = flags[byte_idx];
    if (value == 0) continue;
    for (int bit = 0; bit < 8; ++bit) {
      if ((value & static_cast<std::uint8_t>(1U << bit)) == 0) continue;
      seen_labels[static_cast<std::size_t>(byte_idx * 8 + bit + 1)] = true;
    }
  }
}

std::uint64_t tile_total_port_count(const campaign::TileOp& op) {
  std::uint64_t total = 0;
  for (int face = 0; face < 4; ++face) total += op.n[face];
  return total;
}

std::uint64_t tile_max_face_port_count(const campaign::TileOp& op) {
  std::uint64_t max_ports = 0;
  for (int face = 0; face < 4; ++face) {
    max_ports = std::max(max_ports, static_cast<std::uint64_t>(op.n[face]));
  }
  return max_ports;
}

std::uint64_t tile_group_count(const campaign::TileOp& op) {
  std::array<bool, 129> seen_labels{};
  const std::uint64_t total_ports =
      std::min<std::uint64_t>(tile_total_port_count(op), 192);
  for (std::uint64_t idx = 0; idx < total_ports; ++idx) {
    const std::uint8_t label = op.face_groups[idx];
    if (label >= 1 && label <= 128) {
      seen_labels[static_cast<std::size_t>(label)] = true;
    }
  }
  mark_flag_labels(op.inner_flags, seen_labels);
  mark_flag_labels(op.outer_flags, seen_labels);

  std::uint64_t count = 0;
  for (std::size_t label = 1; label < seen_labels.size(); ++label) {
    if (seen_labels[label]) ++count;
  }
  return count;
}

std::uint64_t deterministic_string_hash(const std::string& s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

std::uint64_t sample_hash(const campaign::TileCoord& coord,
                          std::uint64_t r_inner,
                          std::uint64_t r_outer,
                          std::uint64_t seed,
                          SampleClass sample_class) {
  std::uint64_t h = 0x9e3779b97f4a7c15ULL;
  const auto mix = [&](std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  };
  mix(seed);
  mix(static_cast<std::uint64_t>(sample_class));
  mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(coord.i)));
  mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(coord.j)));
  mix(r_inner);
  mix(r_outer);
  return h;
}

std::uint64_t default_sample_seed(std::uint64_t k_sq,
                                  std::uint64_t r_inner,
                                  std::uint64_t r_outer,
                                  const std::string& region_spec) {
  std::uint64_t h = deterministic_string_hash(region_spec);
  const auto mix = [&](std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  };
  mix(k_sq);
  mix(r_inner);
  mix(r_outer);
  mix(static_cast<std::uint64_t>(campaign::S));
  mix(static_cast<std::uint64_t>(campaign::OFFSET_X));
  mix(static_cast<std::uint64_t>(campaign::OFFSET_Y));
  return h;
}

std::uint64_t tile_pressure_score(const campaign::TileOp& op) {
  std::uint64_t score = tile_total_port_count(op);
  for (std::uint8_t value : op.face_groups) {
    if (value != 0) ++score;
  }
  for (std::uint8_t value : op.inner_flags) {
    if (value != 0) ++score;
  }
  for (std::uint8_t value : op.outer_flags) {
    if (value != 0) ++score;
  }
  if ((op.tile_flags & campaign::OVERFLOW_BIT) != 0) {
    score += 1000000ULL;
  }
  return score;
}

std::uint64_t sample_coord_key(const campaign::TileCoord& coord) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(coord.i))
          << 32) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(coord.j));
}

std::array<std::size_t, static_cast<std::size_t>(SampleClass::kCount)>
sample_quotas(std::size_t target_count) {
  std::array<std::size_t, static_cast<std::size_t>(SampleClass::kCount)> quotas{};
  const std::size_t base = target_count / quotas.size();
  std::size_t remaining = target_count;
  for (SampleClass sample_class : kSampleClassOrder) {
    const std::size_t idx = static_cast<std::size_t>(sample_class);
    quotas[idx] = base;
    remaining -= base;
  }
  quotas[static_cast<std::size_t>(SampleClass::kDeterministicRandom)] +=
      remaining;
  return quotas;
}

class StratifiedTileSampler {
 public:
  StratifiedTileSampler(TelemetryLevel level,
                        std::uint64_t seed,
                        std::size_t target_count)
      : level_(level),
        seed_(seed),
        target_count_(target_count),
        quotas_(sample_quotas(target_count)) {
    const std::size_t reservoir_capacity = std::max<std::size_t>(
        target_count_, kDefaultAuditTileSampleTarget);
    for (std::size_t i = 0; i < reservoirs_.size(); ++i) {
      reservoirs_[i] = std::make_unique<SampleReservoir>(
          std::max<std::size_t>(reservoir_capacity, quotas_[i] * 4 + 64));
    }
  }

  void consider(const campaign::TileCoord& coord,
                const campaign::TileOp& op,
                bool has_i,
                bool has_o,
                std::uint64_t r_inner,
                std::uint64_t r_outer) {
    const std::uint64_t pressure = tile_pressure_score(op);
    std::array<bool, static_cast<std::size_t>(SampleClass::kCount)> member{};
    member[static_cast<std::size_t>(SampleClass::kGeoI)] = has_i;
    member[static_cast<std::size_t>(SampleClass::kGeoO)] = has_o;
    member[static_cast<std::size_t>(SampleClass::kAxis)] = coord.i == 0;
    member[static_cast<std::size_t>(SampleClass::kDiagonal)] =
        coord.j == coord.i;
    member[static_cast<std::size_t>(SampleClass::kHighPressure)] =
        pressure > 0;
    member[static_cast<std::size_t>(SampleClass::kDeterministicRandom)] = true;

    for (SampleClass sample_class : kSampleClassOrder) {
      const std::size_t idx = static_cast<std::size_t>(sample_class);
      if (!member[idx]) continue;
      ++population_counts_[idx];
      reservoirs_[idx]->consider(SampleCandidate{
          sample_class,
          sample_hash(coord, r_inner, r_outer, seed_, sample_class),
          pressure,
          coord,
          op,
      });
    }
  }

  std::vector<SampleCandidate> selected_samples() const {
    std::vector<SampleCandidate> selected;
    selected.reserve(target_count_);
    std::unordered_set<std::uint64_t> seen;
    std::array<std::vector<SampleCandidate>,
               static_cast<std::size_t>(SampleClass::kCount)>
        sorted;
    for (SampleClass sample_class : kSampleClassOrder) {
      const std::size_t idx = static_cast<std::size_t>(sample_class);
      sorted[idx] = reservoirs_[idx]->sorted();
    }

    const auto take_for_class = [&](SampleClass sample_class,
                                    std::size_t requested) {
      const std::size_t idx = static_cast<std::size_t>(sample_class);
      std::size_t taken = 0;
      for (const SampleCandidate& candidate : sorted[idx]) {
        if (taken >= requested || selected.size() >= target_count_) break;
        const std::uint64_t key = sample_coord_key(candidate.coord);
        if (!seen.insert(key).second) continue;
        selected.push_back(candidate);
        ++taken;
      }
    };

    for (SampleClass sample_class : kSampleClassOrder) {
      take_for_class(sample_class, quotas_[static_cast<std::size_t>(sample_class)]);
    }

    if (selected.size() < target_count_) {
      take_for_class(SampleClass::kDeterministicRandom,
                     target_count_ - selected.size());
    }
    return selected;
  }

  nlohmann::json quotas_json() const {
    nlohmann::json out = nlohmann::json::object();
    for (SampleClass sample_class : kSampleClassOrder) {
      out[sample_class_name(sample_class)] =
          quotas_[static_cast<std::size_t>(sample_class)];
    }
    return out;
  }

  nlohmann::json population_counts_json() const {
    nlohmann::json out = nlohmann::json::object();
    for (SampleClass sample_class : kSampleClassOrder) {
      out[sample_class_name(sample_class)] =
          population_counts_[static_cast<std::size_t>(sample_class)];
    }
    return out;
  }

  std::uint64_t seed() const noexcept { return seed_; }
  std::size_t target_count() const noexcept { return target_count_; }
  TelemetryLevel level() const noexcept { return level_; }

 private:
  TelemetryLevel level_ = TelemetryLevel::kAudit;
  std::uint64_t seed_ = 0;
  std::size_t target_count_ = 0;
  std::array<std::size_t, static_cast<std::size_t>(SampleClass::kCount)> quotas_{};
  std::array<std::uint64_t, static_cast<std::size_t>(SampleClass::kCount)>
      population_counts_{};
  std::array<std::unique_ptr<SampleReservoir>,
             static_cast<std::size_t>(SampleClass::kCount)>
      reservoirs_{};
};

nlohmann::json tileop_json(const campaign::TileOp& op) {
  nlohmann::json n = nlohmann::json::array();
  for (int i = 0; i < 4; ++i) n.push_back(op.n[i]);
  nlohmann::json face_groups = nlohmann::json::array();
  for (std::uint8_t value : op.face_groups) face_groups.push_back(value);
  nlohmann::json inner_flags = nlohmann::json::array();
  for (std::uint8_t value : op.inner_flags) inner_flags.push_back(value);
  nlohmann::json outer_flags = nlohmann::json::array();
  for (std::uint8_t value : op.outer_flags) outer_flags.push_back(value);
  return {
      {"n", n},
      {"face_groups", face_groups},
      {"inner_flags", inner_flags},
      {"outer_flags", outer_flags},
      {"tile_flags", op.tile_flags},
  };
}

void account_tile_stats_and_samples(
    const std::vector<campaign::TileCoord>& coords,
    std::span<const campaign::TileOp> tileops,
    std::uint64_t flat_index_start,
    RunStats& stats,
    StratifiedTileSampler* sampler,
    std::uint64_t r_inner,
    std::uint64_t r_outer) {
  for (std::size_t idx = 0; idx < tileops.size(); ++idx) {
    const campaign::TileOp& op = tileops[idx];
    const campaign::TileCoord& coord = coords[idx];
    const bool has_i = tile_has_any_flag(op.inner_flags);
    const bool has_o = tile_has_any_flag(op.outer_flags);
    HighPressureTile pressure_tile{};
    pressure_tile.flat_index = flat_index_start + static_cast<std::uint64_t>(idx);
    pressure_tile.coord = coord;
    pressure_tile.pressure_score = tile_pressure_score(op);
    pressure_tile.group_count = tile_group_count(op);
    pressure_tile.total_port_count = tile_total_port_count(op);
    pressure_tile.max_face_port_count = tile_max_face_port_count(op);
    for (int face = 0; face < 4; ++face) {
      pressure_tile.face_port_counts[static_cast<std::size_t>(face)] =
          op.n[face];
    }
    pressure_tile.geo_i = has_i;
    pressure_tile.geo_o = has_o;
    pressure_tile.overflow =
        (op.tile_flags & campaign::OVERFLOW_BIT) != 0;
    stats.analytical.consider(pressure_tile);
    if (has_i) ++stats.geo_i_tiles;
    if (has_o) ++stats.geo_o_tiles;
    if (sampler != nullptr) {
      sampler->consider(coord, op, has_i, has_o, r_inner, r_outer);
    }
  }
}

struct CertPoint {
  std::int64_t a = 0;
  std::int64_t b = 0;

  friend bool operator==(const CertPoint&, const CertPoint&) = default;
};

struct CertGraphNode {
  CertPoint point;
  bool inner = false;
  bool outer = false;
};

struct CertTileData {
  campaign::TileCoord coord;
  std::vector<campaign::Prime> primes;
  std::unordered_map<std::uint64_t, int> index_by_coord;
  campaign::DSU dsu{0};
  campaign::internal::DenseRemap remap;
  std::vector<campaign::internal::PrimeGeoFlags> flags;
  std::map<int, std::vector<int>> members_by_label;
};

bool stitch_vertex_equal(const campaign::SpanningStitchVertex& lhs,
                         const campaign::SpanningStitchVertex& rhs) {
  return lhs.tile_index == rhs.tile_index &&
         lhs.group_label == rhs.group_label;
}

campaign::SpanningStitchVertex other_vertex(
    const campaign::SpanningStitchEdge& edge,
    const campaign::SpanningStitchVertex& current) {
  if (stitch_vertex_equal(edge.lhs, current)) return edge.rhs;
  if (stitch_vertex_equal(edge.rhs, current)) return edge.lhs;
  throw std::runtime_error("stitch edge is not incident to current vertex");
}

campaign::TileCoord coord_from_flat_index(const campaign::Grid& grid,
                                          std::int64_t tile_index) {
  for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
    const std::size_t k = static_cast<std::size_t>(i - grid.i_min);
    if (k + 1 >= grid.tower_offset.size()) break;
    const std::int64_t begin = grid.tower_offset[k];
    const std::int64_t end = grid.tower_offset[k + 1];
    if (tile_index < begin || tile_index >= end) continue;
    const std::int32_t j =
        static_cast<std::int32_t>(grid.j_low[k] + (tile_index - begin));
    return campaign::TileCoord{
        i,
        j,
        static_cast<std::int64_t>(grid.o_x) +
            static_cast<std::int64_t>(grid.S_value) * i,
        static_cast<std::int64_t>(grid.o_y) +
            static_cast<std::int64_t>(grid.S_value) * j,
    };
  }
  throw std::runtime_error("spanning cert tile index outside grid");
}

bool within_k_sq(const campaign::Prime& lhs,
                 const campaign::Prime& rhs) {
  const __int128 da = static_cast<__int128>(lhs.a) -
                      static_cast<__int128>(rhs.a);
  const __int128 db = static_cast<__int128>(lhs.b) -
                      static_cast<__int128>(rhs.b);
  return da * da + db * db <= static_cast<__int128>(campaign::k_sq_value);
}

std::uint64_t cert_coord_key(std::int64_t a, std::int64_t b) {
  if (a < 0 || b < 0 ||
      a > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()) ||
      b > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("certificate coordinate outside packed key range");
  }
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) |
         static_cast<std::uint32_t>(b);
}

CertTileData build_cert_tile_data(const campaign::Grid& grid,
                                  const campaign::CampaignConstants& constants,
                                  std::int64_t tile_index) {
  CertTileData data;
  data.coord = coord_from_flat_index(grid, tile_index);
  data.primes = campaign::sieve_tile(data.coord, constants);
  data.index_by_coord.reserve(data.primes.size());
  for (int i = 0; i < static_cast<int>(data.primes.size()); ++i) {
    const campaign::Prime& prime = data.primes[static_cast<std::size_t>(i)];
    data.index_by_coord.emplace(cert_coord_key(prime.a, prime.b), i);
  }
  data.flags.reserve(data.primes.size());
  for (const campaign::Prime& prime : data.primes) {
    const auto norm_sq = static_cast<std::int64_t>(prime.norm_sq);
    data.flags.push_back(campaign::internal::PrimeGeoFlags{
        campaign::is_inner_prime(norm_sq, constants),
        campaign::is_outer_prime(norm_sq, constants),
    });
  }
  data.dsu = campaign::internal::build_local_dsu(data.primes);
  data.remap = campaign::internal::dense_remap_visible_roots(
      &data.dsu, data.primes, data.flags, data.coord);
  if (data.remap.overflow) {
    throw std::runtime_error("cannot materialize certificate through overflow tile");
  }
  for (int i = 0; i < static_cast<int>(data.primes.size()); ++i) {
    const int raw_root = data.dsu.find(i);
    const int label =
        data.remap.wire_label_by_raw_root[static_cast<std::size_t>(raw_root)];
    if (label > 0) data.members_by_label[label].push_back(i);
  }
  return data;
}

CertTileData& cert_tile(
    std::unordered_map<std::int64_t, CertTileData>& cache,
    const campaign::Grid& grid,
    const campaign::CampaignConstants& constants,
    std::int64_t tile_index) {
  const auto it = cache.find(tile_index);
  if (it != cache.end()) return it->second;
  auto inserted = cache.emplace(
      tile_index, build_cert_tile_data(grid, constants, tile_index));
  return inserted.first->second;
}

std::vector<int> local_prime_path(CertTileData& tile, int start, int goal) {
  if (start == goal) return {start};
  const int n = static_cast<int>(tile.primes.size());
  std::vector<int> parent(static_cast<std::size_t>(n), -1);
  std::queue<int> pending;
  parent[static_cast<std::size_t>(start)] = start;
  pending.push(start);
  while (!pending.empty()) {
    const int current = pending.front();
    pending.pop();
    const int root = tile.dsu.find(current);
    const campaign::Prime& current_prime =
        tile.primes[static_cast<std::size_t>(current)];
    for (int da = -campaign::C; da <= campaign::C; ++da) {
      for (int db = -campaign::C; db <= campaign::C; ++db) {
        if (da == 0 && db == 0) continue;
        const int d2 = da * da + db * db;
        if (d2 > campaign::k_sq_value) continue;
        const std::int64_t next_a = current_prime.a + da;
        const std::int64_t next_b = current_prime.b + db;
        if (next_a < 0 || next_b < 0) continue;
        const auto it =
            tile.index_by_coord.find(cert_coord_key(next_a, next_b));
        if (it == tile.index_by_coord.end()) continue;
        const int next = it->second;
        if (parent[static_cast<std::size_t>(next)] >= 0) continue;
        if (tile.dsu.find(next) != root) continue;
        parent[static_cast<std::size_t>(next)] = current;
        if (next == goal) {
          std::vector<int> path;
          for (int x = goal; x != start;
               x = parent[static_cast<std::size_t>(x)]) {
            path.push_back(x);
          }
          path.push_back(start);
          std::reverse(path.begin(), path.end());
          return path;
        }
        pending.push(next);
      }
    }
  }
  throw std::runtime_error("could not materialize local UF coordinate path");
}

std::pair<int, int> bridge_prime_pair(CertTileData& lhs,
                                      int lhs_label,
                                      CertTileData& rhs,
                                      int rhs_label) {
  const auto lhs_it = lhs.members_by_label.find(lhs_label);
  const auto rhs_it = rhs.members_by_label.find(rhs_label);
  if (lhs_it == lhs.members_by_label.end() ||
      rhs_it == rhs.members_by_label.end()) {
    throw std::runtime_error("stitch group label missing from tile data");
  }
  for (int li : lhs_it->second) {
    for (int ri : rhs_it->second) {
      if (within_k_sq(lhs.primes[static_cast<std::size_t>(li)],
                     rhs.primes[static_cast<std::size_t>(ri)])) {
        return {li, ri};
      }
    }
  }
  throw std::runtime_error("could not find concrete bridge prime pair");
}

void append_prime_path(std::vector<CertPoint>& out,
                       const CertTileData& tile,
                       const std::vector<int>& indices) {
  for (int idx : indices) {
    const campaign::Prime& p = tile.primes[static_cast<std::size_t>(idx)];
    CertPoint point{p.a, p.b};
    if (!out.empty() && out.back() == point) continue;
    out.push_back(point);
  }
}

std::vector<CertPoint> materialize_tube_coordinate_path(
    const campaign::Grid& grid,
    const campaign::CampaignConstants& constants,
    const std::vector<campaign::SpanningStitchVertex>& vertices) {
  std::vector<std::int64_t> tile_indices;
  tile_indices.reserve(vertices.size());
  std::unordered_map<std::int64_t, bool> seen_tiles;
  for (const auto& vertex : vertices) {
    if (seen_tiles.emplace(vertex.tile_index, true).second) {
      tile_indices.push_back(vertex.tile_index);
    }
  }

  std::vector<CertGraphNode> nodes;
  std::unordered_map<std::uint64_t, int> index_by_coord;
  for (std::int64_t tile_index : tile_indices) {
    const campaign::TileCoord coord = coord_from_flat_index(grid, tile_index);
    for (const campaign::Prime& prime : campaign::sieve_tile(coord, constants)) {
      const std::uint64_t key = cert_coord_key(prime.a, prime.b);
      if (index_by_coord.find(key) != index_by_coord.end()) continue;
      const int index = static_cast<int>(nodes.size());
      index_by_coord.emplace(key, index);
      const auto norm_sq = static_cast<std::int64_t>(prime.norm_sq);
      nodes.push_back(CertGraphNode{
          CertPoint{prime.a, prime.b},
          campaign::is_inner_prime(norm_sq, constants),
          campaign::is_outer_prime(norm_sq, constants),
      });
    }
  }

  if (nodes.empty()) {
    throw std::runtime_error("span certificate tile tube has no primes");
  }

  std::vector<int> parent(nodes.size(), -1);
  std::queue<int> pending;
  for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
    if (!nodes[static_cast<std::size_t>(i)].inner) continue;
    parent[static_cast<std::size_t>(i)] = i;
    pending.push(i);
  }
  if (pending.empty()) {
    throw std::runtime_error("span certificate tile tube has no inner prime");
  }

  int target = -1;
  while (!pending.empty() && target < 0) {
    const int current = pending.front();
    pending.pop();
    const CertPoint point = nodes[static_cast<std::size_t>(current)].point;
    if (nodes[static_cast<std::size_t>(current)].outer) {
      target = current;
      break;
    }
    for (int da = -campaign::C; da <= campaign::C; ++da) {
      for (int db = -campaign::C; db <= campaign::C; ++db) {
        if (da == 0 && db == 0) continue;
        const int d2 = da * da + db * db;
        if (d2 > campaign::k_sq_value) continue;
        const std::int64_t next_a = point.a + da;
        const std::int64_t next_b = point.b + db;
        if (next_a < 0 || next_b < 0) continue;
        const auto it =
            index_by_coord.find(cert_coord_key(next_a, next_b));
        if (it == index_by_coord.end()) continue;
        const int next = it->second;
        if (parent[static_cast<std::size_t>(next)] >= 0) continue;
        parent[static_cast<std::size_t>(next)] = current;
        if (nodes[static_cast<std::size_t>(next)].outer) {
          target = next;
          pending = std::queue<int>{};
          break;
        }
        pending.push(next);
      }
    }
  }

  if (target < 0) {
    throw std::runtime_error(
        "could not materialize coordinate path through stitch tile tube");
  }

  std::vector<CertPoint> out;
  for (int x = target;; x = parent[static_cast<std::size_t>(x)]) {
    out.push_back(nodes[static_cast<std::size_t>(x)].point);
    if (parent[static_cast<std::size_t>(x)] == x) break;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<campaign::SpanningStitchVertex> ordered_stitch_vertices(
    const campaign::SpanningStitchPath& path) {
  if (!path.reconstructed || !path.final_bridge_present) {
    throw std::runtime_error("stitch path is not reconstructed");
  }
  std::vector<campaign::SpanningStitchVertex> vertices;
  campaign::SpanningStitchVertex current = path.inner_source;
  vertices.push_back(current);
  for (const auto& edge : path.inner_path_edges) {
    current = other_vertex(edge, current);
    vertices.push_back(current);
  }
  if (!stitch_vertex_equal(current, path.inner_endpoint)) {
    throw std::runtime_error("inner stitch path endpoint mismatch");
  }
  current = other_vertex(path.final_bridge, current);
  vertices.push_back(current);
  if (!stitch_vertex_equal(current, path.outer_endpoint)) {
    throw std::runtime_error("final bridge endpoint mismatch");
  }
  for (auto it = path.outer_path_edges.rbegin();
       it != path.outer_path_edges.rend(); ++it) {
    current = other_vertex(*it, current);
    vertices.push_back(current);
  }
  if (!stitch_vertex_equal(current, path.outer_source)) {
    throw std::runtime_error("outer stitch path source mismatch");
  }
  return vertices;
}

std::vector<campaign::SpanningStitchEdge> ordered_stitch_edges(
    const campaign::SpanningStitchPath& path) {
  std::vector<campaign::SpanningStitchEdge> edges;
  edges.insert(edges.end(), path.inner_path_edges.begin(),
               path.inner_path_edges.end());
  edges.push_back(path.final_bridge);
  for (auto it = path.outer_path_edges.rbegin();
       it != path.outer_path_edges.rend(); ++it) {
    edges.push_back(*it);
  }
  return edges;
}

int endpoint_prime(CertTileData& tile, int label, bool inner) {
  const auto it = tile.members_by_label.find(label);
  if (it == tile.members_by_label.end()) {
    throw std::runtime_error("endpoint group label missing");
  }
  for (int idx : it->second) {
    const auto& flags = tile.flags[static_cast<std::size_t>(idx)];
    if ((inner && flags.inner) || (!inner && flags.outer)) return idx;
  }
  throw std::runtime_error(inner ? "inner endpoint prime missing"
                                 : "outer endpoint prime missing");
}

void write_span_certificate(const std::filesystem::path& path,
                            const campaign::Grid& grid,
                            const campaign::CampaignConstants& constants,
                            std::uint64_t k_sq,
                            std::uint64_t r_inner,
                            std::uint64_t r_outer,
                            const std::string& region_spec,
                            const campaign::SpanningTrace& trace) {
  const auto vertices = ordered_stitch_vertices(trace.path);
  const auto cert_path =
      materialize_tube_coordinate_path(grid, constants, vertices);

  nlohmann::json points = nlohmann::json::array();
  for (const CertPoint& p : cert_path) {
    points.push_back({{"a", p.a}, {"b", p.b}});
  }
  nlohmann::json cert = {
      {"schema_version", 1},
      {"k_sq", k_sq},
      {"r_inner", r_inner},
      {"r_outer", r_outer},
      {"region", region_spec},
      {"source", "campaign_main_cuda stitch-path materializer"},
      {"path", points},
  };
  std::ofstream out(path);
  if (!out.is_open()) {
    throw std::runtime_error("could not open span certificate output " +
                             path.string());
  }
  out << cert.dump(2) << "\n";
}

void write_sample_artifacts(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& samples_path,
    const StratifiedTileSampler& sampler,
    std::uint64_t k_sq,
    std::uint64_t r_inner,
    std::uint64_t r_outer,
    const std::string& region_spec,
    RunStats& stats) {
  const std::uint64_t width = r_outer - r_inner;
  const std::string claim_id = "k" + std::to_string(k_sq) + "-r" +
                               std::to_string(r_inner) + "-w" +
                               std::to_string(width);
  const std::vector<SampleCandidate> samples = sampler.selected_samples();

  nlohmann::json class_counts = nlohmann::json::object();
  for (SampleClass sample_class : kSampleClassOrder) {
    class_counts[sample_class_name(sample_class)] = 0;
  }

  std::ofstream sample_out(samples_path);
  if (!sample_out.is_open()) {
    throw std::runtime_error("could not open tile sample output " +
                             samples_path.string());
  }
  for (const SampleCandidate& sample : samples) {
    class_counts[sample_class_name(sample.sample_class)] =
        class_counts[sample_class_name(sample.sample_class)].get<std::size_t>() +
        1;
    nlohmann::json rec = {
        {"schema_version", 2},
        {"claim_id", claim_id},
        {"k_sq", k_sq},
        {"r_inner", r_inner},
        {"r_outer", r_outer},
        {"width", width},
        {"region", region_spec},
        {"telemetry_level", telemetry_level_name(sampler.level())},
        {"sample_plan",
         {{"seed", sampler.seed()},
          {"target_count", sampler.target_count()},
          {"quotas", sampler.quotas_json()}}},
        {"sample_class", sample_class_name(sample.sample_class)},
        {"tile",
         {{"i", sample.coord.i},
          {"j", sample.coord.j},
          {"a_lo", sample.coord.a_lo},
          {"b_lo", sample.coord.b_lo}}},
        {"pressure", {{"score", sample.pressure_score}}},
        {"tileop", tileop_json(sample.tileop)},
    };
    sample_out << rec.dump() << "\n";
  }
  if (!sample_out.good()) {
    throw std::runtime_error("failed while writing tile sample output " +
                             samples_path.string());
  }

  stats.tile_samples_written = static_cast<std::uint64_t>(samples.size());

  nlohmann::json quota_status = nlohmann::json::object();
  nlohmann::json selection = nlohmann::json::array();
  nlohmann::json population_exhaustion = nlohmann::json::object();
  const nlohmann::json quotas = sampler.quotas_json();
  const nlohmann::json populations = sampler.population_counts_json();
  const std::size_t total_unique_population =
      populations.at(sample_class_name(SampleClass::kDeterministicRandom))
          .get<std::size_t>();
  const bool global_population_exhausted =
      samples.size() >= total_unique_population;
  for (SampleClass sample_class : kSampleClassOrder) {
    const char* name = sample_class_name(sample_class);
    const std::size_t quota = quotas.at(name).get<std::size_t>();
    const std::size_t population = populations.at(name).get<std::size_t>();
    const std::size_t emitted = class_counts.at(name).get<std::size_t>();
    const bool exhausted =
        emitted < quota && (population <= emitted || global_population_exhausted);
    quota_status[name] = {
        {"quota", quota},
        {"population", population},
        {"emitted", emitted},
        {"population_exhausted", exhausted},
    };
    population_exhaustion[name] = {
        {"population", population},
        {"population_exhausted", exhausted},
    };
    selection.push_back({
        {"class", name},
        {"description", std::string("deterministic stratified ") + name +
                            " tile sample"},
        {"target_count", quota},
        {"population_exhausted", exhausted},
    });
  }

  nlohmann::json manifest = {
      {"schema_version", 2},
      {"claim_id", claim_id},
      {"k_sq", k_sq},
      {"r_inner", r_inner},
      {"r_outer", r_outer},
      {"width", width},
      {"region", region_spec},
      {"seed", sampler.seed()},
      {"target_tile_samples", sampler.target_count()},
      {"sample_count", samples.size()},
      {"class_counts", class_counts},
      {"population_exhaustion", population_exhaustion},
      {"selection", selection},
      {"telemetry_level", telemetry_level_name(sampler.level())},
      {"sample_plan",
       {{"seed", sampler.seed()},
        {"target_count", sampler.target_count()},
        {"classes",
         {"geo_I", "geo_O", "axis", "diagonal", "high_pressure",
          "deterministic_random"}},
        {"quotas", quotas},
        {"population_counts", populations},
        {"sample_class_counts", class_counts},
        {"quota_status", quota_status},
        {"emitted_count", samples.size()}}},
      {"artifacts", {{"tile_sample_path", samples_path.string()}}},
  };

  std::ofstream manifest_out(manifest_path);
  if (!manifest_out.is_open()) {
    throw std::runtime_error("could not open sample manifest output " +
                             manifest_path.string());
  }
  manifest_out << manifest.dump(2) << "\n";
  if (!manifest_out.good()) {
    throw std::runtime_error("failed while writing sample manifest output " +
                             manifest_path.string());
  }
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
                        const campaign::Grid& grid,
                        const campaign::CampaignConstants& constants,
                        std::size_t chunk_size,
                        std::uint64_t active_tiles,
                        const RunStats& stats,
                        const Timings& timings,
                        campaign::Verdict verdict,
                        bool snapshot_enabled,
                        bool early_exit_enabled,
                        const BzRecord& bz_record,
                        const std::optional<std::string>& span_cert_path,
                        const std::optional<std::string>& sample_manifest_path,
                        const std::optional<std::string>& tile_sample_path,
                        TelemetryLevel telemetry_level,
                        const std::optional<std::string>& stats_level,
                        std::uint64_t sample_seed,
                        std::size_t sample_target_count,
                        bool trace_spanning,
                        const campaign::SpanningTrace& spanning_trace,
                        const campaign::ComponentCensus& component_census) {
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
      {"telemetry_level", telemetry_level_name(telemetry_level)},
      {"command", command},
      {"build",
       {{"commit", GAUSSIAN_MOAT_GIT_COMMIT},
        {"identity", std::string("campaign_main_cuda:") +
                         GAUSSIAN_MOAT_GIT_COMMIT + ":sm_" +
                         GAUSSIAN_MOAT_CUDA_ARCH},
        {"cuda_arch", std::string("sm_") + GAUSSIAN_MOAT_CUDA_ARCH}}},
      {"radii",
       {{"k_sq", k_sq},
        {"r_inner", r_inner},
        {"r_outer", r_outer},
        {"width", r_outer - r_inner}}},
      {"region", region_spec},
      {"hashes",
       {{"grid", campaign::grid_params_hash(grid)},
        {"grid_hash", campaign::grid_params_hash(grid)},
        {"constants", constants.canonical_hash()},
        {"constants_hash", constants.canonical_hash()},
        {"mr_witness", campaign::CampaignConstants::mr_witness_set_sha256()},
        {"mr_witness_hash",
         campaign::CampaignConstants::mr_witness_set_sha256()}}},
      {"snapshot_enabled", snapshot_enabled},
      {"early_exit_enabled", early_exit_enabled},
      {"early_exit_taken", stats.early_exit_taken},
      {"verdict", verdict_name(verdict)},
      {"bz",
       {{"checked", bz_record.checked},
        {"clean", bz_record.clean},
        {"override_used", bz_record.override_used},
        {"sqrt_k", bz_record.sqrt_k},
        {"bz_i_candidate_count", bz_record.bz_i_candidate_count},
        {"bz_o_candidate_count", bz_record.bz_o_candidate_count},
        {"bad_norm_count", bz_record.bad_norm_count}}},
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
      {"host_tileop_counters",
       {{"emitted_overflow_bit_count", stats.emitted_overflow_tileops}}},
      {"device",
       {{"name", stats.dispatch.device_name},
        {"stream_count", stats.dispatch.stream_count},
        {"host_chunk_tiles", stats.dispatch.host_chunk_tiles},
        {"device_slab_tiles", stats.dispatch.device_slab_tiles},
        {"phase1_peak_bytes", stats.dispatch.phase1_peak_bytes},
        {"phase2_peak_bytes", stats.dispatch.phase2_peak_bytes},
        {"pinned_host_bytes", stats.dispatch.pinned_host_bytes}}},
      {"overflow_diagnostics", overflow_diags},
      {"artifacts",
       {{"span_cert_path", span_cert_path.value_or("")},
        {"sample_manifest_path", sample_manifest_path.value_or("")},
        {"tile_sample_path", tile_sample_path.value_or("")}}},
  };

  if (span_cert_path.has_value() || sample_manifest_path.has_value() ||
      tile_sample_path.has_value() || stats_level.has_value() ||
      telemetry_level != TelemetryLevel::kNone) {
    profile["stats_v2"] = {
        {"stats_level", stats_level.value_or("")},
        {"telemetry_level", telemetry_level_name(telemetry_level)},
        {"geo_i_tiles", stats.geo_i_tiles},
        {"geo_o_tiles", stats.geo_o_tiles},
        {"geo_I",
         {{"tile_population", stats.geo_i_tiles},
          {"port_population", stats.analytical.geo_i_port_population}}},
        {"geo_O",
         {{"tile_population", stats.geo_o_tiles},
          {"port_population", stats.analytical.geo_o_port_population}}},
        {"distributions",
         {{"candidate_counts", nullptr},
          {"gaussian_prime_counts", nullptr},
          {"group_counts", stats.analytical.group_counts.json()},
          {"total_port_counts", stats.analytical.total_port_counts.json()},
          {"max_face_port_counts",
           stats.analytical.max_face_port_counts.json()}}},
        {"distribution_availability",
         {{"candidate_counts",
           "unavailable from emitted TileOp stream; only first overflow "
           "diagnostics carry candidate counts"},
          {"gaussian_prime_counts",
           "unavailable from emitted TileOp stream; only first overflow "
           "diagnostics carry prime counts"},
          {"group_counts", "computed analytically from TileOp group labels"},
          {"total_port_counts", "computed analytically from TileOp n[4]"},
          {"max_face_port_counts", "computed analytically from TileOp n[4]"}}},
        {"high_pressure_top_n", kStatsV2HighPressureLimit},
        {"high_pressure", stats.analytical.high_pressure_json()},
        {"component_census", component_census_json(component_census)},
        {"tile_samples_written", stats.tile_samples_written},
        {"sample_seed", sample_seed},
        {"sample_target_count", sample_target_count},
        {"span_cert_path", span_cert_path.value_or("")},
        {"sample_manifest_path", sample_manifest_path.value_or("")},
        {"tile_sample_path", tile_sample_path.value_or("")},
    };
  }

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
        {"inner_source_tile_index", spanning_trace.inner_source_tile_index},
        {"inner_source_group_label", spanning_trace.inner_source_group_label},
        {"outer_source_tile_index", spanning_trace.outer_source_tile_index},
        {"outer_source_group_label", spanning_trace.outer_source_group_label},
    };
    if (spanning_trace.path.enabled) {
      profile["spanning_trace"]["stitch_path"] = {
          {"enabled", spanning_trace.path.enabled},
          {"reconstructed", spanning_trace.path.reconstructed},
          {"failure_reason", spanning_trace.path.failure_reason},
          {"recorded_edges", spanning_trace.path.recorded_edges},
          {"inner_source", stitch_vertex_json(spanning_trace.path.inner_source)},
          {"outer_source", stitch_vertex_json(spanning_trace.path.outer_source)},
          {"inner_endpoint",
           stitch_vertex_json(spanning_trace.path.inner_endpoint)},
          {"outer_endpoint",
           stitch_vertex_json(spanning_trace.path.outer_endpoint)},
          {"final_bridge_present", spanning_trace.path.final_bridge_present},
          {"inner_path_edge_count",
           spanning_trace.path.inner_path_edges.size()},
          {"outer_path_edge_count",
           spanning_trace.path.outer_path_edges.size()},
          {"inner_path_edges",
           stitch_edges_json(spanning_trace.path.inner_path_edges)},
          {"outer_path_edges",
           stitch_edges_json(spanning_trace.path.outer_path_edges)},
      };
      if (spanning_trace.path.final_bridge_present) {
        profile["spanning_trace"]["stitch_path"]["final_bridge"] =
            stitch_edge_json(spanning_trace.path.final_bridge);
      }
    }
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
                 std::uint64_t k_sq,
                 std::uint64_t r_inner,
                 std::uint64_t r_outer,
                 StratifiedTileSampler* sampler,
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
  stats.emitted_overflow_tileops += count_emitted_overflow_tileops(tileops);
  accumulate_dispatch_stats(stats.dispatch, batch_stats);
  (void)k_sq;
  account_tile_stats_and_samples(
      batch.tiles, tileops,
      stats.produced_tiles - static_cast<std::uint64_t>(batch.tiles.size()),
      stats, sampler, r_inner, r_outer);

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
  result.emitted_overflow_tileops =
      count_emitted_overflow_tileops(result.tileops);
  result.coords = std::move(batch.tiles);

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
                            std::uint64_t r_inner,
                            std::uint64_t r_outer,
                            StratifiedTileSampler* sampler,
                            bool early_exit_enabled) {
  timings.cuda += result.cuda;
  stats.produced_tiles += result.produced_tiles;
  stats.app_batches += 1;
  stats.emitted_overflow_tileops += result.emitted_overflow_tileops;
  accumulate_dispatch_stats(stats.dispatch, result.dispatch);
  account_tile_stats_and_samples(result.coords, result.tileops,
                                 stats.produced_tiles - result.produced_tiles,
                                 stats, sampler, r_inner, r_outer);

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
    std::uint64_t r_inner,
    std::uint64_t r_outer,
    StratifiedTileSampler* sampler,
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
                             r_inner, r_outer, sampler, early_exit_enabled)) {
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
    std::uint64_t r_inner,
    std::uint64_t r_outer,
    StratifiedTileSampler* sampler,
    bool early_exit_enabled) {
  if (!in_flight.has_value()) {
    return false;
  }

  BatchDispatchResult ready = take_in_flight_result(in_flight);
  return ingest_dispatch_result(std::move(ready), compositor, timings, stats,
                                r_inner, r_outer, sampler, early_exit_enabled);
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
  std::optional<std::string> stats_level;
  std::optional<TelemetryLevel> telemetry_level_arg;
  std::optional<std::string> span_cert_path;
  std::optional<std::string> sample_manifest_path;
  std::optional<std::string> tile_sample_path;
  std::optional<std::uint64_t> sample_seed_arg;
  std::optional<std::size_t> tile_sample_count_arg;
  std::size_t chunk_size = kDefaultChunkSize;
  std::size_t tile_sample_count = kDefaultAuditTileSampleTarget;
  bool legacy_stats_level_profile = false;
  bool no_early_exit = false;
  bool overlap_compositor = false;
  bool overflow_diagnostics = false;
  bool allow_uncertified_boundary_band = false;
  bool trace_spanning = false;
  bool trace_spanning_path = false;
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
    } else if (a == "--trace-spanning-path") {
      trace_spanning = true;
      trace_spanning_path = true;
    } else if (a == "--timing") {
      timing = true;
    } else if (take_val("--profile", val)) {
      profile_path = val;
      timing = true;
    } else if (take_val("--telemetry", val)) {
      TelemetryLevel parsed;
      if (!parse_telemetry_level(val, parsed)) {
        std::cerr << "Error: unsupported --telemetry value: " << val << "\n";
        return 2;
      }
      telemetry_level_arg = parsed;
    } else if (take_val("--stats-level", val)) {
      if (val != "profile") {
        std::cerr << "Error: unsupported --stats-level value: " << val << "\n";
        return 2;
      }
      stats_level = val;
      legacy_stats_level_profile = true;
    } else if (take_val("--emit-span-cert", val)) {
      span_cert_path = val;
      trace_spanning = true;
      trace_spanning_path = true;
    } else if (take_val("--sample-manifest", val)) {
      sample_manifest_path = val;
    } else if (take_val("--tile-sample-out", val)) {
      tile_sample_path = val;
    } else if (take_val("--tile-sample-count", val)) {
      std::uint64_t v;
      if (!parse_uint64(val, v) || v == 0 ||
          v > std::numeric_limits<std::size_t>::max()) {
        std::cerr << "Error: invalid --tile-sample-count value: " << val
                  << "\n";
        return 2;
      }
      tile_sample_count_arg = static_cast<std::size_t>(v);
      tile_sample_count = static_cast<std::size_t>(v);
    } else if (take_val("--sample-seed", val)) {
      std::uint64_t v;
      if (!parse_uint64(val, v)) {
        std::cerr << "Error: invalid --sample-seed value: " << val << "\n";
        return 2;
      }
      sample_seed_arg = v;
    } else if (a == "--allow-uncertified-boundary-band") {
      allow_uncertified_boundary_band = true;
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

  TelemetryLevel telemetry_level = telemetry_level_arg.value_or(
      profile_path.has_value() ? TelemetryLevel::kProfile
                               : TelemetryLevel::kNone);
  if (legacy_stats_level_profile) {
    if (telemetry_level_arg.has_value() &&
        telemetry_level_arg.value() != TelemetryLevel::kProfile) {
      std::cerr << "Error: --stats-level profile is a legacy alias for "
                << "--telemetry=profile and cannot be combined with a "
                << "different --telemetry value\n";
      return 2;
    }
    telemetry_level = TelemetryLevel::kProfile;
  }
  if (tile_sample_path.has_value() || sample_manifest_path.has_value()) {
    if (telemetry_level == TelemetryLevel::kNone ||
        telemetry_level == TelemetryLevel::kProfile) {
      telemetry_level = TelemetryLevel::kAudit;
    }
    if (legacy_stats_level_profile && !tile_sample_count_arg.has_value()) {
      tile_sample_count = kLegacyStatsTileSampleTarget;
    }
  }
  const bool sample_artifacts_enabled = telemetry_writes_samples(telemetry_level);

  if (sample_artifacts_enabled && !tile_sample_path.has_value()) {
    std::cerr << "Error: --telemetry=" << telemetry_level_name(telemetry_level)
              << " requires --tile-sample-out\n";
    return 2;
  }
  if (sample_artifacts_enabled && !sample_manifest_path.has_value()) {
    std::cerr << "Error: --telemetry=" << telemetry_level_name(telemetry_level)
              << " requires --sample-manifest\n";
    return 2;
  }
  if (!sample_artifacts_enabled &&
      (tile_sample_count_arg.has_value() || sample_seed_arg.has_value())) {
    std::cerr << "Error: sample count/seed require --telemetry=audit or full\n";
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

  if (overlap_compositor && sample_artifacts_enabled) {
    std::cerr << "Error: --overlap-compositor is not supported with "
              << "--telemetry=audit/full sample emission\n";
    return 2;
  }

  if (!no_early_exit && !snapshot_path.has_value() &&
      sample_artifacts_enabled) {
    std::cerr << "Error: --telemetry=audit/full require --no-early-exit so "
              << "sample artifacts cover the full annulus\n";
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

  const BzRecord bz_record = compute_bz_record(
      *k_sq, *r_inner, *r_outer, allow_uncertified_boundary_band);
  if (!bz_record.clean && !allow_uncertified_boundary_band) {
    std::cerr << "ERROR: boundary band is not BZ-clean or exact BZ check is "
              << "unsupported for K=" << *k_sq << ". Use "
              << "--allow-uncertified-boundary-band only for non-accepted "
              << "diagnostics.\n";
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
  const std::uint64_t sample_seed = sample_seed_arg.value_or(
      default_sample_seed(*k_sq, *r_inner, *r_outer, *region_spec));
  std::optional<StratifiedTileSampler> sampler;
  if (sample_artifacts_enabled) {
    sampler.emplace(telemetry_level, sample_seed, tile_sample_count);
  }
  campaign::Verdict verdict = campaign::Verdict::kUnknown;
  campaign::SpanningTrace spanning_trace;
  campaign::ComponentCensus component_census;

  try {
    cuda_campaign::DispatchConfig config;
    config.host_chunk_tiles = chunk_size;
    config.overflow_diagnostics = overflow_diagnostics;
    config.collect_stage_timings = profile_path.has_value();
    config.max_overflow_diagnostics = kMaxOverflowDiagnostics;

    cuda_campaign::TileBatchDispatcher dispatcher(constants, config);
    campaign::StreamingCompositor compositor;
    compositor.init(grid);
    compositor.set_trace_spanning_path(trace_spanning_path);

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
                                              *r_inner, *r_outer, nullptr,
                                              early_exit_enabled)) {
              break;
            }
            if (drain_in_flight_and_ingest(in_flight, compositor, timings,
                                           run_stats, *r_inner, *r_outer,
                                           nullptr, early_exit_enabled)) {
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
                                              *r_inner, *r_outer, nullptr,
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
                                            *r_inner, *r_outer, nullptr,
                                            early_exit_enabled)) {
            // SPANNING was observed while ingesting the penultimate result.
          }
        }
        if (!run_stats.early_exit_taken) {
          if (drain_in_flight_and_ingest(in_flight, compositor, timings,
                                         run_stats, *r_inner, *r_outer,
                                         nullptr, early_exit_enabled)) {
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
                            timings, run_stats, *k_sq, *r_inner, *r_outer,
                            sampler.has_value() ? &*sampler : nullptr,
                            early_exit_enabled)) {
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
                          timings, run_stats, *k_sq, *r_inner, *r_outer,
                          sampler.has_value() ? &*sampler : nullptr,
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
        if (flush_batch(batch, dispatcher, compositor, snapshot_writer.get(),
                        timings, run_stats, *k_sq, *r_inner, *r_outer,
                        sampler.has_value() ? &*sampler : nullptr,
                        early_exit_enabled)) {
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
    component_census = compositor.component_census();
    if (span_cert_path.has_value()) {
      if (verdict != campaign::Verdict::kSpanning) {
        throw std::runtime_error("--emit-span-cert requested for non-SPANNING run");
      }
      write_span_certificate(std::filesystem::path(*span_cert_path), grid,
                             constants, *k_sq, *r_inner, *r_outer,
                             *region_spec, spanning_trace);
    }
    if (sampler.has_value()) {
      write_sample_artifacts(std::filesystem::path(*sample_manifest_path),
                             std::filesystem::path(*tile_sample_path), *sampler,
                             *k_sq, *r_inner, *r_outer, *region_spec,
                             run_stats);
    }
  } catch (const std::exception& e) {
    std::cerr << "Error in CUDA streaming campaign: " << e.what() << "\n";
    return 5;
  }
  const auto total_end = Clock::now();
  timings.total = total_end - total_start;

  if (profile_path.has_value()) {
    try {
      write_profile_json(std::filesystem::path(*profile_path), command, *k_sq,
                         *r_inner, *r_outer, *region_spec, grid, constants,
                         chunk_size, static_cast<std::uint64_t>(grid.total_tiles),
                         run_stats, timings, verdict, snapshot_enabled,
                         early_exit_enabled, bz_record, span_cert_path,
                         sample_manifest_path, tile_sample_path,
                         telemetry_level, stats_level, sample_seed,
                         tile_sample_count, trace_spanning, spanning_trace,
                         component_census);
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
            << "  telemetry: " << telemetry_level_name(telemetry_level) << "\n"
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
            << "  emitted_overflow_bit_count: "
            << run_stats.emitted_overflow_tileops << "\n"
            << "  constants_hash: " << constants.canonical_hash() << "\n"
            << "  mr_witness_sha256: "
            << campaign::CampaignConstants::mr_witness_set_sha256() << "\n";

  if (sample_artifacts_enabled) {
    std::cout << "  sample_manifest: " << *sample_manifest_path << "\n"
              << "  tile_sample_out: " << *tile_sample_path << "\n"
              << "  tile_samples_written: "
              << run_stats.tile_samples_written << "\n"
              << "  sample_seed: " << sample_seed << "\n"
              << "  sample_target_count: " << tile_sample_count << "\n";
  }

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
              << " inner_source_tile_index="
              << spanning_trace.inner_source_tile_index
              << " inner_source_group_label="
              << spanning_trace.inner_source_group_label
              << " outer_source_tile_index="
              << spanning_trace.outer_source_tile_index
              << " outer_source_group_label="
              << spanning_trace.outer_source_group_label
              << "\n";
    if (spanning_trace.path.enabled) {
      std::cout << "SPANNING_PATH:"
                << " reconstructed="
                << (spanning_trace.path.reconstructed ? 1 : 0)
                << " recorded_edges=" << spanning_trace.path.recorded_edges
                << " inner_path_edges="
                << spanning_trace.path.inner_path_edges.size()
                << " outer_path_edges="
                << spanning_trace.path.outer_path_edges.size()
                << " inner_source="
                << spanning_trace.path.inner_source.tile_index << "/"
                << spanning_trace.path.inner_source.group_label
                << " inner_endpoint="
                << spanning_trace.path.inner_endpoint.tile_index << "/"
                << spanning_trace.path.inner_endpoint.group_label
                << " outer_source="
                << spanning_trace.path.outer_source.tile_index << "/"
                << spanning_trace.path.outer_source.group_label
                << " outer_endpoint="
                << spanning_trace.path.outer_endpoint.tile_index << "/"
                << spanning_trace.path.outer_endpoint.group_label;
      if (spanning_trace.path.final_bridge_present) {
        const auto& edge = spanning_trace.path.final_bridge;
        std::cout << " final_bridge_event=" << edge.event
                  << " final_bridge_lhs=" << edge.lhs.tile_index << "/"
                  << edge.lhs.group_label << ":" << face_name(edge.lhs_face)
                  << "#" << static_cast<int>(edge.lhs_ordinal)
                  << " final_bridge_rhs=" << edge.rhs.tile_index << "/"
                  << edge.rhs.group_label << ":" << face_name(edge.rhs_face)
                  << "#" << static_cast<int>(edge.rhs_ordinal);
      }
      if (!spanning_trace.path.failure_reason.empty()) {
        std::cout << " failure_reason=\""
                  << spanning_trace.path.failure_reason << "\"";
      }
      std::cout << "\n";
    }
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
