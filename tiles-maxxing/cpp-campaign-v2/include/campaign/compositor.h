// include/campaign/compositor.h
//
// Cross-tile UF + moat verdict engine for the cpp-campaign-v2 reference
// build.
//
// API surface is the blueprint §7.5 "frozen surface". Phase 2 implements
// the methods; Phase 1 provides stubs only.
//
// Flow (blueprint §7):
//   1) init(grid)  — allocate parent_ / root_reach_ for the global group
//                    ID space: global_group_id(tile_idx, g) = tile_idx * 128 + g.
//   2) ingest_column(i, column_tileops) — for each tile, call
//      `mark_tile_ports` to OR inner/outer flags into REACH bits, then
//      stitch I↔O within the column (Lemma 4 positional match). After
//      the column is processed, stitch L↔R against the previous column.
//   3) finalize() — emit MOAT if no UF root ever carries both REACH_INNER
//      and REACH_OUTER; else SPANNING.
//
// No geometry. No staircase heuristics. The canonical math (Exit Lemma +
// Theorem 11) guarantees every geo_I / geo_O prime is represented through
// a port of its UF component, so `mark_tile_ports` is a flag read + DSU
// update.
//
// Dependencies: grid.h, tileop.h, union_find.h.

#pragma once

#include <cstdint>

#include "campaign/grid.h"
#include "campaign/tileop.h"
#include "campaign/union_find.h"

namespace campaign {

// Moat campaign verdict.
enum class Verdict : std::uint8_t {
  kUnknown = 0,
  kMoat = 1,
  kSpanning = 2,
};

// Cross-tile UF compositor. Single-threaded; ingests column by column in
// canonical i-ascending order.
class Compositor {
 public:
  Compositor();
  ~Compositor();

  // Allocate internal state for the grid.
  //
  // Preconditions:
  //   * grid.total_tiles <= (2^32 / 128) — blueprint §7.1 (33.5 M cap).
  //
  // STUB in Phase 1.
  void init(const Grid& grid);

  // Ingest one column's worth of TileOps (column i). `column_tileops` has
  // `grid.j_high[k] - grid.j_low[k] + 1` contiguous entries for k = i - i_min.
  //
  // After this call, `has_spanning()` reflects any REACH_INNER ∩ REACH_OUTER
  // collisions encountered so far (incremental check).
  //
  // STUB in Phase 1.
  void ingest_column(std::int32_t i, const TileOp* column_tileops);

  // True iff any DSU root has both REACH_INNER and REACH_OUTER set.
  // Monotonic — once true, stays true.
  bool has_spanning() const noexcept;

  // Emit final verdict. Call only after all columns have been ingested.
  //
  // STUB in Phase 1: returns kUnknown.
  Verdict finalize();

 private:
  // PIMPL-ish state. Full members defined in src/compositor.cpp; header
  // stays lean so tests and apps don't pull in UF internals.
  struct State;
  State* state_ = nullptr;
};

}  // namespace campaign
