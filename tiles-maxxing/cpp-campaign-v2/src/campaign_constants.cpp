// src/campaign_constants.cpp
//
// Implementation of CampaignConstants::from_radii and canonical hashing.
// All arithmetic integer or i128; no floating point.

#include "campaign/campaign_constants.h"

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

#include "campaign/fj64_table.h"
#include "sha256.h"

namespace campaign {

namespace {

// Split a positive i128 into (hi, lo) u64 pair. Caller must ensure v >= 0.
inline void split_i128(__int128 v, std::uint64_t& hi, std::uint64_t& lo) noexcept {
  const unsigned __int128 uv = static_cast<unsigned __int128>(v);
  lo = static_cast<std::uint64_t>(uv & ~0ULL);
  hi = static_cast<std::uint64_t>(uv >> 64);
}

// Reassemble an i128 from (hi, lo) u64 pair (non-negative).
inline __int128 assemble_i128(std::uint64_t hi, std::uint64_t lo) noexcept {
  return static_cast<__int128>((static_cast<unsigned __int128>(hi) << 64) |
                               static_cast<unsigned __int128>(lo));
}

}  // namespace

CampaignConstants CampaignConstants::from_radii(std::uint64_t R_inner,
                                                std::uint64_t R_outer,
                                                std::uint32_t K_SQ_arg) {
  // Basic invariant checks.
  if (R_inner == 0) {
    throw std::invalid_argument("R_inner must be > 0");
  }
  if (R_outer <= R_inner) {
    throw std::invalid_argument("R_outer must be > R_inner");
  }
  if (K_SQ_arg == 0) {
    throw std::invalid_argument("K_SQ must be > 0");
  }
  if (K_SQ_arg != static_cast<std::uint32_t>(K_SQ)) {
    std::ostringstream msg;
    msg << "K_SQ argument (" << K_SQ_arg
        << ") must match compile-time K_SQ (" << K_SQ << ")";
    throw std::invalid_argument(msg.str());
  }

  // Verify R_outer^2 fits in u64 (simple overflow guard).
  // At R_outer < 2^32, R_outer^2 < 2^64 — safe for project R ≤ 1e9.
  if (R_outer > (std::uint64_t{1} << 32)) {
    throw std::invalid_argument(
        "R_outer too large: R_outer^2 must fit in uint64_t (R_outer < 2^32)");
  }

  CampaignConstants c{};
  c.R_inner = R_inner;
  c.R_outer = R_outer;
  c.R_inner_sq = R_inner * R_inner;
  c.R_outer_sq = R_outer * R_outer;
  c.K_SQ_value = K_SQ_arg;
  c.S_value = static_cast<std::uint32_t>(S);
  c.C_value = static_cast<std::uint32_t>(C);
  c.o_x = static_cast<std::uint32_t>(OFFSET_X);
  c.o_y = static_cast<std::uint32_t>(OFFSET_Y);

  // Pre-filter bounds use CEIL_ISQRT(K_SQ) — blueprint §2. Floor would be
  // a canonical false-negative bug.
  const std::uint64_t ceil_sqrt_K =
      static_cast<std::uint64_t>(ceil_isqrt(static_cast<std::int64_t>(K_SQ_arg)));
  c.prefilter_inner = 2ULL * R_inner * ceil_sqrt_K + 1ULL;
  c.prefilter_outer = 2ULL * R_outer * ceil_sqrt_K + 1ULL;

  // 4 * R^2 * K_SQ as i128.
  const __int128 four_K = static_cast<__int128>(4) *
                          static_cast<__int128>(K_SQ_arg);
  const __int128 four_rin_sq_k =
      static_cast<__int128>(c.R_inner_sq) * four_K;
  const __int128 four_rout_sq_k =
      static_cast<__int128>(c.R_outer_sq) * four_K;
  split_i128(four_rin_sq_k, c.four_rin_sq_k_hi, c.four_rin_sq_k_lo);
  split_i128(four_rout_sq_k, c.four_rout_sq_k_hi, c.four_rout_sq_k_lo);

  // Adjacent Octants Lemma precondition: R_inner > floor_isqrt(2 * K_SQ).
  // For project scale this is trivially satisfied (R_inner ~ 1e8 vs ~9).
  const std::int64_t adj_bound =
      floor_isqrt(static_cast<std::int64_t>(2 * K_SQ_arg));
  if (static_cast<std::int64_t>(R_inner) <= adj_bound) {
    std::ostringstream msg;
    msg << "R_inner (" << R_inner
        << ") must exceed floor_isqrt(2*K_SQ) = " << adj_bound
        << " (Adjacent Octants Lemma, blueprint §2)";
    throw std::invalid_argument(msg.str());
  }

  // Annulus thickness is NOT enforced here. The real-valued bound
  //   R_outer - R_inner > S*sqrt(2) + 2*sqrt(K)
  // is a soundness precondition for the *campaign verdict* (blueprint §2,
  // Theorem 11 Case A.2), not a structural precondition for the Grid /
  // TileOp machinery. Tiny-radius tests (e.g. R_inner=10000, R_outer=10032
  // at width 32) exercise the primitives and must be allowed to run.
  //
  // `verify_annulus_thickness(...)` below is the user-facing check; it's
  // invoked by campaign_main at production init but skipped in unit tests.

  return c;
}

bool CampaignConstants::verify_annulus_thickness() const noexcept {
  // Conservative integer surrogate for R_outer - R_inner > S*sqrt(2) + 2*sqrt(K).
  // Using ceil_isqrt on both terms: strictly stronger than the real-valued
  // inequality, so passing here implies passing the original.
  const std::uint64_t ceil_sqrt_2 =
      static_cast<std::uint64_t>(ceil_isqrt(2));
  const std::uint64_t ceil_sqrt_K =
      static_cast<std::uint64_t>(ceil_isqrt(static_cast<std::int64_t>(K_SQ_value)));
  const std::uint64_t annulus_bound =
      static_cast<std::uint64_t>(S_value) * ceil_sqrt_2 + 2ULL * ceil_sqrt_K;
  return (R_outer - R_inner) > annulus_bound;
}

__int128 CampaignConstants::four_rin_sq_k_i128() const noexcept {
  return assemble_i128(four_rin_sq_k_hi, four_rin_sq_k_lo);
}

__int128 CampaignConstants::four_rout_sq_k_i128() const noexcept {
  return assemble_i128(four_rout_sq_k_hi, four_rout_sq_k_lo);
}

std::string CampaignConstants::canonical_hash() const {
  // Logical-field serialization (plan §3 + Q4 resolution).
  // Format: "K=<K>;R_inner=<Ri>;R_outer=<Ro>;offset=<ox>,<oy>;collar=<C>"
  std::ostringstream ss;
  ss << "K=" << K_SQ_value
     << ";R_inner=" << R_inner
     << ";R_outer=" << R_outer
     << ";offset=" << o_x << "," << o_y
     << ";collar=" << C_value;
  return detail::sha256_hex(ss.str());
}

std::string CampaignConstants::mr_witness_set_sha256() {
  return detail::sha256_hex(reinterpret_cast<const std::uint8_t*>(kFj64Table),
                            sizeof(kFj64Table));
}

}  // namespace campaign
