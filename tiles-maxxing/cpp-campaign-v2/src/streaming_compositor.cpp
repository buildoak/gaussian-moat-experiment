// src/streaming_compositor.cpp
//
// Streaming compositor for column-complete ingestion. Group nodes are allocated
// lazily for the current live column state, then compacted to the right-face
// frontier after each column. This keeps memory proportional to live frontier
// and current-column work, not to the full grid.

#include "campaign/streaming_compositor.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
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
  using NodeId = std::uint32_t;
  static constexpr NodeId kInvalidNode = std::numeric_limits<NodeId>::max();

  struct FrontierNode {
    std::int32_t j = 0;
    std::uint8_t port_ordinal = 0;
    NodeId node = 0;
    std::int64_t tile_index = -1;
    std::int32_t group_label = 0;
  };

  struct ReachSource {
    std::int64_t tile_index = -1;
    std::int32_t group_label = 0;
  };

  Grid grid;
  bool initialized = false;
  bool spanning_detected = false;
  SpanningTrace trace;
  std::int32_t next_i = 0;
  std::int32_t last_frontier_i = 0;
  bool has_frontier_column = false;
  std::int64_t current_group_base_index = 0;
  std::size_t current_group_tile_count = 0;
  std::vector<NodeId> current_group_nodes;
  std::vector<NodeId> parent;
  std::vector<std::uint8_t> reach;
  std::vector<std::array<ReachSource, 2>> reach_sources;
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
    reach_sources.push_back({});
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

  TileCoord coord_from_flat_index(std::int64_t tile_index) const {
    if (tile_index < 0 || tile_index >= grid.total_tiles) {
      return TileCoord{};
    }
    for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
      const std::size_t k = static_cast<std::size_t>(i - grid.i_min);
      if (k + 1 >= grid.tower_offset.size()) break;
      const std::int64_t begin = grid.tower_offset[k];
      const std::int64_t end = grid.tower_offset[k + 1];
      if (tile_index < begin || tile_index >= end) continue;
      const std::int32_t j =
          static_cast<std::int32_t>(grid.j_low[k] + (tile_index - begin));
      return TileCoord{
          i,
          j,
          static_cast<std::int64_t>(grid.o_x) +
              static_cast<std::int64_t>(grid.S_value) * i,
          static_cast<std::int64_t>(grid.o_y) +
              static_cast<std::int64_t>(grid.S_value) * j,
      };
    }
    return TileCoord{};
  }

  void record_spanning(const char* event,
                       NodeId root,
                       std::uint8_t reach_before,
                       std::uint8_t reach_after,
                       std::uint8_t added_bits,
                       std::int64_t tile_index = -1,
                       std::int32_t group_label = 0,
                       std::int64_t lhs_tile_index = -1,
                       std::int32_t lhs_group_label = 0,
                       std::int64_t rhs_tile_index = -1,
                       std::int32_t rhs_group_label = 0) {
    if (trace.detected || (reach_after & kReachBoth) != kReachBoth) return;
    trace.detected = true;
    trace.event = event;
    trace.column_i = next_i;
    trace.tile_index = tile_index;
    trace.group_label = group_label;
    trace.lhs_tile_index = lhs_tile_index;
    trace.lhs_group_label = lhs_group_label;
    trace.rhs_tile_index = rhs_tile_index;
    trace.rhs_group_label = rhs_group_label;
    trace.component = root;
    trace.reach_before = reach_before;
    trace.reach_after = reach_after;
    trace.added_bits = added_bits;
    if (root < reach_sources.size()) {
      trace.inner_source_tile_index = reach_sources[root][0].tile_index;
      trace.inner_source_group_label = reach_sources[root][0].group_label;
      trace.outer_source_tile_index = reach_sources[root][1].tile_index;
      trace.outer_source_group_label = reach_sources[root][1].group_label;
    }
    if (tile_index >= 0) {
      const TileCoord coord = coord_from_flat_index(tile_index);
      trace.column_i = coord.i;
      trace.tile_j = coord.j;
    } else if (rhs_tile_index >= 0) {
      const TileCoord coord = coord_from_flat_index(rhs_tile_index);
      trace.column_i = coord.i;
      trace.tile_j = coord.j;
    } else if (lhs_tile_index >= 0) {
      const TileCoord coord = coord_from_flat_index(lhs_tile_index);
      trace.column_i = coord.i;
      trace.tile_j = coord.j;
    }
  }

  NodeId unite(NodeId a,
               NodeId b,
               const char* event = "unite",
               std::int64_t lhs_tile_index = -1,
               std::int32_t lhs_group_label = 0,
               std::int64_t rhs_tile_index = -1,
               std::int32_t rhs_group_label = 0) {
    NodeId ra = find(a);
    NodeId rb = find(b);
    if (ra == rb) {
      const std::uint8_t before = reach[ra];
      record_spanning(event, ra, before, before, 0, -1, 0,
                      lhs_tile_index, lhs_group_label, rhs_tile_index,
                      rhs_group_label);
      latch_if_spanning(ra);
      return ra;
    }
    if (rb < ra) std::swap(ra, rb);
    const std::uint8_t before = reach[ra];
    const std::uint8_t added = reach[rb];
    parent[rb] = ra;
    reach[ra] = static_cast<std::uint8_t>(before | added);
    for (int bit = 0; bit < 2; ++bit) {
      const std::uint8_t mask = static_cast<std::uint8_t>(1U << bit);
      if ((reach[rb] & mask) == 0) continue;
      if (reach_sources[ra][bit].tile_index >= 0) continue;
      reach_sources[ra][bit] = reach_sources[rb][bit];
    }
    record_spanning(event, ra, before, reach[ra], added, -1, 0,
                    lhs_tile_index, lhs_group_label, rhs_tile_index,
                    rhs_group_label);
    latch_if_spanning(ra);
    return ra;
  }

  void mark(NodeId node,
            std::uint8_t bits,
            std::int64_t tile_index = -1,
            std::int32_t group_label = 0,
            const char* event = "mark") {
    const NodeId root = find(node);
    const std::uint8_t before = reach[root];
    if ((bits & kReachInner) != 0 &&
        reach_sources[root][0].tile_index < 0) {
      reach_sources[root][0] = ReachSource{tile_index, group_label};
    }
    if ((bits & kReachOuter) != 0 &&
        reach_sources[root][1].tile_index < 0) {
      reach_sources[root][1] = ReachSource{tile_index, group_label};
    }
    reach[root] = static_cast<std::uint8_t>(reach[root] | bits);
    record_spanning(event, root, before, reach[root], bits, tile_index,
                    group_label);
    latch_if_spanning(root);
  }

  void latch_if_spanning(NodeId root) noexcept {
    if ((reach[root] & kReachBoth) == kReachBoth) {
      spanning_detected = true;
    }
  }

  void begin_current_column_groups(std::int32_t i, std::size_t tile_count) {
    current_group_nodes.clear();
    current_group_base_index = 0;
    current_group_tile_count = tile_count;
    if (tile_count == 0) return;

    if (tile_count >
        std::numeric_limits<std::size_t>::max() / kGroupsPerTile) {
      throw std::runtime_error("StreamingCompositor current group table too large");
    }
    const auto [jlo, jhi] = grid.column_bounds(i);
    if (jhi < jlo) {
      throw std::runtime_error("StreamingCompositor current column is empty");
    }
    current_group_base_index = checked_tile_index(i, jlo);
    current_group_nodes.assign(tile_count * kGroupsPerTile, kInvalidNode);
  }

  void validate_group_ref(std::int64_t tile_index, int group_label) const {
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
  }

  NodeId node_for_group(std::int64_t tile_index, int group_label) {
    validate_group_ref(tile_index, group_label);
    if (current_group_nodes.empty()) {
      throw std::runtime_error("current group table is not initialized");
    }
    if (tile_index < current_group_base_index) {
      throw std::runtime_error("current group tile precedes active column");
    }
    const auto local_tile =
        static_cast<std::uint64_t>(tile_index - current_group_base_index);
    if (local_tile >= current_group_tile_count) {
      throw std::runtime_error("current group tile exceeds active column");
    }
    const std::size_t slot =
        static_cast<std::size_t>(local_tile) * kGroupsPerTile +
        static_cast<std::size_t>(group_label - 1);
    NodeId& node = current_group_nodes[slot];
    if (node == kInvalidNode) {
      node = make_node();
    }
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
          mark(node_for_group(tile_index, group_label), bits, tile_index,
               group_label, "overflow_mark");
        }
      }
    }
  }

  void mark_tile_ports(std::int64_t tile_index, const TileOp& op) {
    if (op.tile_flags & OVERFLOW_BIT) {
      if (!trace.detected) {
        record_spanning("overflow", 0, 0, kReachBoth, kReachBoth, tile_index);
      }
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
        if (bits != 0) mark(node, bits, tile_index, group_label, "mark");
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
      const int a_group = a.face_groups[a_off + p];
      const int b_group = b.face_groups[b_off + p];
      unite(node_for_group(a_idx, a_group),
            node_for_group(b_idx, b_group),
            "bridge_io", a_idx, a_group, b_idx, b_group);
    }
  }

  static bool frontier_key_less(const FrontierNode& entry,
                                std::int32_t j,
                                std::uint8_t port_ordinal) noexcept {
    return entry.j < j ||
           (entry.j == j && entry.port_ordinal < port_ordinal);
  }

  const FrontierNode& frontier_entry_for(std::size_t& frontier_cursor,
                                         std::int32_t j,
                                         std::uint8_t port_ordinal) const {
    while (frontier_cursor < frontier.size() &&
           frontier_key_less(frontier[frontier_cursor], j, port_ordinal)) {
      ++frontier_cursor;
    }
    if (frontier_cursor < frontier.size()) {
      const FrontierNode& entry = frontier[frontier_cursor];
      if (entry.j == j && entry.port_ordinal == port_ordinal) {
        return entry;
      }
    }
    throw std::runtime_error("previous right-face frontier node missing");
  }

  void match_lr(const TileCoord& a_coord,
                const TileCoord& b_coord,
                std::int64_t b_idx,
                const TileOp& a,
                const TileOp& b,
                std::size_t& frontier_cursor) {
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
      const int b_group = b.face_groups[b_off + p];
      const FrontierNode& lhs = frontier_entry_for(
          frontier_cursor, a_coord.j, static_cast<std::uint8_t>(p + 1));
      unite(lhs.node,
            node_for_group(b_idx, b_group),
            "bridge_lr", lhs.tile_index, lhs.group_label, b_idx, b_group);
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
        const int group_label = op.face_groups[off + p];
        out.push_back(FrontierNode{
            coord.j,
            static_cast<std::uint8_t>(p + 1),
            node_for_group(idx, group_label),
            idx,
            group_label,
        });
      }
    }
    return out;
  }

  void compact_to_frontier(std::vector<FrontierNode> next_frontier) {
    if (next_frontier.empty()) {
      current_group_nodes.clear();
      current_group_tile_count = 0;
      parent.clear();
      reach.clear();
      reach_sources.clear();
      frontier.clear();
      return;
    }

    std::vector<NodeId> root_to_compact(parent.size(), kInvalidNode);
    std::vector<NodeId> compact_parent;
    std::vector<std::uint8_t> compact_reach;
    std::vector<std::array<ReachSource, 2>> compact_reach_sources;

    for (FrontierNode& entry : next_frontier) {
      const NodeId root = find(entry.node);
      if (root >= root_to_compact.size()) {
        throw std::runtime_error("StreamingCompositor root remap out of range");
      }
      if (compact_parent.size() > std::numeric_limits<NodeId>::max()) {
        throw std::runtime_error(
            "StreamingCompositor compact frontier node cap exceeded");
      }
      NodeId compact = root_to_compact[root];
      if (compact == kInvalidNode) {
        compact = static_cast<NodeId>(compact_parent.size());
        root_to_compact[root] = compact;
        compact_parent.push_back(compact);
        compact_reach.push_back(reach[root]);
        compact_reach_sources.push_back(reach_sources[root]);
      }
      entry.node = compact;
    }

    current_group_nodes.clear();
    current_group_tile_count = 0;
    parent = std::move(compact_parent);
    reach = std::move(compact_reach);
    reach_sources = std::move(compact_reach_sources);
    frontier = std::move(next_frontier);
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
  state_->begin_current_column_groups(i, current_coords.size());

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
    std::size_t prev_pos = 0;
    std::size_t frontier_cursor = 0;
    for (std::size_t local = 0; local < current_coords.size(); ++local) {
      const TileCoord& coord = current_coords[local];
      while (prev_pos < state_->prev_coords.size() &&
             state_->prev_coords[prev_pos].j < coord.j) {
        ++prev_pos;
      }
      if (prev_pos == state_->prev_coords.size() ||
          state_->prev_coords[prev_pos].j != coord.j) {
        continue;
      }
      const TileCoord& left_coord = state_->prev_coords[prev_pos];
      const TileOp& left_op = state_->prev_column[prev_pos];
      const std::int64_t idx = state_->checked_tile_index(coord.i, coord.j);
      state_->match_lr(left_coord,
                       coord,
                       idx,
                       left_op,
                       current_column[local],
                       frontier_cursor);
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

SpanningTrace StreamingCompositor::spanning_trace() const {
  return state_ ? state_->trace : SpanningTrace{};
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
