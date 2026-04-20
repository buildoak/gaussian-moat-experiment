// src/sieve.cpp
//
// STUB — Phase 2 M2 implements.
//
// Forbidden patterns (plan §9 item 1.5): this TU must NEVER call
// std::sqrt / sqrtf / sqrtl. The `#pragma GCC poison` in sieve.h
// enforces this when CAMPAIGN_STRICT is on; we double-guard here.

#include "campaign/sieve.h"

#include <cstdint>
#include <vector>

namespace campaign {

std::vector<Prime> sieve_tile(const TileCoord& /*coord*/,
                              const CampaignConstants& /*constants*/) {
  return {};  // Phase 1 stub
}

bool is_prime_u64(std::uint64_t /*n*/) {
  return false;  // Phase 1 stub
}

bool is_gaussian_prime_norm(std::uint64_t /*n*/) {
  return false;  // Phase 1 stub
}

}  // namespace campaign
