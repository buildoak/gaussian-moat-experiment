#include "independent_moat.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using moat_verify::Point;
using moat_verify::RowConfig;

namespace {

enum class Status {
  Reject,
  RunContractPass,
  TileSampleAuditPass,
  SpanProofPass,
  MoatProofPass,
  ClaimProofMissing,
};

struct Row {
  std::string claim_id;
  std::uint32_t k_sq = 0;
  std::uint64_t r_inner = 0;
  std::uint64_t r_outer = 0;
  std::uint64_t width = 0;
  std::string region;
  std::string verdict;
  std::string verdict_mode;
  std::string row_class = "profile";
  std::string telemetry_level = "profile";
  bool claim_proof_required = true;
};

struct CheckResult {
  Status status = Status::RunContractPass;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  nlohmann::json detail = nlohmann::json::object();
};

std::string status_name(Status status) {
  switch (status) {
    case Status::Reject:
      return "REJECT";
    case Status::RunContractPass:
      return "RUN_CONTRACT_PASS";
    case Status::TileSampleAuditPass:
      return "TILE_SAMPLE_AUDIT_PASS";
    case Status::SpanProofPass:
      return "SPAN_PROOF_PASS";
    case Status::MoatProofPass:
      return "MOAT_PROOF_PASS";
    case Status::ClaimProofMissing:
      return "CLAIM_PROOF_MISSING";
  }
  return "REJECT";
}

bool is_object(const nlohmann::json& j) {
  return !j.is_null() && j.is_object();
}

const nlohmann::json& required_object(const nlohmann::json& parent,
                                      const char* key) {
  if (!parent.contains(key) || !parent.at(key).is_object()) {
    throw std::runtime_error(std::string("missing object: ") + key);
  }
  return parent.at(key);
}

std::optional<std::uint64_t> maybe_u64(const nlohmann::json& j,
                                       const std::string& key) {
  if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
  if (!j.at(key).is_number_unsigned() && !j.at(key).is_number_integer()) {
    throw std::runtime_error("field is not an integer: " + key);
  }
  const auto value = j.at(key).get<std::int64_t>();
  if (value < 0) throw std::runtime_error("field is negative: " + key);
  return static_cast<std::uint64_t>(value);
}

std::optional<std::string> maybe_string(const nlohmann::json& j,
                                        const std::string& key) {
  if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
  if (!j.at(key).is_string()) throw std::runtime_error("field is not a string: " + key);
  return j.at(key).get<std::string>();
}

std::optional<std::string> maybe_command_string(const nlohmann::json& j,
                                                const std::string& key) {
  if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
  const auto& value = j.at(key);
  if (value.is_string()) return value.get<std::string>();
  if (!value.is_array()) throw std::runtime_error("field is not a string or argv array: " + key);

  std::ostringstream ss;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (!value.at(i).is_string()) {
      throw std::runtime_error("command argv element is not a string: " + key);
    }
    if (i != 0) ss << ' ';
    ss << value.at(i).get<std::string>();
  }
  return ss.str();
}

std::uint64_t require_u64(const nlohmann::json& j, const std::string& key) {
  const auto value = maybe_u64(j, key);
  if (!value) throw std::runtime_error("missing integer: " + key);
  return *value;
}

std::string require_string(const nlohmann::json& j, const std::string& key) {
  const auto value = maybe_string(j, key);
  if (!value || value->empty()) throw std::runtime_error("missing string: " + key);
  return *value;
}

std::optional<std::uint64_t> row_number_from(const nlohmann::json& j,
                                             const std::string& key) {
  if (!is_object(j)) return std::nullopt;
  if (const auto value = maybe_u64(j, key)) return value;
  if (j.contains("radii") && j.at("radii").is_object()) {
    return maybe_u64(j.at("radii"), key);
  }
  return std::nullopt;
}

std::optional<std::string> row_string_from(const nlohmann::json& j,
                                           const std::string& key) {
  if (!is_object(j)) return std::nullopt;
  if (const auto value = maybe_string(j, key)) return value;
  if (j.contains("row") && j.at("row").is_object()) {
    return maybe_string(j.at("row"), key);
  }
  return std::nullopt;
}

Row parse_row(const nlohmann::json& row_json) {
  Row row;
  row.claim_id = require_string(row_json, "claim_id");
  row.k_sq = static_cast<std::uint32_t>(require_u64(row_json, "k_sq"));
  row.r_inner = require_u64(row_json, "r_inner");
  row.r_outer = require_u64(row_json, "r_outer");
  row.width = require_u64(row_json, "width");
  row.region = require_string(row_json, "region");
  row.verdict = require_string(row_json, "verdict");
  row.verdict_mode = require_string(row_json, "verdict_mode");
  if (const auto value = maybe_string(row_json, "row_class")) row.row_class = *value;
  if (const auto value = maybe_string(row_json, "telemetry_level")) row.telemetry_level = *value;
  if (row_json.contains("claim_proof_required")) {
    if (!row_json.at("claim_proof_required").is_boolean()) {
      throw std::runtime_error("claim_proof_required must be boolean");
    }
    row.claim_proof_required = row_json.at("claim_proof_required").get<bool>();
  }
  return row;
}

void add_error(CheckResult& result, const std::string& message) {
  result.errors.push_back(message);
}

void add_warning(CheckResult& result, const std::string& message) {
  result.warnings.push_back(message);
}

bool is_nonnegative_integer_value(const nlohmann::json& value) {
  if (value.is_number_unsigned()) return true;
  if (value.is_number_integer()) return value.get<std::int64_t>() >= 0;
  return false;
}

bool is_nonnegative_number_value(const nlohmann::json& value) {
  if (value.is_number_unsigned()) return true;
  if (value.is_number_integer()) return value.get<std::int64_t>() >= 0;
  if (value.is_number_float()) return value.get<double>() >= 0.0;
  return false;
}

