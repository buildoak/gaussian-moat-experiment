#include "independent_moat.hpp"

#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>

#include <nlohmann/json.hpp>

using namespace moat_verify;

namespace {

constexpr int kSpanCertSchemaVersion = 1;
constexpr std::uint64_t kMaxSquaredRadius =
    std::numeric_limits<std::uint64_t>::max();

std::string type_name(const nlohmann::json& value) {
  return std::string(value.type_name());
}

const nlohmann::json& require_field(const nlohmann::json& object,
                                    const char* field) {
  const auto it = object.find(field);
  if (it == object.end()) {
    throw std::runtime_error(std::string("missing required field: ") + field);
  }
  return *it;
}

void reject_unknown_fields(const nlohmann::json& object,
                           const std::set<std::string>& allowed,
                           const std::string& context) {
  for (const auto& item : object.items()) {
    if (!allowed.contains(item.key())) {
      throw std::runtime_error(context + " has unknown field: " + item.key());
    }
  }
}

std::uint64_t require_u64(const nlohmann::json& object, const char* field) {
  const nlohmann::json& value = require_field(object, field);
  if (!value.is_number_unsigned()) {
    throw std::runtime_error(std::string("field ") + field +
                             " must be an unsigned integer, got " +
                             type_name(value));
  }
  return value.get<std::uint64_t>();
}

std::uint32_t require_u32(const nlohmann::json& object, const char* field) {
  const std::uint64_t value = require_u64(object, field);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(std::string("field ") + field +
                             " exceeds uint32 range");
  }
  return static_cast<std::uint32_t>(value);
}

std::int64_t require_nonnegative_i64(const nlohmann::json& object,
                                     const char* field) {
  const nlohmann::json& value = require_field(object, field);
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string("point field ") + field +
                             " must be an integer, got " + type_name(value));
  }
  if (value.is_number_unsigned()) {
    const std::uint64_t u = value.get<std::uint64_t>();
    if (u > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
      throw std::runtime_error(std::string("point field ") + field +
                               " exceeds int64 range");
    }
    return static_cast<std::int64_t>(u);
  }
  const std::int64_t i = value.get<std::int64_t>();
  if (i < 0) {
    throw std::runtime_error(std::string("point field ") + field +
                             " must be nonnegative");
  }
  return i;
}

std::string require_string(const nlohmann::json& object, const char* field) {
  const nlohmann::json& value = require_field(object, field);
  if (!value.is_string()) {
    throw std::runtime_error(std::string("field ") + field +
                             " must be a string, got " + type_name(value));
  }
  return value.get<std::string>();
}

bool is_sane_token_string(const std::string& value, std::size_t max_len) {
  if (value.empty() || value.size() > max_len) return false;
  for (const unsigned char ch : value) {
    if (std::iscntrl(ch) != 0 || std::isspace(ch) != 0) return false;
  }
  return true;
}

bool is_sane_text_string(const std::string& value, std::size_t max_len) {
  if (value.empty() || value.size() > max_len) return false;
  for (const unsigned char ch : value) {
    if (std::iscntrl(ch) != 0) return false;
  }
  return true;
}

void validate_optional_token_string(const nlohmann::json& cert,
                                    const char* field,
                                    std::size_t max_len) {
  if (!cert.contains(field)) return;
  const std::string value = require_string(cert, field);
  if (!is_sane_token_string(value, max_len)) {
    throw std::runtime_error(std::string("field ") + field +
                             " must be nonempty, whitespace-free, and at most " +
                             std::to_string(max_len) + " bytes");
  }
}

void validate_optional_text_string(const nlohmann::json& cert,
                                   const char* field,
                                   std::size_t max_len) {
  if (!cert.contains(field)) return;
  const std::string value = require_string(cert, field);
  if (!is_sane_text_string(value, max_len)) {
    throw std::runtime_error(std::string("field ") + field +
                             " must be nonempty, control-free, and at most " +
                             std::to_string(max_len) + " bytes");
  }
}

