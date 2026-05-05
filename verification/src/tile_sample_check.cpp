#include "independent_moat.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

using namespace moat_verify;

namespace {

constexpr std::array<const char*, 6> kAllowedSampleClasses = {
    "geo_I", "geo_O", "axis", "diagonal", "high_pressure",
    "deterministic_random"};

struct AuditManifest {
  std::string claim_id;
  RowConfig row{};
  std::uint64_t width = 0;
  std::string region;
  std::optional<std::uint64_t> sample_count;
  std::optional<std::uint64_t> target_tile_samples;
  std::map<std::string, std::uint64_t> quotas;
  std::map<std::string, bool> exhausted;
  std::map<std::string, std::uint64_t> class_counts;
  std::string row_class;
};

struct SampleRecord {
  RowConfig row{};
  std::uint64_t width = 0;
  std::string region;
  std::string claim_id;
  std::string sample_class;
  TileCoord tile{};
  TileOpLite expected{};
};

bool allowed_sample_class(const std::string& sample_class) {
  return std::find(kAllowedSampleClasses.begin(), kAllowedSampleClasses.end(),
                   sample_class) != kAllowedSampleClasses.end();
}

std::string allowed_classes_string() {
  std::ostringstream out;
  for (std::size_t i = 0; i < kAllowedSampleClasses.size(); ++i) {
    if (i != 0) out << ",";
    out << kAllowedSampleClasses[i];
  }
  return out.str();
}

std::uint64_t require_u64(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) {
    throw std::runtime_error(std::string("missing required field ") + key);
  }
  return j.at(key).get<std::uint64_t>();
}

std::uint32_t require_u32(const nlohmann::json& j, const char* key) {
  const std::uint64_t value = require_u64(j, key);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(std::string(key) + " exceeds uint32 range");
  }
  return static_cast<std::uint32_t>(value);
}

std::string require_string(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) {
    throw std::runtime_error(std::string("missing required field ") + key);
  }
  return j.at(key).get<std::string>();
}

std::uint64_t checked_width(const RowConfig& row) {
  if (row.r_outer <= row.r_inner) {
    throw std::runtime_error("r_outer must be greater than r_inner");
  }
  return row.r_outer - row.r_inner;
}

__int128 sq128(std::int64_t v) {
  return static_cast<__int128>(v) * static_cast<__int128>(v);
}