std::optional<std::uint64_t> optional_u64_field(CheckResult& result,
                                                const nlohmann::json& obj,
                                                const std::string& path,
                                                const std::string& key) {
  if (!obj.contains(key) || obj.at(key).is_null()) return std::nullopt;
  if (!is_nonnegative_integer_value(obj.at(key))) {
    add_error(result, path + "." + key + " must be a nonnegative integer");
    return std::nullopt;
  }
  if (obj.at(key).is_number_unsigned()) return obj.at(key).get<std::uint64_t>();
  return static_cast<std::uint64_t>(obj.at(key).get<std::int64_t>());
}

std::optional<std::string> optional_string_field(CheckResult& result,
                                                 const nlohmann::json& obj,
                                                 const std::string& path,
                                                 const std::string& key) {
  if (!obj.contains(key) || obj.at(key).is_null()) return std::nullopt;
  if (!obj.at(key).is_string()) {
    add_error(result, path + "." + key + " must be a string");
    return std::nullopt;
  }
  const auto value = obj.at(key).get<std::string>();
  if (value.empty()) {
    add_error(result, path + "." + key + " must not be empty");
    return std::nullopt;
  }
  return value;
}

void compare_number(CheckResult& result,
                    const nlohmann::json& obj,
                    const std::string& obj_name,
                    const std::string& field,
                    std::uint64_t expected) {
  const auto got = row_number_from(obj, field);
  if (!got) return;
  if (*got != expected) {
    std::ostringstream ss;
    ss << obj_name << "." << field << " mismatch: got " << *got
       << " expected " << expected;
    add_error(result, ss.str());
  }
}

void compare_string(CheckResult& result,
                    const nlohmann::json& obj,
                    const std::string& obj_name,
                    const std::string& field,
                    const std::string& expected) {
  const auto got = row_string_from(obj, field);
  if (!got) return;
  if (*got != expected) {
    add_error(result, obj_name + "." + field + " mismatch: got " + *got +
                          " expected " + expected);
  }
}

void compare_row_shape(CheckResult& result,
                       const Row& row,
                       const nlohmann::json& obj,
                       const std::string& obj_name) {
  compare_number(result, obj, obj_name, "k_sq", row.k_sq);
  compare_number(result, obj, obj_name, "r_inner", row.r_inner);
  compare_number(result, obj, obj_name, "r_outer", row.r_outer);
  compare_number(result, obj, obj_name, "width", row.width);
  compare_string(result, obj, obj_name, "region", row.region);
  compare_string(result, obj, obj_name, "verdict", row.verdict);
}

std::optional<std::string> nested_string(const nlohmann::json& j,
                                         const std::vector<std::string>& path) {
  const nlohmann::json* cur = &j;
  for (const std::string& key : path) {
    if (!cur->is_object() || !cur->contains(key)) return std::nullopt;
    cur = &cur->at(key);
  }
  if (!cur->is_string()) return std::nullopt;
  return cur->get<std::string>();
}

std::optional<std::uint64_t> nested_u64(const nlohmann::json& j,
                                        const std::vector<std::string>& path) {
  const nlohmann::json* cur = &j;
  for (const std::string& key : path) {
    if (!cur->is_object() || !cur->contains(key)) return std::nullopt;
    cur = &cur->at(key);
  }
  if (!cur->is_number_unsigned() && !cur->is_number_integer()) return std::nullopt;
  const auto value = cur->get<std::int64_t>();
  if (value < 0) return std::nullopt;
  return static_cast<std::uint64_t>(value);
}

std::optional<std::string> first_present(
    const std::vector<std::optional<std::string>>& values) {
  for (const auto& value : values) {
    if (value && !value->empty()) return value;
  }
  return std::nullopt;
}

bool telemetry_at_least(const std::string& got, const std::string& needed) {
  auto rank = [](const std::string& level) {
    if (level == "none") return 0;
    if (level == "profile") return 1;
    if (level == "audit") return 2;
    if (level == "full") return 3;
    return -1;
  };
  return rank(got) >= rank(needed) && rank(needed) >= 0;
}

void check_row_contract(CheckResult& result, const Row& row) {
  if (row.k_sq == 0) add_error(result, "k_sq must be positive");
  if (row.r_outer <= row.r_inner) add_error(result, "r_outer must exceed r_inner");
  if (row.width != row.r_outer - row.r_inner) {
    add_error(result, "width must equal r_outer - r_inner");
  }
  if (row.region != "full-octant") add_error(result, "region must be full-octant");
  if (row.verdict != "SPANNING" && row.verdict != "MOAT") {
    add_error(result, "verdict must be SPANNING or MOAT");
  }
  if (row.verdict == "SPANNING" && row.verdict_mode != "ANY-SPAN") {
    add_error(result, "SPANNING verdict_mode must be ANY-SPAN");
  }
  if (row.verdict == "MOAT" && row.verdict_mode != "ANY-SHELL-MOAT") {
    add_error(result, "MOAT verdict_mode must be ANY-SHELL-MOAT");
  }
  if (!telemetry_at_least(row.telemetry_level, row.row_class == "accepted" ? "audit" : "profile")) {
    add_error(result, "telemetry level is too weak for row_class=" + row.row_class);
  }
  if (row.width < static_cast<std::uint64_t>(moat_verify::collar(row.k_sq))) {
    add_error(result, "annulus width is smaller than sqrt(K) collar");
  }
}

