// include/campaign/region.h
//
// Sub-region specification for the cpp-campaign-v2 reference build.
//
// Design principle (plan §1): "sub-region = self-contained campaign".
// Every invocation takes a Region spec (default: full canonical octant)
// and runs the complete pipeline against it. A 3-tile unit test and a
// 10M-tile production run are the same binary, same code paths, same
// output format. The unit tests, integration tests, and the eventual
// CUDA-parity gate all collapse to `compare_snapshots A B`.
//
// Region shapes supported in v2:
//   * Full canonical octant (all active tiles in the grid).
//   * Tile-index box with per-column j-range.
//   * Explicit tile list for sparse hand-golden regions.
//
// JSON grammar (parsed via nlohmann::json from `--region <file>`):
//
//     { "full_octant": true }
//
//   OR
//
//     {
//       "i_lo": 0, "i_hi": 1,
//       "columns": [
//         { "i": 0, "j_lo": 312500, "j_hi": 312502 },
//         { "i": 1, "j_lo": 312500, "j_hi": 312501 }
//       ]
//     }
//
//   OR
//
//     {
//       "tiles": [
//         { "i": 0, "j": 312500 },
//         { "i": 1, "j": 312500 }
//       ]
//     }

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "campaign/constants.h"
#include "campaign/grid.h"

namespace campaign {

// Inclusive j-range [j_lo, j_hi] for a single column i.
struct JRange {
  int j_lo = 0;
  int j_hi = -1;  // j_hi < j_lo => empty column

  bool empty() const noexcept { return j_hi < j_lo; }
  int size() const noexcept { return empty() ? 0 : (j_hi - j_lo + 1); }
};

// Region specification.
//
// Three populated forms:
//
//   1. `is_full_octant == true` — `column_ranges` is empty; caller resolves
//      to the full grid via `Region::full_octant(grid)` before iterating.
//
//   2. `is_full_octant == false` — `column_ranges` has one entry per column
//      in [i_lo, i_hi]. Each entry is a `JRange` that is the intersection
//      of the requested box with the Grid's tower at that column (narrower
//      than the tower, never wider).
//
//   3. `is_explicit_tile_list == true` — `explicit_tiles` has the exact
//      requested tile coordinates in canonical lexicographic order.
//
// Invariants (asserted by factories in region.cpp):
//   * i_lo <= i_hi
//   * column_ranges.size() == (i_hi - i_lo + 1) when not full-octant
//   * every JRange is either empty() or j_lo <= j_hi
struct Region {
  bool is_full_octant = false;
  bool is_explicit_tile_list = false;

  int i_lo = 0;
  int i_hi = -1;  // inclusive; -1 when unset

  // column_ranges[i - i_lo] gives the JRange for column i.
  // Empty for full-octant form (resolved lazily via full_octant()).
  std::vector<JRange> column_ranges;
  std::vector<TileCoord> explicit_tiles;

  // Factory: resolve the full canonical octant against a built Grid.
  //
  // Populates i_lo, i_hi, and column_ranges from the grid's tower table.
  // Returns a concrete (non-full_octant-flagged) Region so downstream code
  // has a single iteration path.
  //
  // Preconditions:
  //   * grid has been built via Grid::build().
  static Region full_octant(const Grid& grid);

  // Factory: build a Region from a tile-index box spec.
  //
  // `j_ranges[k]` is the JRange for column `i_lo + k`. The caller supplies
  // exactly `i_hi - i_lo + 1` entries.
  //
  // The returned Region does NOT clip to any Grid; the caller is responsible
  // for intersecting with tower bounds (typically via `clip_to_grid()`, TODO
  // Phase 2 if needed — not required for Phase 1 acceptance).
  //
  // Throws std::invalid_argument on size mismatch or inverted box.
  static Region from_tile_box(int i_lo, int i_hi,
                              std::vector<JRange> j_ranges);

  // Factory: build a Region from an explicit tile-list spec.
  //
  // Throws std::invalid_argument on an empty list or duplicate coordinates.
  static Region from_tile_list(std::vector<TileCoord> tiles);

  // Parse a JSON region object (see grammar above).
  static Region from_json(const nlohmann::json& doc);

  // Parse a JSON region file (see grammar above).
  //
  // Preconditions:
  //   * `path` points to a readable file containing a single JSON object
  //     following the grammar documented at file-top.
  //
  // Throws:
  //   * std::runtime_error if the file cannot be opened.
  //   * std::invalid_argument if the JSON fails grammar validation.
  //
  // On success returns a Region that is either full-octant (caller should
  // resolve via `full_octant(grid)`) or tile-box form (ready to iterate).
  static Region from_json_file(const std::string& path);

  // Accessor: return the JRange for column `i`.
  //
  // Preconditions:
  //   * !is_full_octant (caller must resolve first)
  //   * i_lo <= i <= i_hi
  //
  // Returns an empty JRange if column i has no tiles in the region
  // (never throws on in-range queries).
  JRange column_slice(int i) const;

  // Exact per-column ranges. Tile-list regions can return multiple ranges
  // for one column; tile-box regions return at most one.
  std::vector<JRange> column_slices(int i) const;

  const std::vector<TileCoord>& tiles() const noexcept { return explicit_tiles; }

  // Total active tile count across all columns. O(i_hi - i_lo + 1).
  std::int64_t tile_count() const noexcept;
};

}  // namespace campaign
