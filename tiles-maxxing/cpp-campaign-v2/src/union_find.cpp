// src/union_find.cpp
//
// STUB (except constructor) — Phase 2 M3 implements full logic.

#include "campaign/union_find.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace campaign {

DSU::DSU(std::int32_t n) : parent_(static_cast<std::size_t>(n), 0) {
  // Identity parent: parent[i] = i
  for (std::int32_t i = 0; i < n; ++i) {
    parent_[static_cast<std::size_t>(i)] = i;
  }
}

std::int32_t DSU::find(std::int32_t /*x*/) {
  return 0;  // Phase 1 stub
}

std::int32_t DSU::unite(std::int32_t /*a*/, std::int32_t /*b*/) {
  return 0;  // Phase 1 stub
}

std::int32_t DSU::size() const noexcept {
  return static_cast<std::int32_t>(parent_.size());
}

std::vector<std::int32_t> DSU::roots() const {
  return {};  // Phase 1 stub
}

}  // namespace campaign