void check_bz(CheckResult& result, const nlohmann::json& bundle) {
  const nlohmann::json* bz = nullptr;
  if (bundle.contains("bz") && bundle.at("bz").is_object()) {
    bz = &bundle.at("bz");
  } else if (bundle.contains("profile") && bundle.at("profile").contains("bz") &&
             bundle.at("profile").at("bz").is_object()) {
    bz = &bundle.at("profile").at("bz");
  }
  if (bz == nullptr) {
    add_error(result, "missing bz record");
    return;
  }
  auto require_bool = [&](const char* key, bool expected) {
    if (!bz->contains(key) || !bz->at(key).is_boolean()) {
      add_error(result, std::string("bz.") + key + " missing or not boolean");
      return;
    }
    if (bz->at(key).get<bool>() != expected) {
      add_error(result, std::string("bz.") + key + " must be " +
                            (expected ? "true" : "false"));
    }
  };
  require_bool("checked", true);
  require_bool("clean", true);
  require_bool("override_used", false);
  const auto bad_norm_count = maybe_u64(*bz, "bad_norm_count");
  if (!bad_norm_count) {
    add_error(result, "bz.bad_norm_count missing");
  } else if (*bad_norm_count != 0) {
    add_error(result, "bz.bad_norm_count must be zero");
  }
}

void scan_overflow_value(CheckResult& result,
                         const nlohmann::json& value,
                         const std::string& path) {
  if (value.is_number_integer() || value.is_number_unsigned()) {
    if (value.get<std::int64_t>() != 0) add_error(result, "nonzero overflow counter: " + path);
    return;
  }
  if (value.is_boolean()) {
    if (value.get<bool>()) add_error(result, "true overflow flag: " + path);
    return;
  }
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      scan_overflow_value(result, it.value(), path + "." + it.key());
    }
  }
}

void check_overflows(CheckResult& result, const nlohmann::json& bundle) {
  const nlohmann::json* counters = nullptr;
  if (bundle.contains("overflow_counters")) {
    counters = &bundle.at("overflow_counters");
  } else if (bundle.contains("profile") && bundle.at("profile").contains("overflow_counters")) {
    counters = &bundle.at("profile").at("overflow_counters");
  }
  if (counters == nullptr) {
    add_error(result, "missing overflow_counters");
    return;
  }
  scan_overflow_value(result, *counters, "overflow_counters");
  if (const auto emitted = nested_u64(bundle, {"profile", "emitted_tileop_overflow_count"})) {
    if (*emitted != 0) add_error(result, "profile.emitted_tileop_overflow_count must be zero");
  }
}

void check_build_identity(CheckResult& result, const nlohmann::json& profile) {
  const auto commit = first_present({
      maybe_string(profile, "commit"),
      nested_string(profile, {"build", "commit"}),
  });
  if (!commit || commit->empty()) add_error(result, "missing commit");

  const auto build_identity = first_present({
      maybe_string(profile, "build_identity"),
      nested_string(profile, {"build", "identity"}),
      nested_string(profile, {"build", "id"}),
  });
  if (!build_identity || build_identity->empty()) add_error(result, "missing build identity");

  const auto cuda_arch = first_present({
      maybe_string(profile, "cuda_arch"),
      nested_string(profile, {"build", "cuda_arch"}),
  });
  if (!cuda_arch || cuda_arch->empty()) add_error(result, "missing CUDA architecture");

  const auto grid_hash = first_present({
      nested_string(profile, {"hashes", "grid"}),
      nested_string(profile, {"hashes", "grid_hash"}),
  });
  const auto constants_hash = first_present({
      nested_string(profile, {"hashes", "constants"}),
      nested_string(profile, {"hashes", "constants_hash"}),
  });
  const auto mr_witness_hash = first_present({
      nested_string(profile, {"hashes", "mr_witness"}),
      nested_string(profile, {"hashes", "mr_witness_hash"}),
  });
  if (!grid_hash || grid_hash->empty()) add_error(result, "missing grid hash");
  if (!constants_hash || constants_hash->empty()) add_error(result, "missing constants hash");
  if (!mr_witness_hash || mr_witness_hash->empty()) add_error(result, "missing MR witness hash");
}

bool valid_sha256(const std::string& s);

void check_nonnegative_counter_field(CheckResult& result,
                                     const nlohmann::json& obj,
                                     const std::string& path,
                                     const std::string& key,
                                     bool required) {
  if (!obj.contains(key) || obj.at(key).is_null()) {
    if (required) add_error(result, path + "." + key + " missing");
    return;
  }
  if (!is_nonnegative_integer_value(obj.at(key))) {
    add_error(result, path + "." + key + " must be a nonnegative integer");
  }
}

void check_distribution_counts(CheckResult& result,
                               const nlohmann::json& counts,
                               const std::string& path) {
  if (!counts.is_object()) {
    add_error(result, path + " must be an object of nonnegative counts");
    return;
  }
  if (counts.empty()) {
    add_error(result, path + " must contain at least one count");
    return;
  }
  for (auto it = counts.begin(); it != counts.end(); ++it) {
    const std::string item_path = path + "." + it.key();
    if (!is_nonnegative_number_value(it.value())) {
      add_error(result, item_path + " must be a nonnegative count");
    }
  }
}

void check_distribution_field(CheckResult& result,
                              const nlohmann::json& stats,
                              const std::string& stats_path,
                              const std::string& key) {
  if (!stats.contains(key)) return;
  const std::string path = stats_path + "." + key;
  const auto& dist = stats.at(key);
  if (dist.is_null()) {
    add_warning(result, path + " is null; distribution not checked");
    return;
  }
  if (!dist.is_object()) {
    add_error(result, path + " must be object-shaped");
    return;
  }
  if (dist.contains("buckets")) {
    if (!dist.at("buckets").is_array()) {
      add_error(result, path + ".buckets must be an array");
    } else if (dist.at("buckets").empty()) {
      add_error(result, path + ".buckets must not be empty");
    } else {
      for (std::size_t i = 0; i < dist.at("buckets").size(); ++i) {
        const auto& bucket = dist.at("buckets").at(i);
        const std::string bucket_path =
            path + ".buckets[" + std::to_string(i) + "]";
        if (!bucket.is_object()) {
          add_error(result, bucket_path + " must be object");
          continue;
        }
        check_nonnegative_counter_field(result, bucket, bucket_path, "value",
                                        true);
        check_nonnegative_counter_field(result, bucket, bucket_path, "count",
                                        true);
      }
    }
    for (const char* counter :
         {"observed_min", "observed_max", "sample_count", "total_count"}) {
      check_nonnegative_counter_field(result, dist, path, counter, true);
    }
    return;
  }
  if (dist.contains("counts")) {
    check_distribution_counts(result, dist.at("counts"), path + ".counts");
  } else {
    check_distribution_counts(result, dist, path);
  }
}

