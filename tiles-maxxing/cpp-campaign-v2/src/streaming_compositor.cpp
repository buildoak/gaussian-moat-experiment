// src/streaming_compositor.cpp
//
// Streaming compositor for column-complete ingestion. Group nodes are allocated
// lazily for the current live column state, then compacted to the right-face
// frontier after each column. This keeps memory proportional to live frontier
// and current-column work, not to the full grid.

#include "campaign/streaming_compositor.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
  if (coord.i == grid.i_min && face == Face::L) {
    throw std::runtime_error(
        "side-exposure violation: face_L at i_min entered L-R stitching");
  }
  if (coord.i == grid.i_max && face == Face::R) {
    throw std::runtime_error(
        "side-exposure violation: face_R at i_max entered L-R stitching");
  }
}

}  // namespace

struct StreamingCompositor::State {
  static constexpr std::uint32_t kGroupsPerTile = 128;
  static constexpr std::uint8_t kReachInner = 0x1;
  static constexpr std::uint8_t kReachOuter = 0x2;
  static constexpr std::uint8_t kReachBoth = kReachInner | kReachOuter;
  using GroupKey = std::uint64_t;
  using NodeId = std::uint32_t;

  struct FrontierNode {
    std::int32_t j = 0;
    std::uint8_t port_ordinal = 0;
    NodeId node = 0;
  };

  Grid grid;
  bool initialized = false;
  bool spanning_detected = false;
  std::int32_t next_i = 0;
  std::int32_t last_frontier_i = 0;
  bool has_frontier_column = false;
  std::unordered_map<GroupKey, NodeId> group_to_node;
  std::vector<NodeId> parent;
  std::vector<std::uint8_t> reach;
  std::vector<TileOp> prev_column;
  std::vector<TileCoord> prev_coords;
  std::vector<FrontierNode> frontier;

  NodeId make_node() {
    if (parent.size() > std::numeric_limits<NodeId>::max()) {
      throw std::runtime_error("StreamingCompositor live DSU node cap exceeded");
    }
    const NodeId id = static_cast<NodeId>(parent.size());
    parent.push_back(id);
    reach.push_back(0);
    return id;
  }

  NodeId find(NodeId x) {
    NodeId root = x;
    while (parent[root] != root) root = parent[root];
    while (parent[x] != x) {
      const NodeId next = parent[x];
      parent[x] = root;
      x = next;
    }
    return root;
  }

  NodeId find_const(NodeId x) const {
    while (parent[x] != x) x = parent[x];
    return x;
  }