std::int64_t floor_isqrt_i128(__int128 n, std::int64_t upper) {
  std::int64_t lo = 0;
  std::int64_t hi = upper;
  while (lo < hi) {
    const std::int64_t mid = lo + (hi - lo + 1) / 2;
    if (sq128(mid) <= n) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

std::int64_t ceil_isqrt_i128(__int128 n, std::int64_t upper) {
  std::int64_t lo = 0;
  std::int64_t hi = upper;
  while (lo < hi) {
    const std::int64_t mid = lo + (hi - lo) / 2;
    if (sq128(mid) >= n) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

struct TileBox {
  std::int64_t x_lo = 0;
  std::int64_t x_hi = 0;
  std::int64_t y_lo = 0;
  std::int64_t y_hi = 0;
};

TileBox tile_box(std::int32_t i, std::int32_t j) {
  const std::int64_t x_lo = kTileSide * static_cast<std::int64_t>(i);
  const std::int64_t y_lo = kTileSide * static_cast<std::int64_t>(j);
  return TileBox{x_lo, x_lo + kTileSide, y_lo, y_lo + kTileSide};
}

bool definitely_inactive(std::int32_t i,
                         std::int32_t j,
                         const RowConfig& row) {
  const TileBox b = tile_box(i, j);
  if (b.x_hi < 0) return true;
  if (b.y_hi < b.x_lo) return true;

  const __int128 r_inner_sq = static_cast<__int128>(row.r_inner) *
                              static_cast<__int128>(row.r_inner);
  const __int128 r_outer_sq = static_cast<__int128>(row.r_outer) *
                              static_cast<__int128>(row.r_outer);
  const std::int64_t x_hi_pos = std::max<std::int64_t>(0, b.x_hi);
  const std::int64_t y_hi_pos = std::max<std::int64_t>(0, b.y_hi);
  if (sq128(x_hi_pos) + sq128(y_hi_pos) < r_inner_sq) return true;

  const std::int64_t x_lo_pos = std::max<std::int64_t>(0, b.x_lo);
  const std::int64_t y_lo_pos = std::max<std::int64_t>(0, b.y_lo);
  const std::int64_t y_min_on_diag =
      std::max<std::int64_t>(y_lo_pos, x_lo_pos);
  if (y_min_on_diag <= b.y_hi &&
      sq128(x_lo_pos) + sq128(y_min_on_diag) > r_outer_sq) {
    return true;
  }
  return false;
}

bool active_by_y_range(std::int32_t i,
                       std::int32_t j,
                       const RowConfig& row) {
  const TileBox b = tile_box(i, j);
  const std::int64_t x_start = std::max<std::int64_t>(0, b.x_lo);
  const std::int64_t x_end = b.x_hi;
  if (x_end < x_start) return false;

  const __int128 r_inner_sq = static_cast<__int128>(row.r_inner) *
                              static_cast<__int128>(row.r_inner);
  const __int128 r_outer_sq = static_cast<__int128>(row.r_outer) *
                              static_cast<__int128>(row.r_outer);
  const auto r_inner = static_cast<std::int64_t>(row.r_inner);
  const auto r_outer = static_cast<std::int64_t>(row.r_outer);

  for (std::int64_t x = x_start; x <= x_end; ++x) {
    const __int128 x_sq = sq128(x);
    const __int128 y_sq_ub = r_outer_sq - x_sq;
    if (y_sq_ub < 0) continue;
    const std::int64_t y_hi_annulus = floor_isqrt_i128(y_sq_ub, r_outer);
    const __int128 y_sq_lb = r_inner_sq - x_sq;
    const std::int64_t y_lo_annulus =
        (y_sq_lb <= 0) ? 0 : ceil_isqrt_i128(y_sq_lb, r_inner);
    const std::int64_t y_lo = std::max({b.y_lo, x, y_lo_annulus});
    const std::int64_t y_hi = std::min(b.y_hi, y_hi_annulus);
    if (y_lo <= y_hi) return true;
  }
  return false;
}

bool active_full_octant_tile(std::int32_t i,
                             std::int32_t j,
                             const RowConfig& row) {
  if (i < 0 || j < 0) return false;
  if (definitely_inactive(i, j, row)) return false;
  return active_by_y_range(i, j, row);
}

TileOpLite parse_tileop(const nlohmann::json& j) {
  TileOpLite op{};
  for (int i = 0; i < 4; ++i) op.n[static_cast<std::size_t>(i)] = j.at("n").at(i).get<std::uint8_t>();
  for (std::size_t i = 0; i < op.face_groups.size(); ++i) {
    op.face_groups[i] = j.at("face_groups").at(i).get<std::uint8_t>();
  }
  for (std::size_t i = 0; i < op.inner_flags.size(); ++i) {
    op.inner_flags[i] = j.at("inner_flags").at(i).get<std::uint8_t>();
    op.outer_flags[i] = j.at("outer_flags").at(i).get<std::uint8_t>();
  }
  op.tile_flags = j.at("tile_flags").get<std::uint8_t>();
  return op;
}

RowConfig parse_row(const nlohmann::json& j) {
  RowConfig row{require_u32(j, "k_sq"), require_u64(j, "r_inner"),
                require_u64(j, "r_outer")};
  checked_width(row);
  if (row.r_outer > (std::uint64_t{1} << 32)) {
    throw std::runtime_error("r_outer too large for uint64 squared arithmetic");
  }
  return row;
}

void require_width_region(std::uint64_t width,
                          const std::string& region,
                          const RowConfig& row) {
  if (width != checked_width(row)) {
    throw std::runtime_error("width does not equal r_outer-r_inner");
  }
  if (region != "full-octant") {
    throw std::runtime_error("region must be full-octant");
  }
}

bool population_exhausted_in_object(const nlohmann::json& object,
                                    const std::string& sample_class) {
  if (!object.is_object() || !object.contains(sample_class)) return false;
  const nlohmann::json& entry = object.at(sample_class);
  if (entry.is_boolean()) return entry.get<bool>();
  if (entry.is_object()) {
    if (entry.value("exhausted", false)) return true;
    if (entry.value("population_exhausted", false)) return true;
  }
  return false;
}

AuditManifest parse_manifest(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) throw std::runtime_error("could not open manifest");
  const nlohmann::json j = nlohmann::json::parse(in);

  const int schema_version = j.at("schema_version").get<int>();
  if (schema_version != 1 && schema_version != 2) {
    throw std::runtime_error("manifest schema_version must be 1 or 2");
  }
  AuditManifest manifest;
  manifest.claim_id = require_string(j, "claim_id");
  manifest.row = parse_row(j);
  manifest.width = require_u64(j, "width");
  manifest.region = require_string(j, "region");
  require_width_region(manifest.width, manifest.region, manifest.row);

  if (j.contains("sample_count")) {
    manifest.sample_count = j.at("sample_count").get<std::uint64_t>();
  }
  if (j.contains("target_tile_samples")) {
    manifest.target_tile_samples =
        j.at("target_tile_samples").get<std::uint64_t>();
  }
  if (j.contains("row_class")) {
    manifest.row_class = j.at("row_class").get<std::string>();
  }

  if (!j.contains("selection") || !j.at("selection").is_array()) {
    throw std::runtime_error("manifest missing selection quota array");
  }
  for (const nlohmann::json& entry : j.at("selection")) {
    const std::string sample_class = require_string(entry, "class");
    if (!allowed_sample_class(sample_class)) {
      throw std::runtime_error("manifest contains unsupported sample class " +
                               sample_class);
    }
    if (!entry.contains("target_count")) {
      throw std::runtime_error("manifest selection entry for " + sample_class +
                               " missing target_count");
    }
    if (!manifest.quotas.emplace(sample_class,
                                 entry.at("target_count").get<std::uint64_t>())
             .second) {
      throw std::runtime_error("manifest has duplicate quota for sample class " +
                               sample_class);
    }
    manifest.exhausted[sample_class] =
        entry.value("population_exhausted", false) ||
        entry.value("exhausted", false);
  }

  for (const char* sample_class : kAllowedSampleClasses) {
    if (manifest.quotas.find(sample_class) == manifest.quotas.end()) {
      throw std::runtime_error("manifest missing quota for sample class " +
                               std::string(sample_class));
    }
  }

  if (j.contains("population_exhaustion")) {
    const nlohmann::json& exhaustion = j.at("population_exhaustion");
    if (!exhaustion.is_object()) {
      throw std::runtime_error("population_exhaustion must be an object");
    }
    for (auto it = exhaustion.begin(); it != exhaustion.end(); ++it) {
      if (!allowed_sample_class(it.key())) {
        throw std::runtime_error("population_exhaustion contains unsupported "
                                 "sample class " +
                                 it.key());
      }
      manifest.exhausted[it.key()] =
          population_exhausted_in_object(exhaustion, it.key());
    }
  }

  if (j.contains("class_counts")) {
    const nlohmann::json& counts = j.at("class_counts");
    if (!counts.is_object()) {
      throw std::runtime_error("class_counts must be an object");
    }
    for (auto it = counts.begin(); it != counts.end(); ++it) {
      if (!allowed_sample_class(it.key())) {
        throw std::runtime_error("class_counts contains unsupported sample "
                                 "class " +
                                 it.key());
      }
      manifest.class_counts[it.key()] = it.value().get<std::uint64_t>();
    }
    for (const char* sample_class : kAllowedSampleClasses) {
      if (manifest.class_counts.find(sample_class) ==
          manifest.class_counts.end()) {
        throw std::runtime_error("class_counts missing sample class " +
                                 std::string(sample_class));
      }
    }
  }

  return manifest;
}

SampleRecord parse_sample(const nlohmann::json& item) {
  const int schema_version = item.at("schema_version").get<int>();
  if (schema_version != 1 && schema_version != 2) {
    throw std::runtime_error("sample schema_version must be 1 or 2");
  }

  SampleRecord sample;
  sample.row = parse_row(item);
  sample.width = require_u64(item, "width");
  sample.region = require_string(item, "region");
  require_width_region(sample.width, sample.region, sample.row);
  if (item.contains("claim_id")) {
    sample.claim_id = item.at("claim_id").get<std::string>();
  }
  sample.sample_class = require_string(item, "sample_class");
  if (!allowed_sample_class(sample.sample_class)) {
    throw std::runtime_error("unsupported sample_class " +
                             sample.sample_class + " (allowed: " +
                             allowed_classes_string() + ")");
  }

  const auto& t = item.at("tile");
  sample.tile = TileCoord{
      t.at("i").get<std::int32_t>(),
      t.at("j").get<std::int32_t>(),
      t.at("a_lo").get<std::int64_t>(),
      t.at("b_lo").get<std::int64_t>()};
  const std::int64_t expected_a_lo =
      kTileSide * static_cast<std::int64_t>(sample.tile.i);
  const std::int64_t expected_b_lo =
      kTileSide * static_cast<std::int64_t>(sample.tile.j);
  if (sample.tile.i < 0 || sample.tile.j < 0 ||
      sample.tile.a_lo != expected_a_lo ||
      sample.tile.b_lo != expected_b_lo) {
    throw std::runtime_error("tile coordinate does not match canonical "
                             "full-octant snapped grid");
  }
  if (!active_full_octant_tile(sample.tile.i, sample.tile.j, sample.row)) {
    throw std::runtime_error("sampled tile is outside the active full-octant "
                             "grid");
  }

  sample.expected = parse_tileop(item.at("tileop"));
  return sample;
}

int port_total(const TileOpLite& op) {
  return static_cast<int>(op.n[0]) + static_cast<int>(op.n[1]) +
         static_cast<int>(op.n[2]) + static_cast<int>(op.n[3]);
}

bool flag(const std::array<std::uint8_t, 16>& flags, int label) {
  const int bit = label - 1;
  return ((flags[static_cast<std::size_t>(bit >> 3)] >> (bit & 7)) & 1U) != 0;
}

std::vector<std::string> semantic_signatures(const TileOpLite& op) {
  std::array<std::string, 129> sig{};
  const int total = port_total(op);
  for (int pos = 0; pos < total; ++pos) {
    const int label = op.face_groups[static_cast<std::size_t>(pos)];
    if (label > 0 && label <= 128) {
      sig[static_cast<std::size_t>(label)] += "p" + std::to_string(pos) + ";";
    }
  }
  for (int label = 1; label <= 128; ++label) {
    if (flag(op.inner_flags, label)) sig[static_cast<std::size_t>(label)] += "I;";
    if (flag(op.outer_flags, label)) sig[static_cast<std::size_t>(label)] += "O;";
  }

  std::vector<std::string> out;
  for (int label = 1; label <= 128; ++label) {
    const std::string& s = sig[static_cast<std::size_t>(label)];
    if (!s.empty()) out.push_back(s);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void require_equal(const TileOpLite& actual, const TileOpLite& expected) {
  if (actual.n != expected.n) {
    throw std::runtime_error("n mismatch actual=[" +
                             std::to_string(actual.n[0]) + "," +
                             std::to_string(actual.n[1]) + "," +
                             std::to_string(actual.n[2]) + "," +
                             std::to_string(actual.n[3]) + "] expected=[" +
                             std::to_string(expected.n[0]) + "," +
                             std::to_string(expected.n[1]) + "," +
                             std::to_string(expected.n[2]) + "," +
                             std::to_string(expected.n[3]) + "]");
  }
  if (semantic_signatures(actual) != semantic_signatures(expected)) {
    throw std::runtime_error("semantic label signature mismatch");
  }
  if (actual.tile_flags != expected.tile_flags) {
    throw std::runtime_error("tile_flags mismatch");
  }
}

void require_same_row(const SampleRecord& sample,
                      const AuditManifest& manifest) {
  if (sample.row.k_sq != manifest.row.k_sq ||
      sample.row.r_inner != manifest.row.r_inner ||
      sample.row.r_outer != manifest.row.r_outer ||
      sample.width != manifest.width ||
      sample.region != manifest.region) {
    throw std::runtime_error("sample row metadata does not match manifest");
  }
  if (!sample.claim_id.empty() && sample.claim_id != manifest.claim_id) {
    throw std::runtime_error("sample claim_id does not match manifest");
  }
}

void require_same_row(const SampleRecord& sample, const SampleRecord& first) {
  if (sample.row.k_sq != first.row.k_sq ||
      sample.row.r_inner != first.row.r_inner ||
      sample.row.r_outer != first.row.r_outer ||
      sample.width != first.width ||
      sample.region != first.region ||
      sample.claim_id != first.claim_id) {
    throw std::runtime_error("sample row metadata is inconsistent within JSONL");
  }
}

std::string tile_key(const TileCoord& tile) {
  return std::to_string(tile.i) + "," + std::to_string(tile.j);
}

void check_manifest_counts(const AuditManifest& manifest,
                           std::uint64_t checked,
                           const std::map<std::string, std::uint64_t>& counts) {
  const bool global_population_exhausted =
      manifest.exhausted.count("deterministic_random") != 0 &&
      manifest.exhausted.at("deterministic_random");
  if (manifest.sample_count.has_value() && checked != *manifest.sample_count) {
    throw std::runtime_error("manifest sample_count mismatch: observed=" +
                             std::to_string(checked) + " manifest=" +
                             std::to_string(*manifest.sample_count));
  }
  if (manifest.target_tile_samples.has_value() &&
      checked < *manifest.target_tile_samples && !global_population_exhausted) {
    throw std::runtime_error("observed sample count below target_tile_samples: "
                             "observed=" +
                             std::to_string(checked) + " target=" +
                             std::to_string(*manifest.target_tile_samples));
  }
  if ((manifest.row_class == "accepted" ||
       manifest.row_class == "accepted_proof") &&
      checked < 512 && !global_population_exhausted) {
    throw std::runtime_error("accepted row sample count below 512 minimum");
  }

  for (const auto& [sample_class, target] : manifest.quotas) {
    const auto it = counts.find(sample_class);
    const std::uint64_t observed = (it == counts.end()) ? 0 : it->second;
    const bool exhausted =
        manifest.exhausted.count(sample_class) != 0 &&
        manifest.exhausted.at(sample_class);
    if (observed < target && !exhausted) {
      throw std::runtime_error("sample class " + sample_class +
                               " below quota: observed=" +
                               std::to_string(observed) + " target=" +
                               std::to_string(target));
    }
  }

  for (const auto& [sample_class, manifest_count] : manifest.class_counts) {
    const auto it = counts.find(sample_class);
    const std::uint64_t observed = (it == counts.end()) ? 0 : it->second;
    if (observed != manifest_count) {
      throw std::runtime_error("manifest class_counts mismatch for " +
                               sample_class + ": observed=" +
                               std::to_string(observed) + " manifest=" +
                               std::to_string(manifest_count));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string samples_path;
  std::string manifest_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--samples" && i + 1 < argc) {
      samples_path = argv[++i];
    } else if (a.rfind("--samples=", 0) == 0) {
      samples_path = a.substr(10);
    } else if (a == "--manifest" && i + 1 < argc) {
      manifest_path = argv[++i];
    } else if (a.rfind("--manifest=", 0) == 0) {
      manifest_path = a.substr(11);
    } else {
      std::cerr << "Usage: tile_sample_check --samples samples.jsonl [--manifest manifest.json]\n";
      return 2;
    }
  }
  if (samples_path.empty()) {
    std::cerr << "ERROR: --samples is required\n";
    return 2;
  }

  try {
    const std::optional<AuditManifest> manifest =
        manifest_path.empty() ? std::nullopt
                              : std::optional<AuditManifest>(
                                    parse_manifest(manifest_path));
    std::ifstream in(samples_path);
    if (!in.is_open()) throw std::runtime_error("could not open samples");
    std::string line;
    std::uint64_t checked = 0;
    std::map<std::string, std::uint64_t> class_counts;
    std::unordered_set<std::string> seen_tiles;
    std::optional<SampleRecord> first_sample;
    std::vector<SampleRecord> samples;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      const nlohmann::json item = nlohmann::json::parse(line);
      const SampleRecord sample = parse_sample(item);
      if (manifest.has_value()) {
        require_same_row(sample, *manifest);
      } else if (first_sample.has_value()) {
        require_same_row(sample, *first_sample);
      } else {
        first_sample = sample;
      }

      const std::string key = tile_key(sample.tile);
      if (!seen_tiles.insert(key).second) {
        throw std::runtime_error("duplicate sample tile " + key);
      }
      ++class_counts[sample.sample_class];
      samples.push_back(sample);
      ++checked;
    }
    if (checked == 0) {
      throw std::runtime_error("no tile samples found");
    }
    if (manifest.has_value()) {
      check_manifest_counts(*manifest, checked, class_counts);
    }

    std::atomic<std::size_t> next_index{0};
    std::mutex error_mutex;
    std::string first_error;
    const unsigned hardware_threads = std::thread::hardware_concurrency();
    const std::size_t worker_count = std::max<std::size_t>(
        1, std::min<std::size_t>(samples.size(), hardware_threads == 0 ? 1 : hardware_threads));
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      workers.emplace_back([&]() {
        while (true) {
          const std::size_t index = next_index.fetch_add(1);
          if (index >= samples.size()) return;
          {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!first_error.empty()) return;
          }
          const SampleRecord& sample = samples[index];
          try {
            const TileOpLite actual = build_tileop(sample.tile, sample.row);
            require_equal(actual, sample.expected);
          } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (first_error.empty()) {
              first_error = "tile (" + std::to_string(sample.tile.i) + "," +
                            std::to_string(sample.tile.j) + "): " + e.what();
            }
            return;
          }
        }
      });
    }
    for (auto& worker : workers) worker.join();
    if (!first_error.empty()) throw std::runtime_error(first_error);

    std::cout << "tile sample check PASS: checked=" << checked;
    if (manifest.has_value()) std::cout << " manifest=checked";
    std::cout << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