void check_high_pressure_tiles(CheckResult& result,
                               const nlohmann::json& stats,
                               const std::string& stats_path,
                               const std::string& key) {
  if (!stats.contains(key)) return;
  const std::string path = stats_path + "." + key;
  const auto& tiles = stats.at(key);
  if (tiles.is_null()) {
    add_warning(result, path + " is null; high-pressure tiles not checked");
    return;
  }
  if (!tiles.is_array()) {
    add_error(result, path + " must be an array");
    return;
  }
  for (std::size_t i = 0; i < tiles.size(); ++i) {
    const auto& entry = tiles.at(i);
    const std::string entry_path = path + "[" + std::to_string(i) + "]";
    if (!entry.is_object()) {
      add_error(result, entry_path + " must be object");
      continue;
    }
    if (entry.contains("tile") && !entry.at("tile").is_null()) {
      if (entry.at("tile").is_object() && entry.at("tile").empty()) {
        add_error(result, entry_path + ".tile must not be empty");
      } else if (entry.at("tile").is_array() && entry.at("tile").size() < 2) {
        add_error(result, entry_path + ".tile array must contain at least two coordinates");
      }
    } else {
      check_nonnegative_counter_field(result, entry, entry_path, "tile_i",
                                      true);
      check_nonnegative_counter_field(result, entry, entry_path, "tile_j",
                                      true);
    }
    const bool has_score = entry.contains("score") && !entry.at("score").is_null();
    const bool has_pressure_score =
        entry.contains("pressure_score") && !entry.at("pressure_score").is_null();
    if (!has_score && !has_pressure_score) {
      add_error(result, entry_path + ".score missing");
    }
    if (has_score && !is_nonnegative_number_value(entry.at("score"))) {
      add_error(result, entry_path + ".score must be a nonnegative number");
    }
    if (has_pressure_score &&
        !is_nonnegative_number_value(entry.at("pressure_score"))) {
      add_error(result,
                entry_path + ".pressure_score must be a nonnegative number");
    }
  }
}

void check_component_size_array(CheckResult& result,
                                const nlohmann::json& census,
                                const std::string& census_path,
                                const std::string& key) {
  if (!census.contains(key) || census.at(key).is_null()) return;
  const std::string path = census_path + "." + key;
  const auto& values = census.at(key);
  if (!values.is_array()) {
    add_error(result, path + " must be an array");
    return;
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto& item = values.at(i);
    const std::string item_path = path + "[" + std::to_string(i) + "]";
    if (is_nonnegative_integer_value(item)) continue;
    if (item.is_object()) {
      const bool has_size =
          item.contains("size") && is_nonnegative_integer_value(item.at("size"));
      const bool has_group_count =
          item.contains("group_count") &&
          is_nonnegative_integer_value(item.at("group_count"));
      const bool has_tile_count =
          item.contains("tile_count") &&
          is_nonnegative_integer_value(item.at("tile_count"));
      if (!has_size && !has_group_count && !has_tile_count) {
        add_error(result, item_path +
                              " must contain size, group_count, or tile_count");
      }
      continue;
    }
    add_error(result, item_path +
                          " must be a nonnegative integer or component object");
  }
}

void check_component_census(CheckResult& result,
                            const nlohmann::json& stats,
                            const std::string& stats_path) {
  if (!stats.contains("component_census")) return;
  const std::string path = stats_path + ".component_census";
  const auto& census = stats.at("component_census");
  if (census.is_null()) {
    add_warning(result, path + " is null; component census not checked");
    return;
  }
  if (!census.is_object()) {
    add_error(result, path + " must be object");
    return;
  }
  check_nonnegative_counter_field(result, census, path, "i_only_components", true);
  check_nonnegative_counter_field(result, census, path, "o_only_components", true);
  check_nonnegative_counter_field(result, census, path, "i_and_o_components", true);
  check_component_size_array(result, census, path, "largest_component_sizes");
  check_component_size_array(result, census, path, "largest_boundary_touching_components");
}