  NodeId unite(NodeId a, NodeId b) {
    NodeId ra = find(a);
    NodeId rb = find(b);
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

  void mark(NodeId node, std::uint8_t bits) {
    const NodeId root = find(node);
    reach[root] = static_cast<std::uint8_t>(reach[root] | bits);
    latch_if_spanning(root);
  }

  void latch_if_spanning(NodeId root) noexcept {
    if ((reach[root] & kReachBoth) == kReachBoth) {
      spanning_detected = true;
    }
  }

  GroupKey global_group_id(std::int64_t tile_index, int group_label) const {
    if (group_label == 0) {
      throw std::runtime_error(
          "TileOp group label is 0 (reserved zero-sentinel) in active port");
    }
    if (group_label < 0 ||
        group_label > static_cast<int>(kGroupsPerTile)) {
      throw std::runtime_error("TileOp group label outside 1..128");
    }
    if (tile_index < 0) {
      throw std::runtime_error("negative tile index in active port");
    }
    const std::uint64_t tile = static_cast<std::uint64_t>(tile_index);
    if (tile > (std::numeric_limits<GroupKey>::max() -
                static_cast<std::uint64_t>(group_label - 1)) /
                   kGroupsPerTile) {
      throw std::runtime_error("global group id exceeds uint64_t");
    }
    return tile * kGroupsPerTile +
           static_cast<std::uint64_t>(group_label - 1);
  }

  NodeId node_for_group(std::int64_t tile_index, int group_label) {
    const GroupKey gid = global_group_id(tile_index, group_label);
    auto it = group_to_node.find(gid);
    if (it != group_to_node.end()) return it->second;
    const NodeId node = make_node();
    group_to_node.emplace(gid, node);
    return node;
  }

  std::int64_t checked_tile_index(std::int32_t i, std::int32_t j) const {
    const std::int64_t idx = grid.flat_index(i, j);
    if (idx < 0) {
      throw std::runtime_error("tile coordinate is not active in grid");
    }
    return idx;
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
          mark(node_for_group(tile_index, group_label), bits);
        }
      }
    }
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
        const NodeId node = node_for_group(tile_index, group_label);
        const std::uint8_t bits = reach_for_group(op, group_label);
        if (bits != 0) mark(node, bits);
      }
    }
  }

  void match_io(std::int64_t a_idx,
                std::int64_t b_idx,
                const TileOp& a,
                const TileOp& b) {
    if ((a.tile_flags & OVERFLOW_BIT) || (b.tile_flags & OVERFLOW_BIT)) {
      return;
    }
    const int face_o = face_index(Face::O);
    const int face_i = face_index(Face::I);
    require_port_count_equal(a.n[face_o], b.n[face_i], "I/O");
    const int a_off = face_offset(a, Face::O);
    const int b_off = face_offset(b, Face::I);
    for (int p = 0; p < a.n[face_o]; ++p) {
      unite(node_for_group(a_idx, a.face_groups[a_off + p]),
            node_for_group(b_idx, b.face_groups[b_off + p]));
    }
  }

  NodeId frontier_node_for(std::int32_t j, std::uint8_t port_ordinal) const {
    for (const FrontierNode& entry : frontier) {
      if (entry.j == j && entry.port_ordinal == port_ordinal) {
        return entry.node;
      }
    }
    throw std::runtime_error("previous right-face frontier node missing");
  }

  void match_lr(const TileCoord& a_coord,
                const TileCoord& b_coord,
                std::int64_t b_idx,
                const TileOp& a,
                const TileOp& b) {
    assert_not_side_exposed_lr_input(grid, a_coord, Face::R);
    assert_not_side_exposed_lr_input(grid, b_coord, Face::L);
    if ((a.tile_flags & OVERFLOW_BIT) || (b.tile_flags & OVERFLOW_BIT)) {
      return;
    }
    const int face_r = face_index(Face::R);
    const int face_l = face_index(Face::L);
    require_port_count_equal(a.n[face_r], b.n[face_l], "L/R");
    const int b_off = face_offset(b, Face::L);
    for (int p = 0; p < a.n[face_r]; ++p) {
      unite(frontier_node_for(a_coord.j, static_cast<std::uint8_t>(p + 1)),
            node_for_group(b_idx, b.face_groups[b_off + p]));
    }
  }

  std::vector<FrontierNode> build_current_frontier(
      const std::vector<TileCoord>& current_coords,
      const std::vector<TileOp>& current_column) {
    std::vector<FrontierNode> out;
    for (std::size_t local = 0; local < current_coords.size(); ++local) {
      const TileCoord& coord = current_coords[local];
      const TileOp& op = current_column[local];
      if (op.tile_flags & OVERFLOW_BIT) continue;
      const std::int64_t idx = checked_tile_index(coord.i, coord.j);
      const int face_r = face_index(Face::R);
      const int off = face_offset(op, Face::R);
      for (int p = 0; p < op.n[face_r]; ++p) {
        out.push_back(FrontierNode{
            coord.j,
            static_cast<std::uint8_t>(p + 1),
            node_for_group(idx, op.face_groups[off + p]),
        });
      }
    }
    return out;
  }

  void compact_to_frontier(std::vector<FrontierNode> next_frontier) {
    if (next_frontier.empty()) {
      group_to_node.clear();
      parent.clear();
      reach.clear();
      frontier.clear();
      return;
    }

    std::map<NodeId, NodeId> root_to_compact;
    std::vector<NodeId> compact_parent;
    std::vector<std::uint8_t> compact_reach;

    for (FrontierNode& entry : next_frontier) {
      const NodeId root = find(entry.node);
      if (compact_parent.size() > std::numeric_limits<NodeId>::max()) {
        throw std::runtime_error(
            "StreamingCompositor compact frontier node cap exceeded");
      }
      auto [it, inserted] = root_to_compact.emplace(
          root, static_cast<NodeId>(compact_parent.size()));
      if (inserted) {
        compact_parent.push_back(it->second);
        compact_reach.push_back(reach[root]);
      }
      entry.node = it->second;
    }

    group_to_node.clear();
    parent = std::move(compact_parent);
    reach = std::move(compact_reach);
    frontier = std::move(next_frontier);
  }

  const TileOp* prev_op_for_j(std::int32_t j) const {
    for (std::size_t pos = 0; pos < prev_coords.size(); ++pos) {
      if (prev_coords[pos].j == j) return &prev_column[pos];
    }
    return nullptr;
  }

  TileCoord prev_coord_for_j(std::int32_t j) const {
    for (const TileCoord& coord : prev_coords) {
      if (coord.j == j) return coord;
    }
    throw std::runtime_error("previous frontier coordinate missing");
  }
};

