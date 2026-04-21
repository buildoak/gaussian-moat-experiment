// src/geo_tests.cpp

#include "campaign/geo_tests.h"

#include <cstdint>

#include "campaign/constants.h"

namespace campaign {

static_assert(ceil_isqrt(36) == 6, "ceil_isqrt(36) must be 6");
static_assert(ceil_isqrt(40) == 7, "ceil_isqrt(40) must be 7");

namespace {

constexpr __int128 to_i128(std::uint64_t value) noexcept {
  return static_cast<__int128>(value);
}

}  // namespace

bool is_inner_prime(std::int64_t norm_sq,
                    const CampaignConstants& constants) noexcept {
  if (norm_sq < 0) {
    return false;
  }

  // Spec predicate (Codex-M1 fix, spec lines 307-317):
  //   geo_I := { p in V(G_full) : ||p||^2 <= (R_inner + sqrt(K))^2 }
  //   integer test: (norm_sq - R_inner^2 - K)^2 <= 4*R_inner^2*K
  // The `p in V(G_full)` clause implies ||p||^2 >= R_inner^2 (spec line 321).
  // So membership = R_inner^2 <= norm_sq <= (R_inner + sqrt(K))^2.
  // At K=36 this matches the prior ceil_isqrt(K) band exactly. At K=40 the
  // band was strictly wider than spec; norm-form is canonical.
  const __int128 norm = static_cast<__int128>(norm_sq);
  const __int128 r_sq = to_i128(constants.R_inner_sq);
  if (norm < r_sq) {
    return false;
  }
  const __int128 k = static_cast<__int128>(k_sq_value);
  const __int128 four_r_sq_k = constants.four_rin_sq_k_i128();
  const __int128 eps = norm - r_sq - k;
  return eps * eps <= four_r_sq_k;
}

bool is_outer_prime(std::int64_t norm_sq,
                    const CampaignConstants& constants) noexcept {
  if (norm_sq < 0) {
    return false;
  }

  // Spec predicate (Codex-M1 fix, spec lines 307-317):
  //   geo_O := { p in V(G_full) : ||p||^2 >= (R_outer - sqrt(K))^2 }
  //   integer test: (R_outer^2 - norm_sq + K)^2 <= 4*R_outer^2*K
  //                 = (norm_sq - R_outer^2 - K)^2 <= 4*R_outer^2*K
  // The `p in V(G_full)` clause implies ||p||^2 <= R_outer^2.
  // So membership = (R_outer - sqrt(K))^2 <= norm_sq <= R_outer^2.
  const __int128 norm = static_cast<__int128>(norm_sq);
  const __int128 r_sq = to_i128(constants.R_outer_sq);
  if (norm > r_sq) {
    return false;
  }
  const __int128 k = static_cast<__int128>(k_sq_value);
  const __int128 four_r_sq_k = constants.four_rout_sq_k_i128();
  const __int128 eps = norm - r_sq - k;
  return eps * eps <= four_r_sq_k;
}

}  // namespace campaign