void check_stats_v2(CheckResult& result,
                    const nlohmann::json& owner,
                    const std::string& owner_path) {
  if (!owner.contains("stats_v2")) return;
  const std::string stats_path = owner_path.empty() ? "stats_v2" : owner_path + ".stats_v2";
  const auto& stats = owner.at("stats_v2");
  if (stats.is_null()) {
    add_warning(result, stats_path + " is null; analytical telemetry not checked");
    return;
  }
  if (!stats.is_object()) {
    add_error(result, stats_path + " must be object");
    return;
  }

  for (const std::string& key : {
           "geo_i_tiles",
           "geo_o_tiles",
           "geo_i_ports",
           "geo_o_ports",
           "sample_count",
           "tile_sample_count",
           "tile_samples_written",
       }) {
    check_nonnegative_counter_field(result, stats, stats_path, key, false);
  }

  for (const std::string& key : {
           "candidate_count_distribution",
           "candidate_counts",
           "gaussian_prime_count_distribution",
           "prime_count_distribution",
           "prime_counts",
           "group_count_distribution",
           "group_counts",
           "total_port_count_distribution",
           "port_count_distribution",
           "port_counts",
           "max_face_port_count_distribution",
           "max_face_port_counts",
       }) {
    check_distribution_field(result, stats, stats_path, key);
  }
  if (stats.contains("distributions") && stats.at("distributions").is_object()) {
    const auto& distributions = stats.at("distributions");
    for (const std::string& key : {
             "candidate_counts",
             "gaussian_prime_counts",
             "group_counts",
             "total_port_counts",
             "max_face_port_counts",
         }) {
      check_distribution_field(result, distributions,
                               stats_path + ".distributions", key);
    }
  }

  check_high_pressure_tiles(result, stats, stats_path, "high_pressure_tiles");
  check_high_pressure_tiles(result, stats, stats_path, "high_pressure");
  check_component_census(result, stats, stats_path);

  if (stats.contains("snapshot_sha256") && !stats.at("snapshot_sha256").is_null()) {
    const auto sha = optional_string_field(result, stats, stats_path, "snapshot_sha256");
    if (sha && !valid_sha256(*sha)) {
      add_error(result, stats_path + ".snapshot_sha256 must be a lowercase sha256");
    }
  }
  optional_string_field(result, stats, stats_path, "snapshot_path");
  optional_string_field(result, stats, stats_path, "sample_manifest_path");
  optional_string_field(result, stats, stats_path, "tile_sample_path");
}

const nlohmann::json* artifact_table(const nlohmann::json& bundle) {
  if (bundle.contains("artifacts")) return &bundle.at("artifacts");
  if (bundle.contains("profile") && bundle.at("profile").contains("artifacts")) {
    return &bundle.at("profile").at("artifacts");
  }
  return nullptr;
}

bool valid_sha256(const std::string& s) {
  if (s.size() != 64) return false;
  for (const char ch : s) {
    const bool ok = ('0' <= ch && ch <= '9') || ('a' <= ch && ch <= 'f');
    if (!ok) return false;
  }
  return true;
}

bool artifact_matches_cert(const nlohmann::json& artifact,
                           const std::string& cert_path) {
  const auto name = maybe_string(artifact, "name");
  const auto type = maybe_string(artifact, "type");
  const auto path = maybe_string(artifact, "path");
  return (name && *name == "span_certificate") ||
         (type && *type == "span_certificate") ||
         (!cert_path.empty() && path && *path == cert_path);
}

std::vector<nlohmann::json> artifact_entries(CheckResult& result,
                                             const nlohmann::json& artifacts,
                                             const std::string& path) {
  std::vector<nlohmann::json> entries;
  if (artifacts.is_array()) {
    for (const auto& item : artifacts) entries.push_back(item);
    return entries;
  }
  if (artifacts.is_object()) {
    for (auto it = artifacts.begin(); it != artifacts.end(); ++it) {
      if (it.value().is_object()) {
        nlohmann::json item = it.value();
        if (!item.contains("name")) item["name"] = it.key();
        entries.push_back(item);
      } else if (it.value().is_string()) {
        entries.push_back({{"name", it.key()}, {"path", it.value().get<std::string>()}});
      } else {
        add_error(result, path + "." + it.key() + " artifact entry must be object or path string");
      }
    }
    return entries;
  }
  add_error(result, path + " must be array or object");
  return entries;
}

void check_artifact_entry(CheckResult& result,
                          const nlohmann::json& artifact,
                          const std::string& path,
                          bool require_hash_size_schema) {
  if (!artifact.is_object()) {
    add_error(result, path + " must be object");
    return;
  }

  const auto name = optional_string_field(result, artifact, path, "name");
  optional_string_field(result, artifact, path, "type");
  const auto artifact_path = optional_string_field(result, artifact, path, "path");
  if (!name && !artifact_path) add_error(result, path + " missing name/path");

  bool saw_sha = false;
  if (artifact.contains("sha256") && !artifact.at("sha256").is_null()) {
    saw_sha = true;
    const auto sha = optional_string_field(result, artifact, path, "sha256");
    if (sha && !valid_sha256(*sha)) {
      add_error(result, path + ".sha256 must be a lowercase sha256");
    }
  }

  const bool saw_size = (artifact.contains("size_bytes") && !artifact.at("size_bytes").is_null()) ||
                        (artifact.contains("size") && !artifact.at("size").is_null());
  const auto size_bytes = optional_u64_field(result, artifact, path, "size_bytes");
  const auto size = optional_u64_field(result, artifact, path, "size");
  if (size_bytes && size && *size_bytes != *size) {
    add_error(result, path + ".size must match size_bytes when both are present");
  }

  const auto schema_version = optional_u64_field(result, artifact, path, "schema_version");
  if (schema_version && *schema_version == 0) {
    add_error(result, path + ".schema_version must be positive");
  }

  if (require_hash_size_schema) {
    if (!saw_sha) add_error(result, path + " missing sha256");
    if (!saw_size) add_error(result, path + " missing size/size_bytes");
    if (!schema_version) add_error(result, path + " missing schema_version");
  } else {
    if (!saw_sha) add_warning(result, path + " missing sha256");
    if (!saw_size) add_warning(result, path + " missing size/size_bytes");
  }
}

void check_artifact_ledger(CheckResult& result,
                           const nlohmann::json& owner,
                           const std::string& owner_path,
                           const std::string& key) {
  if (!owner.contains(key)) return;
  const std::string path = owner_path.empty() ? key : owner_path + "." + key;
  const auto& ledger = owner.at(key);
  if (ledger.is_null()) {
    add_warning(result, path + " is null; artifact ledger not checked");
    return;
  }
  const auto entries = artifact_entries(result, ledger, path);
  if (entries.empty() && (ledger.is_array() || ledger.is_object())) {
    add_warning(result, path + " is empty");
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    check_artifact_entry(result, entries.at(i), path + "[" + std::to_string(i) + "]", false);
  }
}

