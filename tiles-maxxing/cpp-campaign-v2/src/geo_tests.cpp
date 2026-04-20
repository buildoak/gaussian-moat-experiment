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

constexpr __int128 square_band_delta(std::uint64_t radius,
                                     std::uint64_t ceil_sqrt_k) noexcept {
  return static_cast<__int128>(2) * to_i128(radius) * to_i128(ceil_sqrt_k) +
         to_i128(ceil_sqrt_k) * to_i128(ceil_sqrt_k);
}

constexpr __int128 lower_outer_delta(std::uint64_t radius,
                                     std::uint64_t ceil_sqrt_k) noexcept {
  return static_cast<__int128>(2) * to_i128(radius) * to_i128(ceil_sqrt_k) -
         to_i128(ceil_sqrt_k) * to_i128(ceil_sqrt_k);
}

}  // namespace

bool is_inner_prime(std::int64_t norm_sq,
                    const CampaignConstants& constants) noexcept {
  if (norm_sq < 0) {
    return false;
  }

  const std::uint64_t ceil_sqrt_k =
      static_cast<std::uint64_t>(ceil_isqrt(k_sq_value));
  const __int128 norm = static_cast<__int128>(norm_sq);
  const __int128 lower = to_i128(constants.R_inner_sq);
  const __int128 upper =
      lower + square_band_delta(constants.R_inner, ceil_sqrt_k);

  return lower <= norm && norm <= upper;
}

bool is_outer_prime(std::int64_t norm_sq,
                    const CampaignConstants& constants) noexcept {
  if (norm_sq < 0) {
    return false;
  }

  const std::uint64_t ceil_sqrt_k =
      static_cast<std::uint64_t>(ceil_isqrt(k_sq_value));
  const __int128 norm = static_cast<__int128>(norm_sq);
  const __int128 upper = to_i128(constants.R_outer_sq);
  const __int128 lower =
      upper - lower_outer_delta(constants.R_outer, ceil_sqrt_k);

  return lower <= norm && norm <= upper;
}

}  // namespace campaign
