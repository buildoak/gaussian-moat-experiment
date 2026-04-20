// tests/test_compositor.cpp

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "campaign/compositor.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"

namespace {

campaign::Grid make_grid(const std::vector<std::pair<int, int>>& columns) {
  campaign::Grid grid;
  grid.i_min = 0;
  grid.i_max = static_cast<std::int32_t>(columns.size()) - 1;
  grid.j_low.reserve(columns.size());
  grid.j_high.reserve(columns.size());
  grid.tower_offset.reserve(columns.size() + 1);
  grid.tower_offset.push_back(0);

  std::int64_t total = 0;
  for (const auto [lo, hi] : columns) {
    grid.j_low.push_back(lo);
    grid.j_high.push_back(hi);
    total += hi - lo + 1;
    grid.tower_offset.push_back(total);
  }
  grid.total_tiles = total;
  return grid;
}

campaign::TileOp make_tile(
    const std::array<std::vector<std::uint8_t>, campaign::NUM_FACES>& faces,
    const std::vector<std::uint8_t>& inner_groups = {},
    const std::vector<std::uint8_t>& outer_groups = {},
    std::uint8_t tile_flags = 0) {
  campaign::TileOp op{};
  op.tile_flags = tile_flags;

  int off = 0;
  for (int f = 0; f < campaign::NUM_FACES; ++f) {
    op.n[f] = static_cast<std::uint8_t>(faces[f].size());
    for (std::uint8_t group : faces[f]) {
      op.face_groups[off++] = group;
    }
  }
  for (std::uint8_t group : inner_groups) {
    campaign::bit_set(op.inner_flags, group);
  }
  for (std::uint8_t group : outer_groups) {
    campaign::bit_set(op.outer_flags, group);
  }
  return op;
}

campaign::TileOp make_tile(
    const std::vector<std::uint8_t>& face_i,
    const std::vector<std::uint8_t>& face_o,
    const std::vector<std::uint8_t>& face_l,
    const std::vector<std::uint8_t>& face_r,
    const std::vector<std::uint8_t>& inner_groups = {},
    const std::vector<std::uint8_t>& outer_groups = {},
    std::uint8_t tile_flags = 0) {
  return make_tile({face_i, face_o, face_l, face_r},
                   inner_groups,
                   outer_groups,
                   tile_flags);
}

campaign::Verdict run_campaign(
    const campaign::Grid& grid,
    const std::vector<std::vector<campaign::TileOp>>& columns) {
  campaign::Compositor compositor;
  compositor.init(grid);
  for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
    compositor.ingest_column(i, columns[static_cast<std::size_t>(i)].data());
  }
  return compositor.finalize();
}

}  // namespace

TEST(Compositor, SingleTileMoatWhenOnlyInnerFlagged) {
  const campaign::Grid grid = make_grid({{0, 0}});
  const campaign::TileOp tile =
      make_tile({1}, {}, {}, {}, {1}, {});

  EXPECT_EQ(run_campaign(grid, {{tile}}), campaign::Verdict::kMoat);
}

TEST(Compositor, SingleTileSpanningWhenOneGroupHasBothFlags) {
  const campaign::Grid grid = make_grid({{0, 0}});
  const campaign::TileOp tile =
      make_tile({1}, {}, {}, {}, {1}, {1});

  campaign::Compositor compositor;
  compositor.init(grid);
  compositor.ingest_column(0, &tile);

  EXPECT_TRUE(compositor.has_spanning());
  EXPECT_EQ(compositor.finalize(), campaign::Verdict::kSpanning);
}

TEST(Compositor, TwoTileVerticalStitchingMergesReachabilityByOrdinal) {
  const campaign::Grid grid = make_grid({{0, 1}});
  const campaign::TileOp bottom =
      make_tile({}, {1}, {}, {}, {1}, {});
  const campaign::TileOp top =
      make_tile({2}, {}, {}, {}, {}, {2});
  const std::vector<campaign::TileOp> column{bottom, top};

  campaign::Compositor compositor;
  compositor.init(grid);
  compositor.ingest_column(0, column.data());

  EXPECT_TRUE(compositor.has_spanning());
  EXPECT_EQ(compositor.finalize(), campaign::Verdict::kSpanning);
}

TEST(Compositor, TwoTileNoSpanningWhenFlaggedGroupsAreNotStitched) {
  const campaign::Grid grid = make_grid({{0, 1}});
  const campaign::TileOp bottom =
      make_tile({1}, {2}, {}, {}, {1}, {});
  const campaign::TileOp top =
      make_tile({3}, {4}, {}, {}, {}, {4});
  const std::vector<campaign::TileOp> column{bottom, top};

  campaign::Compositor compositor;
  compositor.init(grid);
  compositor.ingest_column(0, column.data());

  EXPECT_FALSE(compositor.has_spanning());
  EXPECT_EQ(compositor.finalize(), campaign::Verdict::kMoat);
}

TEST(Compositor, OverflowTileConservativelySpans) {
  const campaign::Grid grid = make_grid({{0, 0}});
  const campaign::TileOp tile =
      make_tile({}, {}, {}, {}, {}, {}, campaign::OVERFLOW_BIT);

  campaign::Compositor compositor;
  compositor.init(grid);
  compositor.ingest_column(0, &tile);

  EXPECT_TRUE(compositor.has_spanning());
  EXPECT_EQ(compositor.finalize(), campaign::Verdict::kSpanning);
}

TEST(Compositor, SideExposedFacesDoNotStitchAcrossOctantBoundary) {
  const campaign::Grid grid = make_grid({{0, 0}, {0, 0}});
  const campaign::TileOp left =
      make_tile({}, {}, {1}, {}, {1}, {});
  const campaign::TileOp right =
      make_tile({}, {}, {}, {1}, {}, {1});

  EXPECT_EQ(run_campaign(grid, {{left}, {right}}), campaign::Verdict::kMoat);
}

TEST(Compositor, PortCountMismatchFails) {
  const campaign::Grid grid = make_grid({{0, 1}});
  const campaign::TileOp bottom =
      make_tile({}, {1}, {}, {}, {}, {});
  const campaign::TileOp top =
      make_tile({}, {}, {}, {}, {}, {});
  const std::vector<campaign::TileOp> column{bottom, top};

  campaign::Compositor compositor;
  compositor.init(grid);

#ifndef NDEBUG
  EXPECT_DEATH(compositor.ingest_column(0, column.data()),
               "Lemma 4 port-count mismatch");
#else
  EXPECT_THROW(compositor.ingest_column(0, column.data()),
               std::runtime_error);
#endif
}

TEST(Compositor, DeterministicVerdictAcrossRuns) {
  const campaign::Grid grid = make_grid({{0, 1}});
  const campaign::TileOp bottom =
      make_tile({}, {1}, {}, {}, {1}, {});
  const campaign::TileOp top =
      make_tile({2}, {}, {}, {}, {}, {2});
  const std::vector<std::vector<campaign::TileOp>> columns{{bottom, top}};

  for (int run = 0; run < 3; ++run) {
    EXPECT_EQ(run_campaign(grid, columns), campaign::Verdict::kSpanning);
  }
}
