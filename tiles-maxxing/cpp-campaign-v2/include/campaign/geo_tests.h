// include/campaign/geo_tests.h
//
// Spec norm-form inner / outer geo tests for the cpp-campaign-v2 reference build.
//
// Given a prime with squared norm `norm_sq`, decide whether it lies in
// `geo_I` (the canonical inner-boundary prime set) or `geo_O` (the outer
// set). Spec predicate (tile-operator-definition-v-claude.md:314-325):
//
//   inner: (norm_sq - R_inner^2 - K)^2 <= 4 * R_inner^2 * K
//   outer: (R_outer^2 - norm_sq + K)^2 <= 4 * R_outer^2 * K   (equivalently
//                                         (norm_sq - R_outer^2 + K)^2 <= ...)
//
// Audit Codex-M1 (2026-04-21): previous implementation used a widened
// ceil_isqrt(K) band as the primary test; at K=36 the two are identical,
// but at K=40 the band accepted primes the spec rejects. The ceil_isqrt(K)
// prefilter is retained inside the implementation as a short-circuit.
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
