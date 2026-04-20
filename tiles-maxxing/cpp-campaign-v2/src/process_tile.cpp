// src/process_tile.cpp
//
// Per-tile orchestrator: sieve -> geo flags -> TileOp encoder.

#include "campaign/tileop.h"

#include <cstdint>
#include <vector>

#include "campaign/geo_tests.h"
#include "tileop_internal.h"

namespace campaign {

TileOp process_tile(const TileCoord& coord,
                    const CampaignConstants& constants,
                    const Grid& /*grid*/) {
  std::vector<Prime> primes = sieve_tile(coord, constants);
  std::vector<internal::PrimeGeoFlags> prime_flags;
  prime_flags.reserve(primes.size());

  for (const Prime& prime : primes) {
    const auto norm_sq = static_cast<std::int64_t>(prime.norm_sq);
    prime_flags.push_back(internal::PrimeGeoFlags{
        is_inner_prime(norm_sq, constants),
        is_outer_prime(norm_sq, constants),
    });
  }

  return internal::build_tileop_for_primes(std::move(primes),
                                           std::move(prime_flags), coord,
                                           constants);
}

void process_tile(const TileCoord& coord,
                  const Grid& grid,
                  const CampaignConstants& constants,
                  TileOp* out) {
  *out = process_tile(coord, constants, grid);
}

}  // namespace campaign
