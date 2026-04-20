// include/campaign/geo_tests.h
//
// Band-based inner / outer geo tests for the cpp-campaign-v2 reference build.
//
// Given a prime with squared norm `norm_sq`, decide whether it lies in
// `geo_I` (the canonical inner-boundary prime set) or `geo_O` (the outer
// set). Model A semantics are the integer bands:
//
//   inner: R_inner^2 <= norm_sq <= (R_inner + ceil_isqrt(K))^2
//   outer: (R_outer - ceil_isqrt(K))^2 <= norm_sq <= R_outer^2
//
// CRITICAL: the band width uses CEIL_ISQRT(K), not the collar C. For
// non-square K (e.g. K = 40), C = 6 but the candidate band width is 7.
//
// Dependencies: campaign_constants.h.

#pragma once

#include <cstdint>

#include "campaign/campaign_constants.h"

namespace campaign {

// Return true iff the Gaussian prime with squared norm `norm_sq` is in the
// canonical inner geo set geo_I.
bool is_inner_prime(std::int64_t norm_sq,
                    const CampaignConstants& constants) noexcept;

// Return true iff the Gaussian prime with squared norm `norm_sq` is in the
// canonical outer geo set geo_O.
bool is_outer_prime(std::int64_t norm_sq,
                    const CampaignConstants& constants) noexcept;

}  // namespace campaign
