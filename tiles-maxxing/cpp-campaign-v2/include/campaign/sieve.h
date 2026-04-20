// include/campaign/sieve.h
//
// Halo Gaussian-prime sieve for the cpp-campaign-v2 reference build.
//
// For a given TileCoord, enumerates all Gaussian primes whose coordinates
// lie within the halo-expanded region
// [a_lo - C, a_lo + S + C] × [b_lo - C, b_lo + S + C]
// intersected with the canonical octant R and the annulus
// R_inner² ≤ a² + b² ≤ R_outer².
//
// Includes axis primes at x = 0 via the inert-prime residue class (Option
// B, blueprint §4.1 + M2 acceptance gate (c) — a prime (0, q) lies on the
// y-axis iff q ≡ 3 (mod 4) and q is rational prime).
//
// Output contract (for Phase 2 workers):
//
//   sieve_tile(coord, constants) -> std::vector<Prime>
//
//   Prime order is wire-stable: lex (a, b). Tests rely on this for
//   reproducibility.
//
// Implementation forbidden patterns (blueprint §6.3, plan §9 item 1.5):
//   * No std::sqrt / sqrtf / sqrtl anywhere in the TU.
//   * #pragma GCC poison sqrt sqrtf sqrtl at file top of src/sieve.cpp
//     (injected via CAMPAIGN_STRICT compile flag).
//   * Integer arithmetic only; i128 for R² at project scale.
//
// Dependencies: campaign_constants.h, grid.h (TileCoord).

#pragma once

#include <cstdint>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/grid.h"

namespace campaign {

#if defined(CAMPAIGN_STRICT) && defined(__GNUC__)
// Poison floating-point sqrt in every TU that includes this header. Test
// generators (which may legitimately use gmpy2 / mpmath) should NOT
// include this header — they live in scripts/ and Python.
#pragma GCC poison sqrt sqrtf sqrtl
#endif

// Single Gaussian prime in the halo-expanded region.
//
// `a, b` are world-space coordinates (integer). `norm_sq = a*a + b*b`
// pre-computed at sieve time; downstream consumers (UF union predicate,
// geo tests) read this directly without re-squaring.
//
// `packed_pos` is the packed row/col position inside the halo-expanded
// SIDE_EXP × SIDE_EXP grid: packed_pos = row * SIDE_EXP + col where
// col = a - (a_lo - C), row = b - (b_lo - C). Matches the CUDA K3 compact
// output convention (blueprint §6.2).
struct Prime {
  std::int64_t a = 0;
  std::int64_t b = 0;
  std::uint64_t norm_sq = 0;
  std::uint32_t packed_pos = 0;
};

// Enumerate Gaussian primes in the halo-expanded region of `coord`.
//
// Preconditions:
//   * constants.K_SQ_value matches the compile-time K_SQ.
//   * coord.a_lo == (o_x + coord.i * S); coord.b_lo == (o_y + coord.j * S).
//
// Returns a vector sorted by lex (a, b). Empty iff the halo misses all
// octant lattice points in the annulus.
//
// Complexity: O(SIDE_EXP²) candidate sweep + O(primes * log R) MR cost.
// Hot path for CPU reference but not for GPU (which will use K1..K3).
//
// STUB in Phase 1: implementation deferred to Phase 2 M2. The header
// fixes the API signature.
std::vector<Prime> sieve_tile(const TileCoord& coord,
                              const CampaignConstants& constants);

// Miller-Rabin primality test for u64 using `kMillerRabinWitnesses`.
//
// Deterministic across the full 64-bit range with the pinned witness set.
// Used by `sieve_tile` and by the axis-prime y-axis path. Exposed for
// testing.
//
// STUB in Phase 1.
bool is_prime_u64(std::uint64_t n);

// Return true iff n is a Gaussian-prime norm, i.e. either
//   * n == 2, or
//   * n is a rational prime with n ≡ 1 (mod 4), or
//   * n = q² where q is a rational prime with q ≡ 3 (mod 4).
// Used for axis-prime detection (q on the y-axis has norm q²).
//
// STUB in Phase 1.
bool is_gaussian_prime_norm(std::uint64_t n);

}  // namespace campaign
