// tests/test_streaming_compositor.cpp

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "campaign/compositor.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/streaming_compositor.h"
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
    if (hi >= lo) total += hi - lo + 1;
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

class FrontierOracle {
 public:
  explicit FrontierOracle(const campaign::Grid& grid) : grid_(grid) {
    const std::uint64_t total_groups =
        static_cast<std::uint64_t>(grid.total_tiles) * kGroupsPerTile;
    if (total_groups > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("test grid too large");
    }
    parent_.resize(static_cast<std::size_t>(total_groups));
    reach_.assign(static_cast<std::size_t>(total_groups), 0);
    tileops_.resize(static_cast<std::size_t>(grid.total_tiles));
    ingested_.assign(static_cast<std::size_t>(grid.total_tiles), 0);
    for (std::uint32_t gid = 0; gid < total_groups; ++gid) parent_[gid] = gid;
  }

  void ingest_column(std::int32_t i,
                     const std::vector<campaign::TileOp>& column) {
    current_i_ = i;
    current_coords_ = grid_.enumerate_column_tiles(i);
    ASSERT_EQ(column.size(), current_coords_.size());

    for (std::size_t local = 0; local < current_coords_.size(); ++local) {
      const auto& coord = current_coords_[local];
      const std::int64_t idx = checked_tile_index(coord.i, coord.j);
      const campaign::TileOp& op = column[local];
      tileops_[static_cast<std::size_t>(idx)] = op;
      ingested_[static_cast<std::size_t>(idx)] = 1;
      mark_tile_ports(idx, op);
      if (grid_.flat_index(i, coord.j - 1) >= 0) {
        const std::int64_t below_idx = checked_tile_index(i, coord.j - 1);
        match_io(below_idx, idx,
                 tileops_[static_cast<std::size_t>(below_idx)], op);
      }
    }

    const std::int32_t left_i = i - 1;
    if (grid_.has_column(left_i)) {
      for (const auto& coord : current_coords_) {
        if (grid_.flat_index(left_i, coord.j) < 0) continue;
        const std::int64_t left_idx = checked_tile_index(left_i, coord.j);
        if (!ingested_[static_cast<std::size_t>(left_idx)]) continue;
        const std::int64_t idx = checked_tile_index(i, coord.j);
        match_lr(left_idx, idx,
                 tileops_[static_cast<std::size_t>(left_idx)],
                 tileops_[static_cast<std::size_t>(idx)]);
      }
    }
  }

  std::vector<campaign::FrontierPort> project_frontier() const {
    std::vector<campaign::FrontierPort> out;
    std::map<std::uint32_t, std::uint32_t> canonical;
    std::uint32_t next_component = 1;

    for (const auto& coord : current_coords_) {
      const std::int64_t idx = checked_tile_index(coord.i, coord.j);
      const campaign::TileOp& op = tileops_[static_cast<std::size_t>(idx)];
      if (op.tile_flags & campaign::OVERFLOW_BIT) continue;
      const int off = campaign::face_offset(op, campaign::Face::R);
      const int face_r = static_cast<int>(campaign::Face::R);
      for (int p = 0; p < op.n[face_r]; ++p) {
        const std::uint32_t root =
            find_const(global_group_id(idx, op.face_groups[off + p]));
        auto [it, inserted] = canonical.emplace(root, next_component);
        if (inserted) ++next_component;
        out.push_back(campaign::FrontierPort{
            coord.j,
            static_cast<std::uint8_t>(p + 1),
            it->second,
            reach_[root],
        });
      }
    }
    return out;
  }

 private:
  static constexpr std::uint32_t kGroupsPerTile = 128;
  static constexpr std::uint8_t kReachBoth = 0x3;

  campaign::Grid grid_;
  std::int32_t current_i_ = 0;
  std::vector<std::uint32_t> parent_;
  std::vector<std::uint8_t> reach_;
  std::vector<campaign::TileOp> tileops_;
  std::vector<std::uint8_t> ingested_;
  std::vector<campaign::TileCoord> current_coords_;

