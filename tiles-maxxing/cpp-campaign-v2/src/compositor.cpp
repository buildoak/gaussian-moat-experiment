// src/compositor.cpp
//
// Cross-tile TileOp compositor. The global DSU is over per-tile UF group
// labels; face ports stitch by positional ordinal and union the group labels
// carried at that ordinal.

#include "campaign/compositor.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "campaign/constants.h"

namespace campaign {

namespace {

inline int face_index(Face face) noexcept {
  return static_cast<int>(face);
}

std::uint8_t reach_for_group(const TileOp& op, int group_label) noexcept {
  std::uint8_t reach = 0;
  if (bit_test(op.inner_flags, group_label)) reach |= 0x1;
  if (bit_test(op.outer_flags, group_label)) reach |= 0x2;
  return reach;
}

void require_port_count_equal(std::uint8_t a_count,
                              std::uint8_t b_count,
                              const char* edge) {
  if (a_count != b_count) {
#ifndef NDEBUG
    assert(a_count == b_count && "Lemma 4 port-count mismatch");
#endif
    throw std::runtime_error(std::string("Lemma 4 port-count mismatch on ") +
                             edge);
  }
}

void assert_not_side_exposed_lr_input(const Grid& grid,
                                      const TileCoord& coord,
                                      Face face) {
  // Theorem 12 supplies reflection closure for side-exposed octant faces.
  assert(!(coord.i == grid.i_min && face == Face::L));
  assert(!(coord.i == grid.i_max && face == Face::R));
}

}  // namespace

struct Compositor::State {
  static constexpr std::uint32_t kGroupsPerTile = 128;
  static constexpr std::uint8_t kReachInner = 0x1;
  static constexpr std::uint8_t kReachOuter = 0x2;
  static constexpr std::uint8_t kReachBoth = kReachInner | kReachOuter;

  Grid grid;
  bool initialized = false;
  bool spanning_detected = false;
  std::vector<std::uint32_t> parent;
  std::vector<std::uint8_t> reach;
  std::vector<TileOp> tileops;
  std::vector<std::uint8_t> ingested;

  std::uint32_t find(std::uint32_t x) {
    std::uint32_t root = x;
    while (parent[root] != root) root = parent[root];
    while (parent[x] != x) {
      const std::uint32_t next = parent[x];
      parent[x] = root;
      x = next;
    }
    return root;
  }

  std::uint32_t unite(std::uint32_t a, std::uint32_t b) {
    std::uint32_t ra = find(a);
    std::uint32_t rb = find(b);
    if (ra == rb) {
      latch_if_spanning(ra);
      return ra;
    }
    if (rb < ra) std::swap(ra, rb);
    parent[rb] = ra;
    reach[ra] = static_cast<std::uint8_t>(reach[ra] | reach[rb]);
    latch_if_spanning(ra);
    return ra;
  }

  void mark(std::uint32_t gid, std::uint8_t bits) {
    const std::uint32_t root = find(gid);
    reach[root] = static_cast<std::uint8_t>(reach[root] | bits);
    latch_if_spanning(root);
  }

  void latch_if_spanning(std::uint32_t root) noexcept {
    if ((reach[root] & kReachBoth) == kReachBoth) {
      spanning_detected = true;
    }
  }

  std::uint32_t global_group_id(std::int64_t tile_index,
                                int group_label) const {
    if (group_label < 1 ||
        group_label > static_cast<int>(kGroupsPerTile)) {
      throw std::runtime_error("TileOp group label outside 1..128");
    }
    const std::uint64_t tile = static_cast<std::uint64_t>(tile_index);
    const std::uint64_t gid =
        tile * kGroupsPerTile + static_cast<std::uint64_t>(group_label - 1);
    if (gid > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("global group id exceeds uint32_t");
    }
    return static_cast<std::uint32_t>(gid);
  }

  std::int64_t checked_tile_index(std::int32_t i, std::int32_t j) const {
    const std::int64_t idx = grid.flat_index(i, j);
    if (idx < 0) {
      throw std::runtime_error("tile coordinate is not active in grid");
    }
    return idx;
  }

  void mark_tile_ports(std::int64_t tile_index, const TileOp& op) {
    if (op.tile_flags & OVERFLOW_BIT) {
      spanning_detected = true;
      mark_all_present_ports(tile_index, op, kReachBoth);
      return;
    }
    if (op.tile_flags & EMPTY_BIT) return;

    for (int f = 0; f < NUM_FACES; ++f) {
      const Face face = static_cast<Face>(f);
      const int off = face_offset(op, face);
      for (int p = 0; p < op.n[f]; ++p) {
        const int group_label = op.face_groups[off + p];
        (void)global_group_id(tile_index, group_label);
        const std::uint8_t bits = reach_for_group(op, group_label);
        if (bits != 0) mark(global_group_id(tile_index, group_label), bits);
      }
    }
  }

  void mark_all_present_ports(std::int64_t tile_index,
                              const TileOp& op,
                              std::uint8_t bits) {
    for (int f = 0; f < NUM_FACES; ++f) {
      const Face face = static_cast<Face>(f);
      const int off = face_offset(op, face);
      for (int p = 0; p < op.n[f]; ++p) {
        const int group_label = op.face_groups[off + p];
        if (group_label != 0) {
          mark(global_group_id(tile_index, group_label), bits);
        }
      }
    }
  }