StreamingCompositor::StreamingCompositor() : state_(new State{}) {}
StreamingCompositor::~StreamingCompositor() { delete state_; }

void StreamingCompositor::init(const Grid& grid) {
  if (grid.is_sparse()) {
    throw std::invalid_argument(
        "StreamingCompositor does not support sparse explicit grids");
  }
  if (grid.total_tiles < 0) {
    throw std::invalid_argument("grid.total_tiles must be non-negative");
  }

  State fresh;
  fresh.grid = grid;
  fresh.initialized = true;
  fresh.next_i = grid.i_min;
  *state_ = std::move(fresh);
}

void StreamingCompositor::ingest_column(
    std::int32_t i,
    std::span<const TileOp> column_tileops) {
  if (!state_ || !state_->initialized) {
    throw std::runtime_error("StreamingCompositor::init must be called first");
  }
  if (!state_->grid.has_column(i)) {
    throw std::invalid_argument("column is outside grid");
  }
  if (i != state_->next_i) {
    throw std::invalid_argument(
        "StreamingCompositor requires i-ascending column ingestion");
  }

  const auto column_tiles = state_->grid.enumerate_column_tiles(i);
  if (column_tileops.size() != column_tiles.size()) {
    throw std::invalid_argument("column_tileops size does not match grid");
  }

  std::vector<TileOp> current_column(column_tileops.begin(),
                                     column_tileops.end());
  std::vector<TileCoord> current_coords = column_tiles;

  for (std::size_t local = 0; local < current_coords.size(); ++local) {
    const TileCoord& coord = current_coords[local];
    const std::int64_t idx = state_->checked_tile_index(coord.i, coord.j);
    const TileOp& op = current_column[local];
    state_->mark_tile_ports(idx, op);

    if (state_->grid.flat_index(i, coord.j - 1) >= 0) {
      const std::int64_t below_idx =
          state_->checked_tile_index(i, coord.j - 1);
      state_->match_io(below_idx, idx, current_column[local - 1], op);
    }
  }

  if (!state_->prev_coords.empty() &&
      state_->last_frontier_i == static_cast<std::int32_t>(i - 1)) {
    for (std::size_t local = 0; local < current_coords.size(); ++local) {
      const TileCoord& coord = current_coords[local];
      const TileOp* left_op = state_->prev_op_for_j(coord.j);
      if (!left_op) continue;
      const TileCoord left_coord = state_->prev_coord_for_j(coord.j);
      const std::int64_t idx = state_->checked_tile_index(coord.i, coord.j);
      state_->match_lr(left_coord, coord, idx, *left_op, current_column[local]);
    }
  }

  std::vector<State::FrontierNode> next_frontier =
      state_->build_current_frontier(current_coords, current_column);
  state_->compact_to_frontier(std::move(next_frontier));

  state_->prev_column = std::move(current_column);
  state_->prev_coords = std::move(current_coords);
  state_->last_frontier_i = i;
  state_->has_frontier_column = !state_->frontier.empty();
  state_->next_i = static_cast<std::int32_t>(i + 1);
}

bool StreamingCompositor::has_spanning() const noexcept {
  return state_ ? state_->spanning_detected : false;
}

Verdict StreamingCompositor::finalize() {
  if (!state_ || !state_->initialized) return Verdict::kUnknown;
  if (state_->spanning_detected) return Verdict::kSpanning;
  if (state_->next_i <= state_->grid.i_max) {
    throw std::runtime_error(
        "StreamingCompositor::finalize requires all columns for MOAT");
  }
  return Verdict::kMoat;
}

std::vector<FrontierPort> StreamingCompositor::project_frontier() const {
  if (!state_ || !state_->initialized || !state_->has_frontier_column) {
    return {};
  }

  std::vector<FrontierPort> out;
  std::map<State::NodeId, std::uint32_t> canonical;
  std::uint32_t next_component = 1;

  for (const State::FrontierNode& entry : state_->frontier) {
    const State::NodeId root = state_->find_const(entry.node);
    auto [it, inserted] = canonical.emplace(root, next_component);
    if (inserted) ++next_component;
    out.push_back(FrontierPort{
        entry.j,
        entry.port_ordinal,
        it->second,
        state_->reach[root],
    });
  }
  return out;
}

}  // namespace campaign