std::uint64_t square_u64_checked(std::uint64_t value,
                                 const std::string& field) {
  const unsigned __int128 square = u128(value) * u128(value);
  if (square > u128(kMaxSquaredRadius)) {
    throw std::runtime_error(field + " squared exceeds uint64 range");
  }
  return static_cast<std::uint64_t>(square);
}

void validate_schema_and_metadata(const nlohmann::json& cert,
                                  RowConfig& row,
                                  std::uint64_t& r_inner_sq,
                                  std::uint64_t& r_outer_sq) {
  if (!cert.is_object()) {
    throw std::runtime_error("certificate root must be a JSON object");
  }
  reject_unknown_fields(cert,
                        {"schema_version", "claim_id", "k_sq", "r_inner",
                         "r_outer", "region", "source", "path"},
                        "certificate");

  const std::uint32_t schema_version = require_u32(cert, "schema_version");
  if (schema_version != kSpanCertSchemaVersion) {
    throw std::runtime_error("schema_version must be 1");
  }

  const std::string region = require_string(cert, "region");
  if (region != "full-octant") {
    throw std::runtime_error("region must be full-octant");
  }

  validate_optional_token_string(cert, "claim_id", 128);
  validate_optional_text_string(cert, "source", 256);

  row.k_sq = require_u32(cert, "k_sq");
  row.r_inner = require_u64(cert, "r_inner");
  row.r_outer = require_u64(cert, "r_outer");
  if (row.k_sq == 0) {
    throw std::runtime_error("k_sq must be positive");
  }
  if (row.r_inner == 0) {
    throw std::runtime_error("r_inner must be positive");
  }
  if (row.r_outer <= row.r_inner) {
    throw std::runtime_error("r_outer must be greater than r_inner");
  }
  r_inner_sq = square_u64_checked(row.r_inner, "r_inner");
  r_outer_sq = square_u64_checked(row.r_outer, "r_outer");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: span_cert_check CERT.json\n";
    return 2;
  }
  try {
    std::ifstream in(argv[1]);
    nlohmann::json cert;
    in >> cert;
    RowConfig row{};
    std::uint64_t r_inner_sq = 0;
    std::uint64_t r_outer_sq = 0;
    validate_schema_and_metadata(cert, row, r_inner_sq, r_outer_sq);

    const auto points_json = cert.at("path");
    if (!points_json.is_array() || points_json.size() < 2) {
      throw std::runtime_error("path must contain at least two points");
    }
    std::vector<Point> path;
    for (std::size_t i = 0; i < points_json.size(); ++i) {
      const auto& item = points_json[i];
      if (!item.is_object()) {
        throw std::runtime_error("path point at index " + std::to_string(i) +
                                 " must be an object");
      }
      reject_unknown_fields(item, {"a", "b"},
                            "path point at index " + std::to_string(i));
      path.push_back(Point{require_nonnegative_i64(item, "a"),
                           require_nonnegative_i64(item, "b")});
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
      const Point p = path[i];
      if (p.a < 0 || p.b < p.a) {
        throw std::runtime_error("point outside full-octant ordering at index " +
                                 std::to_string(i));
      }
      const std::uint64_t n = norm_sq(p);
      if (n < r_inner_sq || n > r_outer_sq) {
        throw std::runtime_error("point outside annulus at index " +
                                 std::to_string(i));
      }
      if (!gaussian_prime_point(p)) {
        throw std::runtime_error("point is not a Gaussian prime at index " +
                                 std::to_string(i));
      }
      if (i != 0 && !within_k(path[i - 1], p, row.k_sq)) {
        throw std::runtime_error("step exceeds K at index " + std::to_string(i));
      }
    }
    if (!geo_inner(norm_sq(path.front()), row)) {
      throw std::runtime_error("first point is not in geo_I");
    }
    if (!geo_outer(norm_sq(path.back()), row)) {
      throw std::runtime_error("last point is not in geo_O");
    }
    std::cout << "span certificate PASS: points=" << path.size() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
