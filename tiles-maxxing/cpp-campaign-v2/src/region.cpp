// src/region.cpp
//
// Implementation of Region factories and JSON parsing.

#include "campaign/region.h"

#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "campaign/grid.h"

namespace campaign {

Region Region::full_octant(const Grid& grid) {
  Region r{};
  r.is_full_octant = false;  // resolved; caller gets a concrete Region
  r.i_lo = grid.i_min;
  r.i_hi = grid.i_max;

  if (grid.i_max < grid.i_min) {
    // Empty grid. Leave column_ranges empty.
    return r;
  }

  const int n_cols = grid.i_max - grid.i_min + 1;
  r.column_ranges.reserve(static_cast<std::size_t>(n_cols));
  for (int k = 0; k < n_cols; ++k) {
    JRange jr{grid.j_low[static_cast<std::size_t>(k)],
              grid.j_high[static_cast<std::size_t>(k)]};
    r.column_ranges.push_back(jr);
  }
  return r;
}

Region Region::from_tile_box(int i_lo, int i_hi,
                             std::vector<JRange> j_ranges) {
  if (i_hi < i_lo) {
    throw std::invalid_argument(
        "Region::from_tile_box: i_hi must be >= i_lo");
  }
  const int expected = i_hi - i_lo + 1;
  if (static_cast<int>(j_ranges.size()) != expected) {
    std::ostringstream msg;
    msg << "Region::from_tile_box: j_ranges.size() (" << j_ranges.size()
        << ") must equal i_hi - i_lo + 1 (" << expected << ")";
    throw std::invalid_argument(msg.str());
  }
  Region r{};
  r.is_full_octant = false;
  r.i_lo = i_lo;
  r.i_hi = i_hi;
  r.column_ranges = std::move(j_ranges);
  return r;
}

Region Region::from_json_file(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("Region::from_json_file: cannot open " + path);
  }

  nlohmann::json doc;
  try {
    in >> doc;
  } catch (const nlohmann::json::parse_error& e) {
    throw std::invalid_argument(
        std::string("Region::from_json_file: invalid JSON: ") + e.what());
  }

  if (!doc.is_object()) {
    throw std::invalid_argument(
        "Region::from_json_file: top-level JSON must be an object");
  }

  // Form 1: {"full_octant": true}
  if (doc.contains("full_octant") && doc["full_octant"].is_boolean() &&
      doc["full_octant"].get<bool>()) {
    Region r{};
    r.is_full_octant = true;
    // Leave i_lo/i_hi/column_ranges unset; caller resolves via full_octant(grid).
    r.i_lo = 0;
    r.i_hi = -1;
    return r;
  }

  // Form 2: tile-index box with columns array
  if (!doc.contains("i_lo") || !doc["i_lo"].is_number_integer()) {
    throw std::invalid_argument(
        "Region::from_json_file: missing integer 'i_lo'");
  }
  if (!doc.contains("i_hi") || !doc["i_hi"].is_number_integer()) {
    throw std::invalid_argument(
        "Region::from_json_file: missing integer 'i_hi'");
  }
  const int i_lo = doc["i_lo"].get<int>();
  const int i_hi = doc["i_hi"].get<int>();

  if (!doc.contains("columns") || !doc["columns"].is_array()) {
    throw std::invalid_argument(
        "Region::from_json_file: missing array 'columns'");
  }

  if (i_hi < i_lo) {
    throw std::invalid_argument(
        "Region::from_json_file: i_hi must be >= i_lo");
  }

  const int expected = i_hi - i_lo + 1;
  const auto& cols = doc["columns"];
  if (static_cast<int>(cols.size()) != expected) {
    std::ostringstream msg;
    msg << "Region::from_json_file: columns.size() (" << cols.size()
        << ") must equal i_hi - i_lo + 1 (" << expected << ")";
    throw std::invalid_argument(msg.str());
  }

  std::vector<JRange> ranges;
  ranges.reserve(static_cast<std::size_t>(expected));

  // Track which columns we've seen to catch duplicates and gaps.
  std::vector<bool> seen(static_cast<std::size_t>(expected), false);
  std::vector<JRange> by_col(static_cast<std::size_t>(expected));

  for (const auto& entry : cols) {
    if (!entry.is_object()) {
      throw std::invalid_argument(
          "Region::from_json_file: each column entry must be an object");
    }
    if (!entry.contains("i") || !entry["i"].is_number_integer()) {
      throw std::invalid_argument(
          "Region::from_json_file: column entry missing integer 'i'");
    }
    if (!entry.contains("j_lo") || !entry["j_lo"].is_number_integer()) {
      throw std::invalid_argument(
          "Region::from_json_file: column entry missing integer 'j_lo'");
    }
    if (!entry.contains("j_hi") || !entry["j_hi"].is_number_integer()) {
      throw std::invalid_argument(
          "Region::from_json_file: column entry missing integer 'j_hi'");
    }
    const int i = entry["i"].get<int>();
    const int j_lo = entry["j_lo"].get<int>();
    const int j_hi = entry["j_hi"].get<int>();
    if (i < i_lo || i > i_hi) {
      std::ostringstream msg;
      msg << "Region::from_json_file: column i=" << i << " outside [i_lo, i_hi]";
      throw std::invalid_argument(msg.str());
    }
    if (j_hi < j_lo) {
      std::ostringstream msg;
      msg << "Region::from_json_file: column i=" << i
          << " has j_hi (" << j_hi << ") < j_lo (" << j_lo << ")";
      throw std::invalid_argument(msg.str());
    }
    const std::size_t k = static_cast<std::size_t>(i - i_lo);
    if (seen[k]) {
      std::ostringstream msg;
      msg << "Region::from_json_file: duplicate column entry i=" << i;
      throw std::invalid_argument(msg.str());
    }
    seen[k] = true;
    by_col[k] = JRange{j_lo, j_hi};
  }

  for (int k = 0; k < expected; ++k) {
    if (!seen[static_cast<std::size_t>(k)]) {
      std::ostringstream msg;
      msg << "Region::from_json_file: missing column entry for i="
          << (i_lo + k);
      throw std::invalid_argument(msg.str());
    }
  }

  return from_tile_box(i_lo, i_hi, std::move(by_col));
}

JRange Region::column_slice(int i) const {
  if (is_full_octant) {
    throw std::logic_error(
        "Region::column_slice: call full_octant(grid) to resolve first");
  }
  if (i < i_lo || i > i_hi) {
    return JRange{};  // empty
  }
  return column_ranges[static_cast<std::size_t>(i - i_lo)];
}

std::int64_t Region::tile_count() const noexcept {
  if (is_full_octant) return 0;  // caller must resolve first
  std::int64_t total = 0;
  for (const auto& r : column_ranges) {
    total += static_cast<std::int64_t>(r.size());
  }
  return total;
}

}  // namespace campaign