  std::uint32_t find(std::uint32_t x) {
    std::uint32_t root = x;
    while (parent_[root] != root) root = parent_[root];
    while (parent_[x] != x) {
      const std::uint32_t next = parent_[x];
      parent_[x] = root;
      x = next;
    }
    return root;
  }

  std::uint32_t find_const(std::uint32_t x) const {
    while (parent_[x] != x) x = parent_[x];
    return x;
  }

  std::uint32_t unite(std::uint32_t a, std::uint32_t b) {
    std::uint32_t ra = find(a);
    std::uint32_t rb = find(b);
    if (rb < ra) std::swap(ra, rb);
    if (ra != rb) {
      parent_[rb] = ra;
      reach_[ra] = static_cast<std::uint8_t>(reach_[ra] | reach_[rb]);
    }
    return ra;
  }

  void mark(std::uint32_t gid, std::uint8_t bits) {
    const std::uint32_t root = find(gid);
    reach_[root] = static_cast<std::uint8_t>(reach_[root] | bits);
  }

  std::uint32_t global_group_id(std::int64_t tile_index,
                                int group_label) const {
    return static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(tile_index) * kGroupsPerTile +
        static_cast<std::uint64_t>(group_label - 1));
  }

  std::int64_t checked_tile_index(std::int32_t i, std::int32_t j) const {
    const std::int64_t idx = grid_.flat_index(i, j);
    if (idx < 0) throw std::runtime_error("bad test tile coordinate");
    return idx;
  }

  void mark_tile_ports(std::int64_t tile_index, const campaign::TileOp& op) {
    if (op.tile_flags & campaign::OVERFLOW_BIT) {
      mark_all_present_ports(tile_index, op, kReachBoth);
      return;
    }
    if (op.tile_flags & campaign::EMPTY_BIT) return;
    for (int f = 0; f < campaign::NUM_FACES; ++f) {
      const auto face = static_cast<campaign::Face>(f);
      const int off = campaign::face_offset(op, face);
      for (int p = 0; p < op.n[f]; ++p) {
        const int group_label = op.face_groups[off + p];
        std::uint8_t bits = 0;
        if (campaign::bit_test(op.inner_flags, group_label)) bits |= 0x1;
        if (campaign::bit_test(op.outer_flags, group_label)) bits |= 0x2;
        if (bits != 0) mark(global_group_id(tile_index, group_label), bits);
      }
    }
  }

  void mark_all_present_ports(std::int64_t tile_index,
                              const campaign::TileOp& op,
                              std::uint8_t bits) {
    for (int f = 0; f < campaign::NUM_FACES; ++f) {
      const auto face = static_cast<campaign::Face>(f);
      const int off = campaign::face_offset(op, face);
      for (int p = 0; p < op.n[f]; ++p) {
        const int group_label = op.face_groups[off + p];
        if (group_label != 0) mark(global_group_id(tile_index, group_label),
                                   bits);
      }
    }
  }

  void match_io(std::int64_t a_idx,
                std::int64_t b_idx,
                const campaign::TileOp& a,
                const campaign::TileOp& b) {
    if ((a.tile_flags & campaign::OVERFLOW_BIT) ||
        (b.tile_flags & campaign::OVERFLOW_BIT)) {
      return;
    }
    const int face_o = static_cast<int>(campaign::Face::O);
    const int face_i = static_cast<int>(campaign::Face::I);
    ASSERT_EQ(a.n[face_o], b.n[face_i]);
    const int a_off = campaign::face_offset(a, campaign::Face::O);
    const int b_off = campaign::face_offset(b, campaign::Face::I);
    for (int p = 0; p < a.n[face_o]; ++p) {
      unite(global_group_id(a_idx, a.face_groups[a_off + p]),
            global_group_id(b_idx, b.face_groups[b_off + p]));
    }
  }

  void match_lr(std::int64_t a_idx,
                std::int64_t b_idx,
                const campaign::TileOp& a,
                const campaign::TileOp& b) {
    if ((a.tile_flags & campaign::OVERFLOW_BIT) ||
        (b.tile_flags & campaign::OVERFLOW_BIT)) {
      return;
    }
    const int face_r = static_cast<int>(campaign::Face::R);
    const int face_l = static_cast<int>(campaign::Face::L);
    ASSERT_EQ(a.n[face_r], b.n[face_l]);
    const int a_off = campaign::face_offset(a, campaign::Face::R);
    const int b_off = campaign::face_offset(b, campaign::Face::L);
    for (int p = 0; p < a.n[face_r]; ++p) {
      unite(global_group_id(a_idx, a.face_groups[a_off + p]),
            global_group_id(b_idx, b.face_groups[b_off + p]));
    }
  }
};