void check_artifact_ledgers(CheckResult& result,
                            const nlohmann::json& bundle,
                            const nlohmann::json& profile) {
  for (const std::string& key : {"artifact_ledger", "artifact_hash_ledger"}) {
    check_artifact_ledger(result, bundle, "", key);
    check_artifact_ledger(result, profile, "profile", key);
  }
  if (profile.contains("stats_v2") && profile.at("stats_v2").is_object()) {
    for (const std::string& key : {"artifact_ledger", "artifact_hash_ledger"}) {
      check_artifact_ledger(result, profile.at("stats_v2"), "profile.stats_v2", key);
    }
  }
  if (bundle.contains("stats_v2") && bundle.at("stats_v2").is_object()) {
    for (const std::string& key : {"artifact_ledger", "artifact_hash_ledger"}) {
      check_artifact_ledger(result, bundle.at("stats_v2"), "stats_v2", key);
    }
  }
}

bool check_artifacts(CheckResult& result,
                     const nlohmann::json& bundle,
                     bool require_any_artifact,
                     const std::string& cert_path) {
  const nlohmann::json* artifacts = artifact_table(bundle);
  if (artifacts == nullptr) {
    if (require_any_artifact) add_error(result, "missing artifact table");
    return false;
  }

  const std::vector<nlohmann::json> entries = artifact_entries(result, *artifacts, "artifacts");

  if (entries.empty() && require_any_artifact) add_error(result, "artifact table is empty");

  bool saw_cert = false;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& artifact = entries.at(i);
    check_artifact_entry(result, artifact, "artifacts[" + std::to_string(i) + "]",
                         require_any_artifact);
    saw_cert = saw_cert || artifact_matches_cert(artifact, cert_path);
  }
  return saw_cert;
}

void check_moat_full_ingest(CheckResult& result,
                            const Row& row,
                            const nlohmann::json& profile) {
  if (row.verdict != "MOAT") return;
  if (const auto full = nested_string(profile, {"early_exit", "state"})) {
    if (*full == "full-ingest") return;
  }
  if (profile.contains("early_exit") && profile.at("early_exit").is_object()) {
    const auto& ee = profile.at("early_exit");
    if (ee.contains("full_ingest") && ee.at("full_ingest").is_boolean() &&
        ee.at("full_ingest").get<bool>()) {
      return;
    }
    if (ee.contains("taken") && ee.at("taken").is_boolean() && ee.at("taken").get<bool>()) {
      add_error(result, "MOAT cannot use early-exit taken=true");
      return;
    }
  }
  const auto produced = nested_u64(profile, {"tiles", "produced"});
  const auto ingested = nested_u64(profile, {"tiles", "ingested"});
  if (produced && ingested && *produced == *ingested) return;
  add_error(result, "MOAT verdict requires full ingest evidence");
}

void check_active_tile_count(CheckResult& result,
                             const nlohmann::json& bundle,
                             const nlohmann::json& profile) {
  const auto enumerated = nested_u64(bundle, {"run_contract", "enumerated_active_tiles"});
  if (!enumerated) return;
  const auto profile_active = nested_u64(profile, {"tiles", "active"});
  if (!profile_active) {
    add_error(result, "run_contract enumerated active tiles present but profile.tiles.active missing");
    return;
  }
  if (*enumerated != *profile_active) {
    add_error(result, "independent active tile count disagrees with profile");
  }
}

bool check_sample_audit(CheckResult& result, const Row& row, const nlohmann::json& bundle) {
  if (!bundle.contains("sample_audit") || bundle.at("sample_audit").is_null()) return false;
  const auto& audit = bundle.at("sample_audit");
  if (!audit.is_object()) {
    add_error(result, "sample_audit must be object");
    return false;
  }
  if (const auto present = audit.find("present"); present != audit.end() &&
                                           present->is_boolean() && !present->get<bool>()) {
    return false;
  }
  if (require_string(audit, "status") != "PASS") {
    add_error(result, "sample_audit.status must be PASS");
  }

  const auto class_exhausted = [&](const std::string& klass) {
    if (audit.contains("population_exhausted") && audit.at("population_exhausted").is_object() &&
        audit.at("population_exhausted").value(klass, false)) {
      return true;
    }
    if (audit.contains("population_exhaustion") && audit.at("population_exhaustion").is_object()) {
      const auto& exhaustion = audit.at("population_exhaustion");
      if (exhaustion.contains(klass) && exhaustion.at(klass).is_object() &&
          exhaustion.at(klass).value("population_exhausted", false)) {
        return true;
      }
    }
    if (audit.contains("manifest") && audit.at("manifest").is_object()) {
      const auto& manifest = audit.at("manifest");
      if (manifest.contains("population_exhaustion") &&
          manifest.at("population_exhaustion").is_object()) {
        const auto& exhaustion = manifest.at("population_exhaustion");
        if (exhaustion.contains(klass) && exhaustion.at(klass).is_object() &&
            exhaustion.at(klass).value("population_exhausted", false)) {
          return true;
        }
      }
      if (manifest.contains("sample_plan") && manifest.at("sample_plan").is_object()) {
        const auto& plan = manifest.at("sample_plan");
        if (plan.contains("quota_status") && plan.at("quota_status").is_object()) {
          const auto& quota_status = plan.at("quota_status");
          if (quota_status.contains(klass) && quota_status.at(klass).is_object() &&
              quota_status.at(klass).value("population_exhausted", false)) {
            return true;
          }
        }
      }
    }
    return false;
  };

  const auto all_requested_classes_exhausted = [&]() {
    if (!audit.contains("quotas") || !audit.at("quotas").is_object()) return false;
    for (auto it = audit.at("quotas").begin(); it != audit.at("quotas").end(); ++it) {
      if (!class_exhausted(it.key())) return false;
    }
    return true;
  };

  const std::uint64_t sample_count = require_u64(audit, "sample_count");
  const std::uint64_t minimum = row.row_class == "accepted" ? 4096 : 1024;
  if (sample_count < minimum && !all_requested_classes_exhausted()) {
    std::ostringstream ss;
    ss << "sample_count below minimum: got " << sample_count << " expected >= " << minimum;
    add_error(result, ss.str());
  }
  if (audit.contains("manifest") && audit.at("manifest").is_object()) {
    compare_row_shape(result, row, audit.at("manifest"), "sample_audit.manifest");
  } else {
    add_error(result, "sample_audit.manifest missing");
  }
  if (audit.contains("quotas") && audit.at("quotas").is_object() &&
      audit.contains("class_counts") && audit.at("class_counts").is_object()) {
    for (auto it = audit.at("quotas").begin(); it != audit.at("quotas").end(); ++it) {
      const std::string klass = it.key();
      const std::uint64_t quota = it.value().get<std::uint64_t>();
      const auto got = maybe_u64(audit.at("class_counts"), klass).value_or(0);
      const bool exhausted = class_exhausted(klass);
      if (got < quota && !exhausted) {
        add_error(result, "sample class below quota without exhaustion: " + klass);
      }
    }
  } else {
    add_warning(result, "sample_audit quotas/class_counts are incomplete in this skeleton bundle");
  }
  result.detail["sample_audit_checked"] = true;
  return result.errors.empty();
}