  void match_io(std::int64_t a_idx,
                std::int64_t b_idx,
                const TileOp& a,
                const TileOp& b) {
    const int face_o = face_index(Face::O);
    const int face_i = face_index(Face::I);
    require_port_count_equal(a.n[face_o], b.n[face_i], "I/O");
    const int a_off = face_offset(a, Face::O);
    const int b_off = face_offset(b, Face::I);
    for (int p = 0; p < a.n[face_o]; ++p) {
      unite(global_group_id(a_idx, a.face_groups[a_off + p]),
            global_group_id(b_idx, b.face_groups[b_off + p]));
    }
  }

  void match_lr(const TileCoord& a_coord,
                const TileCoord& b_coord,
                std::int64_t a_idx,
                std::int64_t b_idx,
                const TileOp& a,
                const TileOp& b) {
    assert_not_side_exposed_lr_input(grid, a_coord, Face::R);
    assert_not_side_exposed_lr_input(grid, b_coord, Face::L);

    const int face_r = face_index(Face::R);
    const int face_l = face_index(Face::L);
    require_port_count_equal(a.n[face_r], b.n[face_l], "L/R");
    const int a_off = face_offset(a, Face::R);
    const int b_off = face_offset(b, Face::L);
    for (int p = 0; p < a.n[face_r]; ++p) {
      unite(global_group_id(a_idx, a.face_groups[a_off + p]),
            global_group_id(b_idx, b.face_groups[b_off + p]));
    }
  }
};

Compositor::Compositor() : state_(new State{}) {}
Compositor::~Compositor() { delete state_; }

void Compositor::init(const Grid& grid) {
  if (grid.total_tiles < 0) {
    throw std::invalid_argument("grid.total_tiles must be non-negative");
  }
  const std::uint64_t total_tiles =
      static_cast<std::uint64_t>(grid.total_tiles);
  const std::uint64_t total_groups =
      total_tiles * State::kGroupsPerTile;
  if (total_groups > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("grid exceeds compositor global-group cap");
  }

  State fresh;
  fresh.grid = grid;
  fresh.initialized = true;
  fresh.parent.resize(static_cast<std::size_t>(total_groups));
  fresh.reach.assign(static_cast<std::size_t>(total_groups), 0);
  fresh.tileops.resize(static_cast<std::size_t>(total_tiles));
  fresh.ingested.assign(static_cast<std::size_t>(total_tiles), 0);
  for (std::uint32_t gid = 0; gid < total_groups; ++gid) {
    fresh.parent[gid] = gid;
  }
  *state_ = std::move(fresh);
}

void Compositor::ingest_column(std::int32_t i,
                               const TileOp* column_tileops) {
  if (!state_ || !state_->initialized) {
    throw std::runtime_error("Compositor::init must be called first");
  }
  if (!column_tileops) {
    throw std::invalid_argument("column_tileops must not be null");
  }
  if (!state_->grid.has_column(i)) {
    throw std::invalid_argument("column is outside grid");
  }

  const auto column_tiles = state_->grid.enumerate_column_tiles(i);
  for (std::size_t local = 0; local < column_tiles.size(); ++local) {
    const TileCoord& coord = column_tiles[local];
    const std::int32_t j = coord.j;
    const std::int64_t idx = state_->checked_tile_index(i, j);
    const TileOp& op = column_tileops[local];
    state_->tileops[static_cast<std::size_t>(idx)] = op;
    state_->ingested[static_cast<std::size_t>(idx)] = 1;
    state_->mark_tile_ports(idx, op);

    if (state_->grid.flat_index(i, j - 1) >= 0) {
      const std::int64_t below_idx = state_->checked_tile_index(i, j - 1);
      state_->match_io(below_idx, idx,
                       state_->tileops[static_cast<std::size_t>(below_idx)],
                       op);
    }
  }

  const std::int32_t left_i = i - 1;
  if (state_->grid.has_column(left_i)) {
    for (const TileCoord& current : column_tiles) {
      const std::int32_t j = current.j;
      if (state_->grid.flat_index(left_i, j) < 0) continue;
      const std::int64_t left_idx = state_->checked_tile_index(left_i, j);
      if (!state_->ingested[static_cast<std::size_t>(left_idx)]) continue;
      const std::int64_t idx = state_->checked_tile_index(i, j);
      const TileCoord left_coord{
          left_i,
          j,
          static_cast<std::int64_t>(state_->grid.o_x) +
              static_cast<std::int64_t>(state_->grid.S_value) * left_i,
          static_cast<std::int64_t>(state_->grid.o_y) +
              static_cast<std::int64_t>(state_->grid.S_value) * j,
      };
      const TileCoord coord{
          i,
          j,
          static_cast<std::int64_t>(state_->grid.o_x) +
              static_cast<std::int64_t>(state_->grid.S_value) * i,
          static_cast<std::int64_t>(state_->grid.o_y) +
              static_cast<std::int64_t>(state_->grid.S_value) * j,
      };
      state_->match_lr(left_coord, coord, left_idx, idx,
                       state_->tileops[static_cast<std::size_t>(left_idx)],
                       state_->tileops[static_cast<std::size_t>(idx)]);
    }
  }
}

bool Compositor::has_spanning() const noexcept {
  return state_ ? state_->spanning_detected : false;
}

Verdict Compositor::finalize() {
  if (!state_ || !state_->initialized) return Verdict::kUnknown;
  return state_->spanning_detected ? Verdict::kSpanning : Verdict::kMoat;
}

}  // namespace campaign
