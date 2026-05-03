// include/campaign/grid.h
//
// Snapped-grid enumerator for the cpp-campaign-v2 reference build.
//
// Builds the tile tower table for the canonical octant
// R = { (x, y) ∈ ℤ² : x ≥ 0, y ≥ x, R_inner² ≤ x² + y² ≤ R_outer² }
// on the uniform sub-lattice { (o_x + i*S, o_y + j*S) : (i, j) ∈ ℤ² }.
//
// A tile T_{i,j} is active iff its proper region
// [o_x + i*S, o_x + (i+1)*S] × [o_y + j*S, o_y + (j+1)*S]
// contains at least one lattice point of R (blueprint §4.2).
//
// All arithmetic is integer (i64 or wider). No std::sqrt, no floating
// point — per forbidden-pattern rule §6.3 #1.
//
// Invariants (I1, I2, I4) are checked in DEBUG by Grid::build() and also
// available as the always-on `Grid::verify_invariants()` member that
// callers can invoke in release builds for a runtime seatbelt (blueprint
// §4.3 mandatory gate, closes the [PROOF GAP] tower-closing compensation
// at spec line 361).
//   I1 — tower contiguity: Tower_i is a contiguous integer interval.
//   I2 — bounded shift:    |j_low(i+1) - j_low(i)| ≤ 1,
//                          |j_high(i+1) - j_high(i)| ≤ 1.
//   I4 — no diagonal orphans: for every diagonally-adjacent active pair,
//        ≥ 1 face-neighbor common to both is active.
//
// Dependencies: constants.h, campaign_constants.h.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"

namespace campaign {

// Tile index + world-space origin bundle. Matches blueprint §4.5.
//
// `(i, j)` is the tile's grid index; `(a_lo, b_lo)` is the world-space
// origin of the proper tile region at `(o_x + i*S, o_y + j*S)`.
//
// i, j are signed because blueprint allows non-negative i and non-negative
// j, but tower shifts can produce transient negatives during enumeration.
// At the public API surface they are always >= 0.
struct TileCoord {
  std::int32_t i = 0;
  std::int32_t j = 0;
  std::int64_t a_lo = 0;
  std::int64_t b_lo = 0;
};

// Snapped-grid tower table.
//
// Life cycle:
//   1) `Grid g = Grid::build(R_inner, R_outer, K_SQ);` — integer enumeration
//      and invariant checks.
//   2) `g.enumerate_active_tiles()` — flat list of all active TileCoords in
//      canonical column-major order (i ascending, then j ascending).
//
// Grid is cheap-copyable (vectors only) and immutable once built. All
// methods are const after `build()`.
struct Grid {
  // Campaign inputs (blueprint §4.5 / §4.6)
  std::uint64_t R_inner = 0;
  std::uint64_t R_outer = 0;
  std::uint64_t R_inner_sq = 0;
  std::uint64_t R_outer_sq = 0;
  std::uint32_t K_SQ_value = 0;
  std::int32_t S_value = S;
  std::int32_t C_value = C;
  std::int32_t o_x = OFFSET_X;
  std::int32_t o_y = OFFSET_Y;

  // Column range of the octant (inclusive). i_max < i_min => empty grid.
  std::int32_t i_min = 0;
  std::int32_t i_max = -1;

  // Per-column tower bounds [j_low[k], j_high[k]] for column i = i_min + k.
  // j_high[k] < j_low[k] encodes an empty column (should not occur in a
  // well-formed canonical-octant build).
  std::vector<std::int32_t> j_low;
  std::vector<std::int32_t> j_high;

  // Prefix sum of column heights — `tower_offset[k]` is the flat index
  // of the first active tile in column i_min + k. `tower_offset.back()`
  // equals total_tiles (sentinel).
  std::vector<std::int64_t> tower_offset;

  // Total active tile count across the octant.
  std::int64_t total_tiles = 0;

  // Optional sparse override used by explicit tile-list regions. Empty means
  // the canonical contiguous tower table is authoritative.
  std::vector<TileCoord> explicit_tiles;

  // -------------------------------------------------------------------------
  // Construction
  // -------------------------------------------------------------------------

  // Build the tower table from campaign radii.
  //
  // Preconditions (checked; throws std::invalid_argument on violation):
  //   * R_outer > R_inner > 0
  //   * K_SQ > 0
  //   * R_inner, R_outer, R_outer^2 all fit in uint64_t
  //
  // Postconditions (asserted in DEBUG, NDEBUG skips invariant checks for
  // speed at project-scale):
  //   * I1, I2, I4 hold across the built tower table.
  //   * total_tiles == sum of column heights.
  //
  // Complexity: O(i_max - i_min + 1) column enumeration + O(total_tiles)
  // for the I4 diagonal scan. ~1s at project scale per blueprint §4.3.
  static Grid build(std::uint64_t R_inner, std::uint64_t R_outer,
                    std::uint32_t K_SQ_arg);

  // -------------------------------------------------------------------------
  // Queries
  // -------------------------------------------------------------------------

  // Return true iff column i is within the octant's column range.
  bool has_column(std::int32_t i) const noexcept;

  bool is_sparse() const noexcept { return !explicit_tiles.empty(); }

  // Inclusive [j_low, j_high] for column i. Precondition: has_column(i).
  // Returns (0, -1) sentinel if i is out of range.
  std::pair<std::int32_t, std::int32_t> column_bounds(std::int32_t i) const noexcept;

  // Flat index of tile (i, j) in the canonical column-major layout.
  // Precondition: has_column(i) && j ∈ [j_low, j_high].
  // Returns -1 if out of range.
  std::int64_t flat_index(std::int32_t i, std::int32_t j) const noexcept;

  // -------------------------------------------------------------------------
  // Enumeration
  // -------------------------------------------------------------------------

  // Emit all active TileCoords in canonical order.
  //
  // Canonical order is column-major: for i = i_min .. i_max, for j =
  // j_low[i] .. j_high[i]. This ordering is the wire-stable convention for
  // the snapshot (plan §5 + §6.3 #5).
  //
  // Output vector has exactly `total_tiles` entries.
  std::vector<TileCoord> enumerate_active_tiles() const;

  // Emit active tiles in one column in canonical j order. For normal grids
  // this expands the contiguous tower range; for sparse grids this returns
  // only explicitly listed coordinates.
  std::vector<TileCoord> enumerate_column_tiles(std::int32_t i) const;

  // Always-on runtime seatbelt for the I1/I2/I4 invariants. Returns the
  // empty string iff the grid satisfies all three; otherwise returns a
  // non-empty diagnostic describing the first violation. Intended to be
  // called by production entry points (see campaign_main) as the
  // release-mode compensation for the tower-closing [PROOF GAP] at spec
  // line 361 per blueprint §4.3. O(columns) for contiguous tower grids.
  //
  // Sparse (explicit_tiles) grids skip the I1/I2 shape checks because they
  // do not represent a tower; only the explicit-tile set is validated for
  // structural consistency.
  std::string verify_invariants() const;
};

}  // namespace campaign