Point parse_point(const nlohmann::json& item) {
  return Point{item.at("a").get<std::int64_t>(), item.at("b").get<std::int64_t>()};
}

bool check_span_certificate(CheckResult& result,
                            const Row& row,
                            const nlohmann::json& bundle,
                            const nlohmann::json& profile,
                            std::string& cert_path) {
  if (!bundle.contains("span_certificate") || bundle.at("span_certificate").is_null()) {
    return false;
  }
  const auto& cert = bundle.at("span_certificate");
  if (!cert.is_object()) {
    add_error(result, "span_certificate must be object");
    return false;
  }
  if (cert.contains("present") && cert.at("present").is_boolean() &&
      !cert.at("present").get<bool>()) {
    return false;
  }
  if (cert.contains("path") && cert.at("path").is_string()) {
    cert_path = cert.at("path").get<std::string>();
  }

  compare_row_shape(result, row, cert, "span_certificate");
  if (row.verdict != "SPANNING") {
    add_error(result, "span_certificate is only valid for SPANNING rows");
    return false;
  }
  const bool has_points = cert.contains("points") && cert.at("points").is_array();
  const bool has_path_points = cert.contains("path_points") && cert.at("path_points").is_array();
  const bool has_path_array = cert.contains("path") && cert.at("path").is_array();
  if (!has_points && !has_path_points && !has_path_array) {
    add_error(result, "span_certificate missing points");
    return false;
  }
  const auto& points_json = has_points ? cert.at("points") :
                            has_path_points ? cert.at("path_points") :
                                              cert.at("path");
  if (!points_json.is_array() || points_json.size() < 2) {
    add_error(result, "span_certificate points must contain at least two points");
    return false;
  }

  const RowConfig row_config{row.k_sq, row.r_inner, row.r_outer};
  std::vector<Point> points;
  for (const auto& item : points_json) points.push_back(parse_point(item));
  for (std::size_t i = 0; i < points.size(); ++i) {
    const Point p = points[i];
    if (p.a < 0 || p.b < p.a) {
      add_error(result, "certificate point outside full-octant ordering at index " +
                            std::to_string(i));
      continue;
    }
    const std::uint64_t n = moat_verify::norm_sq(p);
    if (!moat_verify::in_annulus(p, row_config)) {
      add_error(result, "certificate point outside annulus at index " + std::to_string(i));
    }
    if (!moat_verify::gaussian_prime_point(p)) {
      add_error(result, "certificate point is not Gaussian prime at index " +
                            std::to_string(i));
    }
    if (i != 0 && !moat_verify::within_k(points[i - 1], p, row.k_sq)) {
      add_error(result, "certificate step exceeds K at index " + std::to_string(i));
    }
    (void)n;
  }
  if (!points.empty() && !moat_verify::geo_inner(moat_verify::norm_sq(points.front()), row_config)) {
    add_error(result, "certificate first point is not in geo_I");
  }
  if (!points.empty() && !moat_verify::geo_outer(moat_verify::norm_sq(points.back()), row_config)) {
    add_error(result, "certificate last point is not in geo_O");
  }

  if (cert.contains("binding") && cert.at("binding").is_object()) {
    const auto& binding = cert.at("binding");
    const std::vector<std::pair<std::string, std::optional<std::string>>> checks = {
        {"commit", first_present({
                       maybe_string(profile, "commit"),
                       nested_string(profile, {"build", "commit"}),
                   })},
        {"build_identity", first_present({
                               maybe_string(profile, "build_identity"),
                               nested_string(profile, {"build", "identity"}),
                               nested_string(profile, {"build", "id"}),
                           })},
        {"grid_hash", first_present({
                          nested_string(profile, {"hashes", "grid"}),
                          nested_string(profile, {"hashes", "grid_hash"}),
                      })},
        {"constants_hash", first_present({
                               nested_string(profile, {"hashes", "constants"}),
                               nested_string(profile, {"hashes", "constants_hash"}),
                           })},
        {"mr_witness_hash", first_present({
                                nested_string(profile, {"hashes", "mr_witness"}),
                                nested_string(profile, {"hashes", "mr_witness_hash"}),
                            })},
    };
    for (const auto& [field, expected] : checks) {
      const auto got = maybe_string(binding, field);
      if (got && expected && *got != *expected) {
        add_error(result, "span_certificate.binding." + field + " mismatch");
      }
    }
    const auto command = maybe_command_string(profile, "command");
    const auto cert_command = maybe_command_string(binding, "command");
    if (command && cert_command && *command != *cert_command) {
      add_error(result, "span_certificate.binding.command mismatch");
    }
  } else {
    add_warning(result, "span_certificate.binding missing; skeleton checked coordinates only");
  }

  result.detail["span_certificate_points"] = points.size();
  return result.errors.empty();
}

