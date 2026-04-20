// include/campaign/geo_tests.h
//
// Two-stage norm-form inner / outer geo tests for the cpp-campaign-v2
// reference build.
//
// Given a prime with squared norm `norm_sq`, decide whether it lies in
// `geo_I` (the canonical norm-form inner-boundary prime set) or `geo_O`
// (the outer set). Blueprint §2 "Integer-overflow pre-filter" supplies
// the two-stage integer algorithm:
//
//   int64 eps = norm_sq - R_sq - K;
//   if (llabs(eps) > prefilter_bound) return false;
//   return ((__int128)eps * eps) <= four_r_sq_k;
//
// with `prefilter_bound = 2 * R * ceil_isqrt(K) + 1`.
//
// CRITICAL: prefilter_bound uses CEIL_ISQRT(K), NOT FLOOR. For non-square
// K (e.g. K = 40, √K ≈ 6.32), the tighter floor bound 2*R*6 + 1 would
// reject primes with |eps| ∈ (2R·6, 2R·√K] that actually satisfy the
// canonical test — a false negative that leaves a geo_I prime's UF
// component unflagged and yields a false MOAT (UNSOUND).
//
// Dependencies: campaign_constants.h.

#pragma once

#include <cstdint>

#include "campaign/campaign_constants.h"

namespace campaign {

// Return true iff the Gaussian prime with squared norm `norm_sq` is in
// the canonical inner geo set geo_I.
//
// Two-stage:
//   1) Pre-filter on int64 epsilon (cheap, cuts ≥ 99.9% of primes).
//   2) i128 squared-epsilon bound.
//
// STUB in Phase 1; full impl in M3.
bool is_inner_prime(std::int64_t norm_sq,
                    const CampaignConstants& constants) noexcept;

// Return true iff the Gaussian prime with squared norm `norm_sq` is in
// the canonical outer geo set geo_O.
//
// STUB in Phase 1; full impl in M3.
bool is_outer_prime(std::int64_t norm_sq,
                    const CampaignConstants& constants) noexcept;

}  // namespace campaign
