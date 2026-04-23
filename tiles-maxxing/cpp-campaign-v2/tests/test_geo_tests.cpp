// tests/test_geo_tests.cpp

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "campaign/constants.h"
#include "campaign/geo_tests.h"

namespace {

campaign::CampaignConstants make_constants(std::uint64_t r_inner,
                                           std::uint64_t r_outer) {
  return campaign::CampaignConstants::from_radii(
      r_inner, r_outer, static_cast<std::uint32_t>(campaign::k_sq_value));
}

std::int64_t checked_i64(__int128 value) {
  EXPECT_LE(value, static_cast<__int128>(std::numeric_limits<std::int64_t>::max()));
  EXPECT_GE(value, static_cast<__int128>(std::numeric_limits<std::int64_t>::min()));
  return static_cast<std::int64_t>(value);
}

__int128 floor_sqrt_i128(__int128 value) {
  EXPECT_GE(value, 0);
  if (value <= 0) {
    return 0;
  }

  __int128 lo = 0;
  __int128 hi = 1;
  while (hi <= value / hi) {
    hi *= 2;
  }
  while (lo + 1 < hi) {
    const __int128 mid = lo + (hi - lo) / 2;
    if (mid <= value / mid) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

__int128 inner_upper(const campaign::CampaignConstants& cc) {
  return static_cast<__int128>(cc.R_inner_sq) +
         static_cast<__int128>(campaign::k_sq_value) +
         floor_sqrt_i128(cc.four_rin_sq_k_i128());
}

__int128 outer_lower(const campaign::CampaignConstants& cc) {
  return static_cast<__int128>(cc.R_outer_sq) +
         static_cast<__int128>(campaign::k_sq_value) -
         floor_sqrt_i128(cc.four_rout_sq_k_i128());
}

}  // namespace

static_assert(campaign::ceil_isqrt(36) == 6, "ceil_isqrt(36) must be 6");
static_assert(campaign::ceil_isqrt(40) == 7, "ceil_isqrt(40) must be 7");

TEST(GeoTests, InnerBandCorners) {
  const auto cc = make_constants(10'000, 20'000);
  const __int128 lower = static_cast<__int128>(cc.R_inner_sq);
  const __int128 upper = inner_upper(cc);

  EXPECT_FALSE(campaign::is_inner_prime(checked_i64(lower - 1), cc));
  EXPECT_TRUE(campaign::is_inner_prime(checked_i64(lower), cc));
  EXPECT_TRUE(campaign::is_inner_prime(checked_i64(upper), cc));
  EXPECT_FALSE(campaign::is_inner_prime(checked_i64(upper + 1), cc));
}

TEST(GeoTests, OuterBandCorners) {
  const auto cc = make_constants(10'000, 20'000);
  const __int128 lower = outer_lower(cc);
  const __int128 upper = static_cast<__int128>(cc.R_outer_sq);

  EXPECT_FALSE(campaign::is_outer_prime(checked_i64(lower - 1), cc));
  EXPECT_TRUE(campaign::is_outer_prime(checked_i64(lower), cc));
  EXPECT_TRUE(campaign::is_outer_prime(checked_i64(upper), cc));
  EXPECT_FALSE(campaign::is_outer_prime(checked_i64(upper + 1), cc));
}

TEST(GeoTests, NonSquareKUsesCeilBoundary) {
  if constexpr (campaign::k_sq_value != 40) {
    GTEST_SKIP() << "K=40-specific boundary test";
  }

  static_assert(campaign::ceil_isqrt(40) == 7, "K=40 must round up to 7");
  const auto cc = make_constants(10'000, 20'000);
  const __int128 r = static_cast<__int128>(cc.R_inner);
  const __int128 floor_width = 6;
  const __int128 ceil_width = campaign::ceil_isqrt(campaign::k_sq_value);
  const __int128 delta = 2 * r * floor_width + 100;

  EXPECT_EQ(ceil_width, 7);
  EXPECT_GT(delta, 2 * r * floor_width + floor_width * floor_width);
  EXPECT_LE(delta, 2 * r * ceil_width + ceil_width * ceil_width);
  EXPECT_TRUE(campaign::is_inner_prime(
      checked_i64(static_cast<__int128>(cc.R_inner_sq) + delta), cc));
}

TEST(GeoTests, LargeRUsesInt128Bounds) {
  const auto cc = make_constants(800'000'000, 800'008'192);
  const __int128 inner_hi = inner_upper(cc);
  const __int128 outer_lo = outer_lower(cc);

  EXPECT_GT(inner_hi, static_cast<__int128>(cc.R_inner_sq));
  EXPECT_LT(outer_lo, static_cast<__int128>(cc.R_outer_sq));
  EXPECT_TRUE(campaign::is_inner_prime(checked_i64(inner_hi), cc));
  EXPECT_FALSE(campaign::is_inner_prime(checked_i64(inner_hi + 1), cc));
  EXPECT_TRUE(campaign::is_outer_prime(checked_i64(outer_lo), cc));
  EXPECT_FALSE(campaign::is_outer_prime(checked_i64(outer_lo - 1), cc));
}

TEST(GeoTests, Deterministic) {
  const auto cc = make_constants(10'000, 20'000);
  const std::int64_t inner_sample = checked_i64(inner_upper(cc));
  const std::int64_t outer_sample = checked_i64(outer_lower(cc));
  const bool inner_expected = campaign::is_inner_prime(inner_sample, cc);
  const bool outer_expected = campaign::is_outer_prime(outer_sample, cc);

  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(campaign::is_inner_prime(inner_sample, cc), inner_expected);
    EXPECT_EQ(campaign::is_outer_prime(outer_sample, cc), outer_expected);
  }
}

TEST(GeoTests, ProjectBandsAreDisjoint) {
  const auto cc = make_constants(80'000'000, 80'008'192);
  const __int128 inner_hi = inner_upper(cc);
  const __int128 outer_lo = outer_lower(cc);

  EXPECT_LT(inner_hi, outer_lo);
  EXPECT_FALSE(campaign::is_outer_prime(checked_i64(inner_hi), cc));
  EXPECT_FALSE(campaign::is_inner_prime(checked_i64(outer_lo), cc));
}