CheckResult check_bundle(const nlohmann::json& bundle) {
  CheckResult result;
  if (!bundle.is_object()) throw std::runtime_error("bundle root must be object");
  if (require_u64(bundle, "schema_version") != 1) {
    throw std::runtime_error("unsupported schema_version");
  }
  const Row row = parse_row(required_object(bundle, "row"));
  const auto& profile = required_object(bundle, "profile");

  check_row_contract(result, row);
  compare_row_shape(result, row, profile, "profile");
  if (bundle.contains("run_index") && bundle.at("run_index").is_object()) {
    compare_row_shape(result, row, bundle.at("run_index"), "run_index");
  }
  if (bundle.contains("stdout") && bundle.at("stdout").is_object()) {
    compare_row_shape(result, row, bundle.at("stdout"), "stdout");
    compare_string(result, bundle.at("stdout"), "stdout", "command",
                   maybe_command_string(profile, "command").value_or(""));
  }
  check_bz(result, bundle);
  check_overflows(result, bundle);
  check_build_identity(result, profile);
  check_moat_full_ingest(result, row, profile);
  check_active_tile_count(result, bundle, profile);
  check_stats_v2(result, bundle, "");
  check_stats_v2(result, profile, "profile");
  check_artifact_ledgers(result, bundle, profile);

  bool sample_audit_pass = false;
  try {
    sample_audit_pass = check_sample_audit(result, row, bundle);
  } catch (const std::exception& e) {
    add_error(result, std::string("sample audit invalid: ") + e.what());
  }

  std::string cert_path;
  bool span_proof_pass = false;
  try {
    span_proof_pass = check_span_certificate(result, row, bundle, profile, cert_path);
  } catch (const std::exception& e) {
    add_error(result, std::string("span certificate invalid: ") + e.what());
  }

  const bool require_any_artifact = row.row_class == "accepted" || row.row_class == "profile" ||
                                    sample_audit_pass || span_proof_pass;
  const bool saw_cert_artifact = check_artifacts(result, bundle, require_any_artifact, cert_path);
  if (span_proof_pass && !saw_cert_artifact) {
    add_error(result, "span certificate artifact missing from artifact table");
  }

  result.detail["claim_id"] = row.claim_id;
  result.detail["verdict"] = row.verdict;
  result.detail["row_class"] = row.row_class;
  result.detail["telemetry_level"] = row.telemetry_level;
  result.detail["run_contract"] = result.errors.empty() ? "PASS" : "FAIL";
  result.detail["sample_audit"] = sample_audit_pass ? "PASS" : "NOT_PRESENT";
  result.detail["span_proof"] = span_proof_pass ? "PASS" : "NOT_PRESENT";

  if (!result.errors.empty()) {
    result.status = Status::Reject;
  } else if (span_proof_pass) {
    result.status = Status::SpanProofPass;
  } else if (row.claim_proof_required) {
    result.status = Status::ClaimProofMissing;
  } else if (sample_audit_pass) {
    result.status = Status::TileSampleAuditPass;
  } else {
    result.status = Status::RunContractPass;
  }
  return result;
}

nlohmann::json make_report(const nlohmann::json& bundle, const CheckResult& result) {
  nlohmann::json report;
  report["schema_version"] = 1;
  report["status"] = status_name(result.status);
  report["status_vocabulary"] = {
      "REJECT",
      "RUN_CONTRACT_PASS",
      "TILE_SAMPLE_AUDIT_PASS",
      "SPAN_PROOF_PASS",
      "MOAT_PROOF_PASS",
      "CLAIM_PROOF_MISSING",
  };
  if (bundle.contains("bundle_id")) report["bundle_id"] = bundle.at("bundle_id");
  report["detail"] = result.detail;
  report["errors"] = result.errors;
  report["warnings"] = result.warnings;
  return report;
}

nlohmann::json read_json_file(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) throw std::runtime_error("could not open input: " + path);
  nlohmann::json j;
  in >> j;
  return j;
}

void write_json_file(const std::string& path, const nlohmann::json& report) {
  std::ofstream out(path);
  if (!out.is_open()) throw std::runtime_error("could not open report output: " + path);
  out << report.dump(2) << "\n";
}

void usage() {
  std::cerr << "Usage: postflight_check BUNDLE.json [--report OUT.json]\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_path;
  std::string report_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--report" && i + 1 < argc) {
      report_path = argv[++i];
    } else if (arg.rfind("--report=", 0) == 0) {
      report_path = arg.substr(9);
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else if (input_path.empty()) {
      input_path = arg;
    } else {
      usage();
      return 2;
    }
  }
  if (input_path.empty()) {
    usage();
    return 2;
  }

  try {
    const nlohmann::json bundle = read_json_file(input_path);
    const CheckResult result = check_bundle(bundle);
    const nlohmann::json report = make_report(bundle, result);
    if (!report_path.empty()) write_json_file(report_path, report);
    std::cout << report.dump(2) << "\n";
    return result.status == Status::Reject ? 1 : 0;
  } catch (const std::exception& e) {
    nlohmann::json report;
    report["schema_version"] = 1;
    report["status"] = "REJECT";
    report["errors"] = {std::string("fatal: ") + e.what()};
    std::cerr << report.dump(2) << "\n";
    return 2;
  }
}
