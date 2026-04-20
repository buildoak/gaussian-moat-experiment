// tests/test_grid.cpp
//
// Exercises the snapped-grid enumerator at tiny radii (blueprint §4.3 +
// plan §4 M2 acceptance gate adapted for Phase 1 scope).
//
// Tiny-radius params: R_inner=10000, R_outer=10032, K_SQ=36.
// These do not satisfy the annulus-thickness soundness precondition (used
// only by campaign_main pre-verdict) but they do exercise the grid
// enumeration primitives end-to-end.

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "campaign/constants.h"
#include "campaign/grid.h"

namespace {

constexpr std::uint64_t kRinner = 10000;
constexpr std::uint64_t kRouter = 10032;

TEST(Grid, TinyRadiiBuildsNonEmpty) {
  auto g = campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);
  EXPECT_GE(g.total_tiles, 1)
      << "Grid should have at least one active tile at tiny radii";
  EXPECT_LE(g.i_min, g.i_max) << "i_min must be <= i_max for a non-empty grid";
}

TEST(Grid, AllEnumeratedTilesAreInOctant) {
  auto g = campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);
  const auto tiles = g.enumerate_active_tiles();
  ASSERT_EQ(static_cast<std::int64_t>(tiles.size()), g.total_tiles);

  for (const auto& t : tiles) {
    // Octant membership via tile box: some (x, y) in box with x >= 0 and
    // y >= x. Verify that the tile's proper region intersects the octant
    // (necessary condition).
    const std::int64_t x_hi = t.a_lo + campaign::S;
    const std::int64_t y_hi = t.b_lo + campaign::S;
    EXPECT_GE(x_hi, 0) << "Tile box must touch x >= 0 half-plane";
    EXPECT_GE(y_hi, t.a_lo) << "Tile box must allow y >= x in the octant";
  }
}

TEST(Grid, I1TowerContiguity) {
  auto g = campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);
  const auto tiles = g.enumerate_active_tiles();

  // Group tile j's by column and verify contiguous.
  std::map<std::int32_t, std::vector<std::int32_t>> by_col;
  for (const auto& t : tiles) by_col[t.i].push_back(t.j);
  for (auto& [i, js] : by_col) {
    std::sort(js.begin(), js.end());
    const std::int32_t first = js.front();
    for (std::size_t k = 0; k < js.size(); ++k) {
      EXPECT_EQ(js[k], first + static_cast<std::int32_t>(k))
          << "Column i=" << i << " violates I1 contiguity at idx " << k;
    }
  }
}

TEST(Grid, I2BoundedShift) {
  auto g = campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);
  for (std::int32_t i = g.i_min; i < g.i_max; ++i) {
    const auto [lo, hi] = g.column_bounds(i);
    const auto [lo1, hi1] = g.column_bounds(i + 1);
    // Only compare when both non-empty.
    if (hi >= lo && hi1 >= lo1) {
      const int d_lo = lo1 - lo;
      const int d_hi = hi1 - hi;
      EXPECT_GE(d_lo, -1) << "I2 j_low shift < -1 at i=" << i;
      EXPECT_LE(d_lo, 1) << "I2 j_low shift > 1 at i=" << i;
      EXPECT_GE(d_hi, -1) << "I2 j_high shift < -1 at i=" << i;
      EXPECT_LE(d_hi, 1) << "I2 j_high shift > 1 at i=" << i;
    }
  }
}

TEST(Grid, FlatIndexRoundtrip) {
  auto g = campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);
  const auto tiles = g.enumerate_active_tiles();
  for (std::size_t k = 0; k < tiles.size(); ++k) {
    const auto& t = tiles[k];
    const std::int64_t fi = g.flat_index(t.i, t.j);
    EXPECT_EQ(fi, static_cast<std::int64_t>(k))
        << "flat_index(" << t.i << "," << t.j << ") must equal emission order";
  }
}

TEST(Grid, RejectsBadRadii) {
  EXPECT_THROW(campaign::Grid::build(0, 100, campaign::k_sq_value),
               std::invalid_argument);
  EXPECT_THROW(campaign::Grid::build(100, 50, campaign::k_sq_value),
               std::invalid_argument);
  EXPECT_THROW(campaign::Grid::build(100, 200, 0),
               std::invalid_argument);
}

}  // namespace
