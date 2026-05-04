// include/campaign/streaming_compositor.h
//
// Column-complete streaming TileOp compositor. This is the CPU proof surface
// for SPANNING early exit: ingest complete columns, retain only the previous
// column's TileOps for L/R stitching, and expose a canonical current-frontier
// projection for small-grid tests.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "campaign/compositor.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"

namespace campaign {

struct FrontierPort {
  std::int32_t j = 0;
  std::uint8_t port_ordinal = 0;
  std::uint32_t component = 0;
  std::uint8_t reach = 0;

  friend bool operator==(const FrontierPort&,
                         const FrontierPort&) = default;
};

struct SpanningStitchVertex {
  std::int64_t tile_index = -1;
  std::int32_t group_label = 0;

  friend bool operator==(const SpanningStitchVertex&,
                         const SpanningStitchVertex&) = default;
};

struct SpanningStitchEdge {
  std::string event;
  SpanningStitchVertex lhs;
  SpanningStitchVertex rhs;
  Face lhs_face = Face::I;
  Face rhs_face = Face::I;
  std::uint8_t lhs_ordinal = 0;
  std::uint8_t rhs_ordinal = 0;
};

struct SpanningStitchPath {
  bool enabled = false;
  bool reconstructed = false;
  std::string failure_reason;
  std::uint64_t recorded_edges = 0;
  SpanningStitchVertex inner_source;
  SpanningStitchVertex outer_source;
  SpanningStitchVertex inner_endpoint;
  SpanningStitchVertex outer_endpoint;
  bool final_bridge_present = false;
  SpanningStitchEdge final_bridge;
  std::vector<SpanningStitchEdge> inner_path_edges;
  std::vector<SpanningStitchEdge> outer_path_edges;
};

struct SpanningTrace {
  bool detected = false;
  std::string event;
  std::int32_t column_i = 0;
  std::int32_t tile_j = 0;
  std::int64_t tile_index = -1;
  std::int32_t group_label = 0;
  std::int64_t lhs_tile_index = -1;
  std::int32_t lhs_group_label = 0;
  std::int64_t rhs_tile_index = -1;
  std::int32_t rhs_group_label = 0;
  std::uint32_t component = 0;
  std::uint8_t reach_before = 0;
  std::uint8_t reach_after = 0;
  std::uint8_t added_bits = 0;
  std::int64_t inner_source_tile_index = -1;
  std::int32_t inner_source_group_label = 0;
  std::int64_t outer_source_tile_index = -1;
  std::int32_t outer_source_group_label = 0;
  SpanningStitchPath path;
};

class StreamingCompositor {
 public:
  StreamingCompositor();
  ~StreamingCompositor();

  void init(const Grid& grid);
  void ingest_column(std::int32_t i, std::span<const TileOp> column_tileops);
  void set_trace_spanning_path(bool enabled);

  bool has_spanning() const noexcept;
  SpanningTrace spanning_trace() const;
  Verdict finalize();

  // Test/proof hook: canonical equality + reach state for live right-face
  // ports after the most recently ingested non-empty column. Entries are keyed
  // by (j, 1-based port ordinal); `component` is remapped in first-seen order
  // so two projections are directly comparable.
  std::vector<FrontierPort> project_frontier() const;

 private:
  struct State;
  State* state_ = nullptr;
};

}  // namespace campaign
