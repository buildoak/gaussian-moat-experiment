// src/union_find.cpp
//
// Deterministic smaller-root-wins DSU over uint16_t parents.

#include "campaign/union_find.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace campaign {

static_assert(kMaxDsuSize ==
                  static_cast<std::int32_t>(
                      std::numeric_limits<std::uint16_t>::max()) +
                      1,
              "kMaxDsuSize must equal UINT16_MAX+1 — DSU parents are "
              "stored as uint16_t; widening this cap is a deliberate "
              "design change, not an accidental bump.");

DSU::DSU(std::int32_t n) : parent_() {
  if (n < 0 || n >= kMaxDsuSize) {
    std::ostringstream msg;
    msg << "DSU size " << n
        << " outside [0, " << kMaxDsuSize
        << "): DSU parents are stored as uint16_t so the hard cap is "
        << kMaxDsuSize
        << ". Widening requires changing parent_ to a wider integer.";
    throw std::invalid_argument(msg.str());
  }

  parent_.resize(static_cast<std::size_t>(n));
  for (std::int32_t i = 0; i < n; ++i) {
    parent_[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(i);
  }
}

std::int32_t DSU::find(std::int32_t x) {
  assert(x >= 0);
  assert(x < size());

  auto p = static_cast<std::uint16_t>(x);
  while (p != parent_[p]) {
    parent_[p] = parent_[parent_[p]];
    p = parent_[p];
  }
  return static_cast<std::int32_t>(p);
}

std::int32_t DSU::unite(std::int32_t a, std::int32_t b) {
  std::int32_t ra = find(a);
  std::int32_t rb = find(b);
  if (ra == rb) {
    return ra;
  }
  if (ra > rb) {
    std::swap(ra, rb);
  }
  parent_[static_cast<std::size_t>(rb)] = static_cast<std::uint16_t>(ra);
  return ra;
}

std::int32_t DSU::size() const noexcept {
  return static_cast<std::int32_t>(parent_.size());
}

std::vector<std::int32_t> DSU::roots() const {
  std::vector<std::int32_t> out;
  out.reserve(parent_.size());

  for (std::int32_t i = 0; i < size(); ++i) {
    const std::int32_t root = const_cast<DSU*>(this)->find(i);
    if (std::find(out.begin(), out.end(), root) == out.end()) {
      out.push_back(root);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace campaign
