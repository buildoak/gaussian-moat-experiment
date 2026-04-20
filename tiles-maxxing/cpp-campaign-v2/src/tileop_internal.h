// src/tileop_internal.h
//
// Internal TileOp pipeline helpers. Tests include this header directly so
// synthetic prime sets can exercise the encoder without invoking the sieve.

#pragma once

#include <cstdint>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/grid.h"
#include "campaign/sieve.h"
#include "campaign/tileop.h"
#include "campaign/union_find.h"

namespace campaign::internal {

struct PrimeGeoFlags {
  bool inner = false;
  bool outer = false;
};

struct DenseRemap {
  std::vector<std::int32_t> zero_based_by_raw_root;
  std::vector<std::uint8_t> wire_label_by_raw_root;
  std::int32_t max_label = 0;
  bool overflow = false;
};

DSU build_local_dsu(const std::vector<Prime>& primes);

DenseRemap dense_remap_roots(DSU* dsu, std::int32_t prime_count);

DenseRemap dense_remap_raw_roots_for_test(const std::vector<std::int32_t>& raw_roots,
                                          std::int32_t raw_root_bound);

TileOp build_tileop_for_primes(std::vector<Prime> primes,
                               std::vector<PrimeGeoFlags> prime_flags,
                               const TileCoord& coord,
                               const CampaignConstants& constants);

}  // namespace campaign::internal
