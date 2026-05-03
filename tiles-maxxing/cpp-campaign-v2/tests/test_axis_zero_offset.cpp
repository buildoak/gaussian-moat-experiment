// tests/test_axis_zero_offset.cpp
//
// Regression for canonical zero-offset axis ownership: an inert axis prime
// (0, q) must be proper-owned by column i=0, then survive into visible TileOp
// material. This guards against accidentally relying on a negative-i tile or
// an offset-1 halo to see the y-axis.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/geo_tests.h"
#include "campaign/grid.h"
#include "campaign/sieve.h"
#include "campaign/tileop.h"
#include "../src/tileop_internal.h"

namespace {

constexpr std::uint64_t kAxisPrime = 251;  // prime and 3 mod 4
constexpr std::uint64_t kRinner = 248;
constexpr std::uint64_t kRouter = 254;

bool proper_contains(const campaign::TileCoord& coord, std::int64_t a,
                     std::int64_t b) {
  return coord.a_lo <= a && a <= coord.a_lo + campaign::S &&
         coord.b_lo <= b && b <= coord.b_lo + campaign::S;
}

std::optional<campaign::TileCoord> find_proper_owner(
    const std::vector<campaign::TileCoord>& coords, std::int64_t a,
    std::int64_t b) {
  const auto it = std::find_if(
      coords.begin(), coords.end(), [a, b](const campaign::TileCoord& coord) {
        return proper_contains(coord, a, b);
      });
  if (it == coords.end()) return std::nullopt;
  return *it;
}

bool visible_on_face(const campaign::TileOp& op, campaign::Face face) {
  const int begin = campaign::face_offset(op, face);
  const int count = op.n[static_cast<int>(face)];
  for (int idx = begin; idx < begin + count; ++idx) {
    const int group = op.face_groups[idx];
    if (group > 0 &&
        (campaign::bit_test(op.inner_flags, group) ||
         campaign::bit_test(op.outer_flags, group))) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(AxisZeroOffset, AxisPrimeIsColumnZeroProperVisibleMaterial) {
  ASSERT_EQ(campaign::OFFSET_X, 0);
  ASSERT_EQ(campaign::OFFSET_Y, 0);
  ASSERT_TRUE(campaign::is_prime_u64(kAxisPrime));
  ASSERT_EQ(kAxisPrime % 4, 3u);

  const auto constants = campaign::CampaignConstants::from_radii(
      kRinner, kRouter, campaign::k_sq_value);
  const auto grid =
      campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);

  ASSERT_TRUE(grid.has_column(0));
  EXPECT_EQ(grid.i_min, 0);
  EXPECT_FALSE(grid.has_column(-1));

  const auto column_zero = grid.enumerate_column_tiles(0);
  const auto owner = find_proper_owner(column_zero, 0, kAxisPrime);
  ASSERT_TRUE(owner.has_value())
      << "axis prime must be proper-owned by column i=0, not by halo only";
  EXPECT_EQ(owner->i, 0);
  EXPECT_EQ(owner->a_lo, 0);
  EXPECT_TRUE(proper_contains(*owner, 0, kAxisPrime));

  const auto primes = campaign::sieve_tile(*owner, constants);
  const auto axis_it = std::find_if(
      primes.begin(), primes.end(), [](const campaign::Prime& prime) {
        return prime.a == 0 &&
               prime.b == static_cast<std::int64_t>(kAxisPrime);
      });
  ASSERT_NE(axis_it, primes.end())
      << "column i=0 tile must emit the inert y-axis Gaussian prime";
  EXPECT_EQ(axis_it->norm_sq, kAxisPrime * kAxisPrime);

  const std::int64_t packed_col = 0 - (owner->a_lo - campaign::C);
  const std::int64_t packed_row =
      static_cast<std::int64_t>(kAxisPrime) - (owner->b_lo - campaign::C);
  EXPECT_EQ(axis_it->packed_pos,
            static_cast<std::uint32_t>(packed_row * campaign::SIDE_EXP +
                                       packed_col));
  EXPECT_EQ(packed_col, campaign::C)
      << "zero-offset proper owner should place x=0 on the tile boundary";

  const bool inner = campaign::is_inner_prime(
      static_cast<std::int64_t>(axis_it->norm_sq), constants);
  const bool outer = campaign::is_outer_prime(
      static_cast<std::int64_t>(axis_it->norm_sq), constants);
  ASSERT_TRUE(inner || outer)
      << "fixture must put the axis prime into visible boundary material";

  const campaign::TileOp axis_only =
      campaign::internal::build_tileop_for_primes(
          std::vector<campaign::Prime>{*axis_it},
          std::vector<campaign::internal::PrimeGeoFlags>{{inner, outer}},
          *owner, constants);
  ASSERT_EQ(axis_only.tile_flags & campaign::EMPTY_BIT, 0);
  ASSERT_EQ(axis_only.tile_flags & campaign::OVERFLOW_BIT, 0);
  EXPECT_TRUE(visible_on_face(axis_only, campaign::Face::L));

  const campaign::TileOp full_op =
      campaign::process_tile(*owner, constants, grid);
  ASSERT_EQ(full_op.tile_flags & campaign::EMPTY_BIT, 0);
  ASSERT_EQ(full_op.tile_flags & campaign::OVERFLOW_BIT, 0);
  EXPECT_TRUE(visible_on_face(full_op, campaign::Face::L));
}
