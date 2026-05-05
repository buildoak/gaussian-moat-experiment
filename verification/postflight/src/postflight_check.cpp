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

bool check_artifacts(CheckResult& result,
                     const nlohmann::json& bundle,
                     bool require_any_artifact,
                     const std::string& cert_path) {
  const nlohmann::json* artifacts = artifact_table(bundle);
  if (artifacts == nullptr) {
    if (require_any_artifact) add_error(result, "missing artifact table");
    return false;
  }

  std::vector<nlohmann::json> entries;
  if (artifacts->is_array()) {
    for (const auto& item : *artifacts) entries.push_back(item);
  } else if (artifacts->is_object()) {
    for (auto it = artifacts->begin(); it != artifacts->end(); ++it) {
      nlohmann::json item = it.value();
      if (item.is_object() && !item.contains("name")) item["name"] = it.key();
      entries.push_back(item);
    }
  } else {
    add_error(result, "artifact table must be array or object");
    return false;
  }

  if (entries.empty() && require_any_artifact) add_error(result, "artifact table is empty");

  bool saw_cert = false;
  for (const auto& artifact : entries) {
    if (!artifact.is_object()) {
      add_error(result, "artifact entry must be object");
      continue;
    }
    if (!maybe_string(artifact, "name") && !maybe_string(artifact, "path")) {
      add_error(result, "artifact entry missing name/path");
    }
    const auto sha = maybe_string(artifact, "sha256");
    if (!sha || !valid_sha256(*sha)) add_error(result, "artifact entry missing valid sha256");
    if (!maybe_u64(artifact, "size_bytes")) add_error(result, "artifact entry missing size_bytes");
    if (!maybe_u64(artifact, "schema_version")) {
      add_error(result, "artifact entry missing schema_version");
    }
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
  const std::uint64_t sample_count = require_u64(audit, "sample_count");
  const std::uint64_t minimum = row.row_class == "accepted" ? 4096 : 1024;
  if (sample_count < minimum) {
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
      const bool exhausted = audit.contains("population_exhausted") &&
                             audit.at("population_exhausted").is_object() &&
                             audit.at("population_exhausted").value(klass, false);
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
  if (const auto path = maybe_string(cert, "path")) cert_path = *path;

  compare_row_shape(result, row, cert, "span_certificate");
  if (row.verdict != "SPANNING") {
    add_error(result, "span_certificate is only valid for SPANNING rows");
    return false;
  }
  if (!cert.contains("points") && !cert.contains("path_points")) {
    add_error(result, "span_certificate missing points");
    return false;
  }
  const auto& points_json = cert.contains("points") ? cert.at("points") : cert.at("path_points");
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
    const auto command = maybe_string(profile, "command");
    const auto cert_command = maybe_string(binding, "command");
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
                   maybe_string(profile, "command").value_or(""));
  }
  check_bz(result, bundle);
  check_overflows(result, bundle);
  check_build_identity(result, profile);
  check_moat_full_ingest(result, row, profile);
  check_active_tile_count(result, bundle, profile);

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