campaign::Verdict run_full(
    const campaign::Grid& grid,
    const std::vector<std::vector<campaign::TileOp>>& columns) {
  campaign::Compositor compositor;
  compositor.init(grid);
  for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
    const auto& column = columns[static_cast<std::size_t>(i - grid.i_min)];
    compositor.ingest_column(i, column.data());
  }
  return compositor.finalize();
}

campaign::Verdict run_streaming(
    const campaign::Grid& grid,
    const std::vector<std::vector<campaign::TileOp>>& columns) {
  campaign::StreamingCompositor compositor;
  compositor.init(grid);
  for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
    const auto& column = columns[static_cast<std::size_t>(i - grid.i_min)];
    compositor.ingest_column(i, column);
  }
  return compositor.finalize();
}

}  // namespace

TEST(StreamingCompositor, VerdictParityWithFullCompositor) {
  const campaign::Grid grid = make_grid({{0, 1}, {0, 1}, {0, 1}});
  const std::vector<std::vector<campaign::TileOp>> columns{
      {
          make_tile({}, {2}, {}, {1}, {1}, {}),
          make_tile({3}, {}, {}, {4}, {}, {}),
      },
      {
          make_tile({}, {}, {5}, {5}, {}, {}),
          make_tile({}, {}, {7}, {7}, {}, {}),
      },
      {
          make_tile({}, {}, {9}, {9}, {}, {9}),
          make_tile({}, {}, {10}, {10}, {}, {}),
      },
  };

  EXPECT_EQ(run_streaming(grid, columns), run_full(grid, columns));
  EXPECT_EQ(run_streaming(grid, columns), campaign::Verdict::kSpanning);
}

TEST(StreamingCompositor, RejectsSparseExplicitGrid) {
  campaign::Grid grid = make_grid({{0, 0}});
  grid.explicit_tiles.push_back(campaign::TileCoord{0, 0, 0, 0});

  campaign::StreamingCompositor compositor;
  EXPECT_THROW(compositor.init(grid), std::invalid_argument);
}

TEST(StreamingCompositor, InitDoesNotAllocateFromHugeTotalTileCount) {
  campaign::Grid grid = make_grid({{0, 0}});
  grid.total_tiles = 1'100'000'000LL;

  const std::vector<campaign::TileOp> column{
      make_tile({}, {}, {}, {}, {}, {})};

  campaign::StreamingCompositor compositor;
  EXPECT_NO_THROW(compositor.init(grid));
  compositor.ingest_column(0, column);
  EXPECT_TRUE(compositor.project_frontier().empty());
  EXPECT_EQ(compositor.finalize(), campaign::Verdict::kMoat);
}

TEST(StreamingCompositor, RequiresAscendingCompleteColumns) {
  const campaign::Grid grid = make_grid({{0, 0}, {0, 0}});
  const std::vector<std::vector<campaign::TileOp>> columns{
      {make_tile({}, {}, {}, {}, {}, {})},
      {make_tile({}, {}, {}, {}, {}, {})},
  };

  campaign::StreamingCompositor compositor;
  compositor.init(grid);

  EXPECT_THROW(compositor.ingest_column(1, columns[1]),
               std::invalid_argument);
}

