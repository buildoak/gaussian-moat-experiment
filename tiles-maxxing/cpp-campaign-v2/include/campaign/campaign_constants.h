// include/campaign/campaign_constants.h
//
// Runtime campaign parameters and derived i128 squares for the
// cpp-campaign-v2 reference build.
//
// `CampaignConstants` bundles every derived value the per-tile hot path
// needs from the CLI-supplied (R_inner, R_outer, K_SQ) triple. It is
// computed once at campaign init via `CampaignConstants::from_radii(...)`
// and held immutable thereafter.
//
// All derivations are integer (i64 or i128). No floating-point anywhere.
//
// Dependencies: constants.h, <cstdint>.

#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

#include "campaign/constants.h"

namespace campaign {

// SHA-256 over the raw bytes of the pinned FJ64 262144-entry uint16_t witness
// table. This lands in the snapshot header so the future CUDA port cannot
// silently drift to a different primality table (blueprint §11, R10).
inline constexpr const char* kFj64WitnessTableSha256 =
    "92b8b0ea7ae8703a3fae4f7a1581dd0d04e041bde4eb1d23621a8f39846e909c";

// ---------------------------------------------------------------------------
// CampaignConstants struct
// ---------------------------------------------------------------------------

// Runtime campaign parameter bundle.
//
// Fields marked "u64" are native integers. Fields marked "i128" are carried
// as hi/lo u64 pairs so this struct stays standard-layout and directly
// memcpy-able into a future `__constant__` GPU upload (blueprint §4.6).
//
// Invariants (asserted by `from_radii`):
//   * R_inner_sq == R_inner * R_inner (as i128-capable product)
//   * R_outer_sq == R_outer * R_outer
//   * R_outer    >  R_inner
//   * K_SQ       >  0
//   * four_rin_sq_k  == 4 * R_inner_sq * K_SQ  (i128)
//   * four_rout_sq_k == 4 * R_outer_sq * K_SQ  (i128)
//   * prefilter_inner == 2 * R_inner * ceil_isqrt(K_SQ) + 1   (ceil, NOT floor)
//   * prefilter_outer == 2 * R_outer * ceil_isqrt(K_SQ) + 1   (ceil, NOT floor)
//   * R_inner > floor_isqrt(2 * K_SQ)   (Adjacent Octants Lemma, blueprint §2)
//   * R_outer - R_inner > S*floor_isqrt(2) + 2*ceil_isqrt(K_SQ)
//       approximated conservatively at integer level
//       (annulus-thickness assertion, blueprint §2)
struct CampaignConstants {
  // Core radii
  std::uint64_t R_inner = 0;
  std::uint64_t R_outer = 0;

  // Squared radii (i64 wide enough up to R ≤ 3e9 — safe for project R ~ 1e8)
  std::uint64_t R_inner_sq = 0;
  std::uint64_t R_outer_sq = 0;

  // Pre-filter bounds for norm-form geo tests (blueprint §2).
  // USES CEIL_ISQRT(K_SQ), NOT FLOOR. Floor is the canonical boundary bug.
  std::uint64_t prefilter_inner = 0;
  std::uint64_t prefilter_outer = 0;

  // i128 = 4 * R^2 * K_SQ, stored as hi/lo u64 pair for standard-layout-friendly
  // byte-level determinism. Reassembled via `four_rin_sq_k_i128()`.
  std::uint64_t four_rin_sq_k_hi = 0;
  std::uint64_t four_rin_sq_k_lo = 0;
  std::uint64_t four_rout_sq_k_hi = 0;
  std::uint64_t four_rout_sq_k_lo = 0;

  // Compile-time-reflected scalars (carried in the struct for CUDA upload
  // convenience).
  std::uint32_t K_SQ_value = 0;  // equals constants.h K_SQ
  std::uint32_t S_value = 0;     // equals constants.h S
  std::uint32_t C_value = 0;     // equals constants.h C
  std::uint32_t o_x = 0;         // equals constants.h OFFSET_X
  std::uint32_t o_y = 0;         // equals constants.h OFFSET_Y

  // -------------------------------------------------------------------------
  // Factories
  // -------------------------------------------------------------------------

  // Build a `CampaignConstants` from CLI radii and the compiled K_SQ.
  //
  // Preconditions:
  //   R_inner > 0, R_outer > R_inner, K_SQ > 0.
  //   (R_outer ^ 2) fits in u64  — caller responsibility (R ≤ 4e9).
  //
  // If `strict` is true, the annulus-thickness soundness precondition
  //   (R_outer - R_inner) > S*sqrt(2) + 2*sqrt(K)
  // is enforced at construction time (library-level gate per audit rec
  // (3)). If `strict` is false (default), the check is skipped so tiny-
  // radius unit tests can still construct instances — the same gate is
  // still enforced by `campaign_main` before emitting a verdict. Any new
  // entry point that produces a real verdict SHOULD use strict=true.
  //
  // Throws std::invalid_argument on any invariant violation. The throw
  // surface is small — campaign_main validates CLI before calling.
  static CampaignConstants from_radii(std::uint64_t R_inner,
                                      std::uint64_t R_outer,
                                      std::uint32_t K_SQ_arg,
                                      bool strict = false);

  // -------------------------------------------------------------------------
  // Accessors for i128 constants
  // -------------------------------------------------------------------------

  // Reassemble i128 = 4 * R_inner^2 * K_SQ. Used by the norm-form inner test.
  __int128 four_rin_sq_k_i128() const noexcept;

  // Reassemble i128 = 4 * R_outer^2 * K_SQ. Used by the norm-form outer test.
  __int128 four_rout_sq_k_i128() const noexcept;

  // -------------------------------------------------------------------------
  // Hash / fingerprinting
  // -------------------------------------------------------------------------

  // Canonical logical-field fingerprint (plan §3 + Q4 resolution).
  //
  // Serializes logical fields as UTF-8 bytes in the format
  //     "K=<K_SQ>;R_inner=<R_inner>;R_outer=<R_outer>;offset=<o_x>,<o_y>;collar=<C>"
  // and returns the SHA-256 hex digest.
  //
  // Stable across compiler, toolchain, struct padding, and field reorder —
  // this is what the snapshot header carries for CUDA-port parity.
  std::string canonical_hash() const;

  // SHA-256 hex digest of the raw bytes of the pinned FJ64 witness table.
  // Embedded in the snapshot header (plan M5 brief). Matching this hash is
  // a parity gate for the future CUDA port.
  static std::string mr_witness_set_sha256();

  // -------------------------------------------------------------------------
  // Soundness preconditions (optional, not enforced by from_radii)
  // -------------------------------------------------------------------------

  // Check the annulus thickness precondition for Theorem 11 Case A.2
  // (blueprint §2): R_outer - R_inner > S*sqrt(2) + 2*sqrt(K).
  //
  // This is a soundness gate for the *campaign verdict*, not a structural
  // precondition for Grid / TileOp machinery. `from_radii` does NOT
  // enforce it so tiny-radius unit tests can exercise the primitives.
  //
  // campaign_main calls this at production init and refuses to emit a
  // verdict when false.
  bool verify_annulus_thickness() const noexcept;
};

// Sanity: we rely on standard-layout-ness for byte-identical hashing of
// debug dumps (separate from the canonical hash above).
static_assert(std::is_standard_layout<CampaignConstants>::value,
              "CampaignConstants must be standard-layout");

}  // namespace campaign
