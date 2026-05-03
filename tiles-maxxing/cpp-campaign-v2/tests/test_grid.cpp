// tests/test_grid.cpp
//
// Exercises the snapped-grid enumerator at tiny radii (blueprint §4.3 +
// plan §4 M2 acceptance gate adapted for Phase 1 scope).
//
// Tiny-radius params: R_inner=10000, R_outer=10032, K_SQ=36.
// These do not satisfy the annulus-thickness soundness precondition (used
// only by campaign_main pre-verdict) but they do exercise the grid
// enumeration primitives end-to-end.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "campaign/constants.h"
#include "campaign/grid.h"

namespace {

constexpr std::uint64_t kRinner = 10000;
constexpr std::uint64_t kRouter = 10032;

bool exhaustive_tile_active(std::int32_t i, std::int32_t j,
                            std::uint64_t r_inner,
                            std::uint64_t r_outer) {
  const std::int64_t x_lo = campaign::OFFSET_X +
                            static_cast<std::int64_t>(campaign::S) * i;
  const std::int64_t x_hi = x_lo + campaign::S;
  const std::int64_t y_lo = campaign::OFFSET_Y +
                            static_cast<std::int64_t>(campaign::S) * j;
  const std::int64_t y_hi = y_lo + campaign::S;
  const __int128 r_inner_sq =
      static_cast<__int128>(r_inner) * static_cast<__int128>(r_inner);
  const __int128 r_outer_sq =
      static_cast<__int128>(r_outer) * static_cast<__int128>(r_outer);

  for (std::int64_t x = std::max<std::int64_t>(0, x_lo); x <= x_hi; ++x) {
    for (std::int64_t y = std::max<std::int64_t>(y_lo, x); y <= y_hi; ++y) {
      const __int128 norm_sq =
          static_cast<__int128>(x) * static_cast<__int128>(x) +
          static_cast<__int128>(y) * static_cast<__int128>(y);
      if (norm_sq >= r_inner_sq && norm_sq <= r_outer_sq) return true;
    }
  }
  return false;
}

std::vector<std::pair<std::int32_t, std::int32_t>>
exhaustive_active_tiles(std::uint64_t r_inner, std::uint64_t r_outer) {
  std::vector<std::pair<std::int32_t, std::int32_t>> out;
  const std::int32_t i_upper = static_cast<std::int32_t>(
      (r_outer - campaign::OFFSET_X) / campaign::S);
  const std::int32_t j_upper = static_cast<std::int32_t>(
      (r_outer - campaign::OFFSET_Y) / campaign::S);
  for (std::int32_t i = 0; i <= i_upper; ++i) {
    for (std::int32_t j = 0; j <= j_upper; ++j) {
      if (exhaustive_tile_active(i, j, r_inner, r_outer)) {
        out.emplace_back(i, j);
      }
    }
  }
  return out;
}

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
  EXPECT_THROW(
      campaign::Grid::build(100, (std::uint64_t{1} << 32) + 1,
                            campaign::k_sq_value),
      std::invalid_argument);
}

TEST(Grid, TinyRadiiMatchExhaustiveReference) {
  auto g = campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);
  const auto tiles = g.enumerate_active_tiles();

  std::vector<std::pair<std::int32_t, std::int32_t>> got;
  got.reserve(tiles.size());
  for (const auto& t : tiles) got.emplace_back(t.i, t.j);

  const auto expected = exhaustive_active_tiles(kRinner, kRouter);
  EXPECT_EQ(got, expected);
}

TEST(Grid, ScaleBuildCompletesUnder500ms) {
  const auto start = std::chrono::steady_clock::now();
  const auto g = campaign::Grid::build(1000000, 1001024,
                                       campaign::k_sq_value);
  const auto stop = std::chrono::steady_clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);

  EXPECT_GT(g.total_tiles, 0);
  EXPECT_LT(elapsed_ms.count(), 500);
}

TEST(Grid, VerifyInvariantsReturnsEmptyOnWellFormedBuild) {
  // Audit rec (1): always-on seatbelt. Must succeed on the canonical tiny
  // build that already satisfies I1/I2/I4 structurally.
  auto g = campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);
  const std::string err = g.verify_invariants();
  EXPECT_TRUE(err.empty())
      << "well-formed grid should verify clean, got: " << err;
}

TEST(Grid, VerifyInvariantsDetectsI2Violation) {
  // Synthesize a broken grid: bump the second column's j_low so the
  // shift from column 0 to column 1 exceeds 1. The release-mode shape
  // check must catch this without needing is_active_tile.
  auto g = campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);
  ASSERT_GE(g.j_low.size(), 2u)
      << "tiny-radius build should span at least 2 columns";

  g.j_low[1] = g.j_low[0] + 5;   // shift > 1
  g.j_high[1] = g.j_high[1] + 5; // keep column non-empty

  const std::string err = g.verify_invariants();
  EXPECT_FALSE(err.empty()) << "tampered grid must flag a violation";
  EXPECT_NE(err.find("I2"), std::string::npos)
      << "diagnostic must name the violated invariant, got: " << err;
}

TEST(Grid, VerifyInvariantsDetectsI1MalformedRange) {
  // j_low > j_high + 1 is a structurally malformed column range that
  // the shape check catches without re-running is_active_tile.
  auto g = campaign::Grid::build(kRinner, kRouter, campaign::k_sq_value);
  ASSERT_FALSE(g.j_low.empty());

  g.j_low[0] = g.j_high[0] + 10;  // malformed: j_low > j_high + 1

  const std::string err = g.verify_invariants();
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("I1"), std::string::npos)
      << "diagnostic must name I1 (malformed range), got: " << err;
}

TEST(Grid, VerifyInvariantsDetectsUpperDiagonalOrphan) {
  campaign::Grid g{};
  g.i_min = 0;
  g.i_max = 1;
  g.j_low = {10, 11};
  g.j_high = {10, 11};
  g.tower_offset = {0, 1, 2};
  g.total_tiles = 2;

  const std::string err = g.verify_invariants();
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("I4"), std::string::npos)
      << "diagnostic must name I4, got: " << err;
  EXPECT_NE(err.find("(0,10)"), std::string::npos)
      << "diagnostic must identify the current-column witness, got: " << err;
  EXPECT_NE(err.find("(1,11)"), std::string::npos)
      << "diagnostic must identify the next-column witness, got: " << err;
}

TEST(Grid, VerifyInvariantsDetectsLowerDiagonalOrphan) {
  campaign::Grid g{};
  g.i_min = 0;
  g.i_max = 1;
  g.j_low = {10, 9};
  g.j_high = {10, 9};
  g.tower_offset = {0, 1, 2};
  g.total_tiles = 2;

  const std::string err = g.verify_invariants();
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("I4"), std::string::npos)
      << "diagnostic must name I4, got: " << err;
  EXPECT_NE(err.find("(0,10)"), std::string::npos)
      << "diagnostic must identify the current-column witness, got: " << err;
  EXPECT_NE(err.find("(1,9)"), std::string::npos)
      << "diagnostic must identify the next-column witness, got: " << err;
}

TEST(Grid, VerifyInvariantsEmptyGridReturnsEmpty) {
  // Empty grids trivially satisfy — must not spuriously report error.
  campaign::Grid g{};
  g.i_min = 0;
  g.i_max = -1;
  g.total_tiles = 0;
  EXPECT_TRUE(g.verify_invariants().empty());
}

}  // namespace