TEST(StreamingCompositor, SpanningFinalizeDoesNotRequireRemainingColumns) {
  const campaign::Grid grid = make_grid({{0, 0}, {0, 0}});
  const std::vector<std::vector<campaign::TileOp>> columns{
      {make_tile({}, {}, {}, {1}, {1}, {1})},
      {make_tile({}, {}, {2}, {}, {}, {})},
  };

  campaign::StreamingCompositor compositor;
  compositor.init(grid);
  compositor.ingest_column(0, columns[0]);

  EXPECT_TRUE(compositor.has_spanning());
  EXPECT_EQ(compositor.finalize(), campaign::Verdict::kSpanning);

  compositor.ingest_column(1, columns[1]);
  EXPECT_TRUE(compositor.has_spanning());
  EXPECT_EQ(compositor.finalize(), campaign::Verdict::kSpanning);
}

TEST(StreamingCompositor, ProjectedFrontierMatchesFullProjectionAfterEachColumn) {
  const campaign::Grid grid = make_grid({{0, 1}, {0, 1}, {0, 1}});
  const std::vector<std::vector<campaign::TileOp>> columns{
      {
          make_tile({}, {2}, {}, {1}, {1}, {}),
          make_tile({3}, {}, {}, {4}, {}, {}),
      },
      {
          make_tile({}, {}, {5}, {5}, {}, {}),
          make_tile({}, {}, {7}, {7}, {}, {}),
      },
      {
          make_tile({}, {}, {9}, {9}, {}, {9}),
          make_tile({}, {}, {10}, {10}, {}, {}),
      },
  };

  campaign::StreamingCompositor streaming;
  streaming.init(grid);
  FrontierOracle oracle(grid);

  for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
    const auto& column = columns[static_cast<std::size_t>(i - grid.i_min)];
    streaming.ingest_column(i, column);
    oracle.ingest_column(i, column);
    EXPECT_EQ(streaming.project_frontier(), oracle.project_frontier());
  }
}

TEST(StreamingCompositor, EmptyColumnGapClearsFrontierAndDoesNotStitchAcrossGap) {
  const campaign::Grid grid = make_grid({{0, 0}, {1, 0}, {0, 0}});
  const std::vector<std::vector<campaign::TileOp>> columns{
      {make_tile({}, {}, {}, {1}, {1}, {})},
      {},
      {make_tile({}, {}, {2}, {}, {}, {2})},
  };

  campaign::StreamingCompositor compositor;
  compositor.init(grid);
  compositor.ingest_column(0, columns[0]);
  EXPECT_FALSE(compositor.project_frontier().empty());
  compositor.ingest_column(1, columns[1]);
  EXPECT_TRUE(compositor.project_frontier().empty());
  compositor.ingest_column(2, columns[2]);

  EXPECT_FALSE(compositor.has_spanning());
  EXPECT_EQ(compositor.finalize(), campaign::Verdict::kMoat);
}

TEST(StreamingCompositor, OverflowConservativelyLatchesSpanning) {
  const campaign::Grid grid = make_grid({{0, 0}, {0, 0}});
  const std::vector<campaign::TileOp> first{
      make_tile({}, {}, {}, {}, {}, {}, campaign::OVERFLOW_BIT)};

  campaign::StreamingCompositor compositor;
  compositor.init(grid);
  compositor.ingest_column(0, first);

  EXPECT_TRUE(compositor.has_spanning());
  EXPECT_EQ(compositor.finalize(), campaign::Verdict::kSpanning);
}

TEST(StreamingCompositor, MoatFinalizeRequiresAllColumns) {
  const campaign::Grid grid = make_grid({{0, 0}, {0, 0}});
  const std::vector<campaign::TileOp> first{
      make_tile({}, {}, {}, {}, {}, {})};

  campaign::StreamingCompositor compositor;
  compositor.init(grid);
  compositor.ingest_column(0, first);

  EXPECT_THROW(compositor.finalize(), std::runtime_error);
}
