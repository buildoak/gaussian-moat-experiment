// src/grid.cpp
//
// Snapped-grid enumerator: integer-only tower construction for the
// canonical octant.
//
// Active-tile predicate (blueprint §4.2):
//
//   T_{i,j} is active iff there exists an integer (x, y) in its proper
//   region [x_lo, x_hi] × [y_lo, y_hi] with
//     * x_lo >= 0,                     (octant: x >= 0)
//     * y >= x,                        (octant: y >= x)
//     * R_inner² <= x² + y² <= R_outer² (annulus)
//
//   Here (x_lo, x_hi) = (o_x + i*S, o_x + (i+1)*S),
//        (y_lo, y_hi) = (o_y + j*S, o_y + (j+1)*S).
//
// Strategy:
//   1) Corner-based *necessary* conditions give a quick screen.
//   2) For boundary tiles (near the annulus edges or the y=x diagonal),
//      scan tile columns and compute the admissible monotone y-range with
//      i128 binary searches. Tiles deep in the bulk need no scan — corner
//      predicates suffice.
//
// Boundary confirmation is O(boundary_tiles * S), with closed tile bounds
// preserving the 257×257 lattice-point convention.

#include "campaign/grid.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"

namespace campaign {

namespace {

// Inclusive bounds for tile (i, j): x ∈ [x_lo, x_hi], y ∈ [y_lo, y_hi].
struct TileBox {
  std::int64_t x_lo;
  std::int64_t x_hi;
  std::int64_t y_lo;
  std::int64_t y_hi;
};

inline TileBox tile_box(std::int32_t i, std::int32_t j) noexcept {
  TileBox b;
  const std::int64_t S64 = static_cast<std::int64_t>(S);
  b.x_lo = static_cast<std::int64_t>(OFFSET_X) + S64 * i;
  b.x_hi = b.x_lo + S64;
  b.y_lo = static_cast<std::int64_t>(OFFSET_Y) + S64 * j;
  b.y_hi = b.y_lo + S64;
  return b;
}

inline __int128 sq128(std::int64_t v) noexcept {
  return static_cast<__int128>(v) * static_cast<__int128>(v);
}

// Largest y in [0, upper] such that y² <= n. Requires n >= 0.
std::int64_t floor_isqrt_i128(__int128 n, std::int64_t upper) noexcept {
  std::int64_t lo = 0;
  std::int64_t hi = upper;
  while (lo < hi) {
    const std::int64_t mid = lo + (hi - lo + 1) / 2;
    if (sq128(mid) <= n) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

// Smallest y in [0, upper] such that y² >= n. Requires 0 < n <= upper².
std::int64_t ceil_isqrt_i128(__int128 n, std::int64_t upper) noexcept {
  std::int64_t lo = 0;
  std::int64_t hi = upper;
  while (lo < hi) {
    const std::int64_t mid = lo + (hi - lo) / 2;
    if (sq128(mid) >= n) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

// Does tile (i, j) necessarily have no active lattice point?
// Quick rejections based on corners. Returns true if *definitely inactive*.
// False means "maybe active, need scan".
inline bool definitely_inactive(std::int32_t i, std::int32_t j,
                                const Grid& g) noexcept {
  const TileBox b = tile_box(i, j);

  // Octant: x >= 0. If entire box has x < 0, inactive.
  if (b.x_hi < 0) return true;

  // Octant: y >= x must be satisfiable. Max of (y - x) over box is at
  // (x_lo, y_hi). If y_hi < x_lo, inactive (every point has y < x).
  if (b.y_hi < b.x_lo) return true;

  // Inner radius: need some (x, y) with x² + y² >= R_inner². Max of
  // x² + y² in the non-negative-corner subset is at (x_hi, y_hi) (with
  // x_lo floored to 0). If x_hi² + y_hi² < R_inner², inactive.
  const std::int64_t x_hi_pos = std::max<std::int64_t>(0, b.x_hi);
  const std::int64_t y_hi_pos = std::max<std::int64_t>(0, b.y_hi);
  const __int128 max_norm_sq = sq128(x_hi_pos) + sq128(y_hi_pos);
  if (max_norm_sq < static_cast<__int128>(g.R_inner_sq)) return true;

  // Outer radius: need some (x, y) with x² + y² <= R_outer². Min over
  // non-negative corner subset is at (max(x_lo, 0), max(y_lo, 0)).
  const std::int64_t x_lo_pos = std::max<std::int64_t>(0, b.x_lo);
  const std::int64_t y_lo_pos = std::max<std::int64_t>(0, b.y_lo);
  // For the y >= x constraint, the minimum-norm point must also satisfy
  // y >= x. The feasible min-norm point is at (max(x_lo, 0),
  // max(y_lo, x_lo_pos)). If y_lo_pos >= x_lo_pos this is just (x_lo_pos,
  // y_lo_pos); otherwise (x_lo_pos, x_lo_pos). This is a lower bound on
  // the minimum-norm octant-feasible lattice point.
  const std::int64_t y_min_on_diag =
      std::max<std::int64_t>(y_lo_pos, x_lo_pos);
  // The point (x_lo_pos, y_min_on_diag) is in the box iff y_min_on_diag
  // <= y_hi. If not, no y >= x point in the box; already caught above.
  if (y_min_on_diag <= b.y_hi) {
    const __int128 min_norm_sq = sq128(x_lo_pos) + sq128(y_min_on_diag);
    if (min_norm_sq > static_cast<__int128>(g.R_outer_sq)) return true;
  }

  return false;  // plausibly active — needs confirmation
}

// Confirm tile (i, j) has at least one lattice point (x, y) satisfying the
// full octant-annulus predicate. O(S) columns; all squared values use i128.
bool active_by_y_range(std::int32_t i, std::int32_t j, const Grid& g) {
  const TileBox b = tile_box(i, j);

  const std::int64_t x_start = std::max<std::int64_t>(0, b.x_lo);
  const std::int64_t x_end = b.x_hi;  // inclusive
  if (x_end < x_start) return false;

  const __int128 R_in_sq_128 = static_cast<__int128>(g.R_inner_sq);
  const __int128 R_out_sq_128 = static_cast<__int128>(g.R_outer_sq);
  const std::int64_t R_inner_64 = static_cast<std::int64_t>(g.R_inner);
  const std::int64_t R_outer_64 = static_cast<std::int64_t>(g.R_outer);

  for (std::int64_t x = x_start; x <= x_end; ++x) {
    const __int128 x_sq_128 = sq128(x);
    const __int128 y_sq_ub = R_out_sq_128 - x_sq_128;
    if (y_sq_ub < 0) continue;  // x alone already exceeds R_outer

    const std::int64_t y_hi_annulus =
        floor_isqrt_i128(y_sq_ub, R_outer_64);

    const __int128 y_sq_lb = R_in_sq_128 - x_sq_128;
    const std::int64_t y_lo_annulus =
        (y_sq_lb <= 0) ? 0 : ceil_isqrt_i128(y_sq_lb, R_inner_64);

    const std::int64_t y_lo =
        std::max({b.y_lo, x, y_lo_annulus});
    const std::int64_t y_hi = std::min(b.y_hi, y_hi_annulus);
    if (y_lo <= y_hi) return true;
  }

  return false;
}

// Determine if tile (i, j) is active.
// Fast path: corner reject. Slow path: O(S) scan.
inline bool is_active_tile(std::int32_t i, std::int32_t j, const Grid& g) {
  if (definitely_inactive(i, j, g)) return false;
  return active_by_y_range(i, j, g);
}

// -----------------------------------------------------------------------------
// Column / tower enumeration
// -----------------------------------------------------------------------------

// Find min i such that there exists j with T_{i,j} active.
// At project scale starts at i=0 because x_lo = o_x + i*S = 1 + 0 = 1 >= 0.
// Scans upward until first active column.
std::int32_t find_i_min(const Grid& g) {
  // Lower bound: i = 0 (x_lo = o_x >= 0). Walk until an active j exists.
  for (std::int32_t i = 0; i <= 1'000'000'000; ++i) {
    // Find any j by binary-ish search: check j = i (on diagonal) as a start,
    // then walk up. For speed we do a quick bounded scan.
    const TileBox b_at_diag = tile_box(i, i);
    (void)b_at_diag;
    // Look for j in [i, j_upper_bound]. j_upper_bound: y such that
    // y² <= R_outer² - x_lo². x_lo >= 0 guaranteed.
    const std::int64_t x_lo_64 = static_cast<std::int64_t>(OFFSET_X) +
                                  static_cast<std::int64_t>(S) * i;
    if (x_lo_64 < 0) continue;
    const __int128 rem = static_cast<__int128>(g.R_outer_sq) -
                         static_cast<__int128>(x_lo_64) *
                             static_cast<__int128>(x_lo_64);
    if (rem < 0) {
      // x_lo already exceeds R_outer — no more columns possible.
      return std::numeric_limits<std::int32_t>::min();
    }
    const std::int64_t y_max_for_x =
        floor_isqrt_i128(rem, static_cast<std::int64_t>(g.R_outer));
    const std::int32_t j_upper = static_cast<std::int32_t>(
        (y_max_for_x - static_cast<std::int64_t>(OFFSET_Y)) /
        static_cast<std::int64_t>(S));

    for (std::int32_t j = std::max<std::int32_t>(i - 1, 0);
         j <= j_upper + 1; ++j) {
      if (is_active_tile(i, j, g)) return i;
    }
  }
  return std::numeric_limits<std::int32_t>::min();
}

// Find max i such that there exists j with T_{i,j} active.
std::int32_t find_i_max(const Grid& g) {
  // Upper bound: i*S + o_x <= R_outer ⇒ i <= (R_outer - o_x)/S
  const std::int64_t i_upper = static_cast<std::int64_t>(
      (g.R_outer - static_cast<std::uint64_t>(OFFSET_X)) /
      static_cast<std::uint64_t>(S));
  for (std::int32_t i = static_cast<std::int32_t>(i_upper) + 1;
       i >= 0; --i) {
    const std::int64_t x_lo_64 = static_cast<std::int64_t>(OFFSET_X) +
                                  static_cast<std::int64_t>(S) * i;
    const __int128 rem = static_cast<__int128>(g.R_outer_sq) -
                         static_cast<__int128>(x_lo_64) *
                             static_cast<__int128>(x_lo_64);
    if (rem < 0) continue;
    const std::int64_t y_max_for_x =
        floor_isqrt_i128(rem, static_cast<std::int64_t>(g.R_outer));
    const std::int32_t j_upper = static_cast<std::int32_t>(
        (y_max_for_x - static_cast<std::int64_t>(OFFSET_Y)) /
        static_cast<std::int64_t>(S));
    for (std::int32_t j = std::max<std::int32_t>(i - 1, 0);
         j <= j_upper + 1; ++j) {
      if (is_active_tile(i, j, g)) return i;
    }
  }
  return std::numeric_limits<std::int32_t>::min();
}

// Find [j_low, j_high] for column i. Precondition: column has at least one
// active tile.
std::pair<std::int32_t, std::int32_t> find_tower(std::int32_t i,
                                                 const Grid& g) {
  // Start from the diagonal (j = i), widen up and down.
  const std::int64_t x_lo_64 = static_cast<std::int64_t>(OFFSET_X) +
                                static_cast<std::int64_t>(S) * i;
  const __int128 rem = static_cast<__int128>(g.R_outer_sq) -
                       static_cast<__int128>(x_lo_64) *
                           static_cast<__int128>(x_lo_64);

  std::int32_t j_upper = 0;
  if (rem >= 0) {
    const std::int64_t y_max_for_x =
        floor_isqrt_i128(rem, static_cast<std::int64_t>(g.R_outer));
    j_upper = static_cast<std::int32_t>(
        (y_max_for_x - static_cast<std::int64_t>(OFFSET_Y)) /
        static_cast<std::int64_t>(S));
  }
  const std::int32_t j_search_lo = std::max<std::int32_t>(i - 2, 0);
  const std::int32_t j_search_hi = j_upper + 2;

  // Find any active j.
  std::int32_t any = -1;
  for (std::int32_t j = j_search_lo; j <= j_search_hi; ++j) {
    if (is_active_tile(i, j, g)) { any = j; break; }
  }
  if (any == -1) {
    return {0, -1};  // empty column
  }

  // Extend down.
  std::int32_t j_low = any;
  while (j_low > 0 && is_active_tile(i, j_low - 1, g)) --j_low;
  // And up (walk out to j_search_hi + margin).
  std::int32_t j_high = any;
  while (j_high < j_search_hi + 2 && is_active_tile(i, j_high + 1, g))
    ++j_high;

  return {j_low, j_high};
}

// -----------------------------------------------------------------------------
// Invariant checks (I1, I2, I4) — runtime asserts in DEBUG
// -----------------------------------------------------------------------------

void assert_invariants(const Grid& g) {
#ifndef NDEBUG
  if (g.i_max < g.i_min) return;  // empty grid: trivially satisfies

  // I1 — tower contiguity (every j in [j_low, j_high] must be active).
  for (std::int32_t i = g.i_min; i <= g.i_max; ++i) {
    const std::size_t k = static_cast<std::size_t>(i - g.i_min);
    const std::int32_t jlo = g.j_low[k];
    const std::int32_t jhi = g.j_high[k];
    assert(jlo <= jhi + 1);  // empty column or well-formed range
    for (std::int32_t j = jlo; j <= jhi; ++j) {
      assert(is_active_tile(i, j, g) && "I1 contiguity violated");
    }
  }

  // I2 — bounded shift: |j_low(i+1) - j_low(i)| <= 1 and same for j_high.
  for (std::int32_t i = g.i_min; i < g.i_max; ++i) {
    const std::size_t k = static_cast<std::size_t>(i - g.i_min);
    const std::int32_t d_lo = g.j_low[k + 1] - g.j_low[k];
    const std::int32_t d_hi = g.j_high[k + 1] - g.j_high[k];
    // Only assert if both columns are non-empty.
    const bool col_k_nonempty = g.j_high[k] >= g.j_low[k];
    const bool col_k1_nonempty = g.j_high[k + 1] >= g.j_low[k + 1];
    if (col_k_nonempty && col_k1_nonempty) {
      assert(d_lo >= -1 && d_lo <= 1 && "I2 j_low shift > 1");
      assert(d_hi >= -1 && d_hi <= 1 && "I2 j_high shift > 1");
    }
  }

  // I4 — diagonal orphans: for every diagonally-adjacent active pair
  // (i, j) and (i+1, j+1) or (i, j) and (i+1, j-1), at least one
  // face-neighbor common to both is active.
  auto col_has = [&](std::int32_t i, std::int32_t j) -> bool {
    if (!g.has_column(i)) return false;
    const auto [lo, hi] = g.column_bounds(i);
    return j >= lo && j <= hi;
  };

  for (std::int32_t i = g.i_min; i < g.i_max; ++i) {
    const auto [lo, hi] = g.column_bounds(i);
    for (std::int32_t j = lo; j <= hi; ++j) {
      // (i, j) active; check diagonals (i+1, j+1) and (i+1, j-1)
      for (int dj : {+1, -1}) {
        const std::int32_t j2 = j + dj;
        if (col_has(i + 1, j2)) {
          // A common face-neighbor must be active: either (i+1, j) or
          // (i, j2). (i, j) is already known active, which is not a
          // face-neighbor of (i+1, j2). Face-neighbors of the pair are
          // the two "corner-completers".
          const bool n1 = col_has(i + 1, j);
          const bool n2 = col_has(i, j2);
          assert((n1 || n2) && "I4 diagonal orphan detected");
        }
      }
    }
  }
#else
  (void)g;
#endif
}

}  // namespace

bool Grid::has_column(std::int32_t i) const noexcept {
  return i >= i_min && i <= i_max;
}

std::pair<std::int32_t, std::int32_t> Grid::column_bounds(
    std::int32_t i) const noexcept {
  if (!has_column(i)) return {0, -1};
  const std::size_t k = static_cast<std::size_t>(i - i_min);
  return {j_low[k], j_high[k]};
}

std::int64_t Grid::flat_index(std::int32_t i,
                              std::int32_t j) const noexcept {
  if (!has_column(i)) return -1;
  const std::size_t k = static_cast<std::size_t>(i - i_min);
  if (j < j_low[k] || j > j_high[k]) return -1;
  return tower_offset[k] + static_cast<std::int64_t>(j - j_low[k]);
}

std::vector<TileCoord> Grid::enumerate_active_tiles() const {
  std::vector<TileCoord> out;
  out.reserve(static_cast<std::size_t>(total_tiles));
  for (std::int32_t i = i_min; i <= i_max; ++i) {
    const std::size_t k = static_cast<std::size_t>(i - i_min);
    const std::int32_t jlo = j_low[k];
    const std::int32_t jhi = j_high[k];
    for (std::int32_t j = jlo; j <= jhi; ++j) {
      TileCoord tc{};
      tc.i = i;
      tc.j = j;
      tc.a_lo = static_cast<std::int64_t>(OFFSET_X) +
                static_cast<std::int64_t>(S) * i;
      tc.b_lo = static_cast<std::int64_t>(OFFSET_Y) +
                static_cast<std::int64_t>(S) * j;
      out.push_back(tc);
    }
  }
  return out;
}

Grid Grid::build(std::uint64_t R_inner, std::uint64_t R_outer,
                 std::uint32_t K_SQ_arg) {
  if (R_inner == 0) {
    throw std::invalid_argument("Grid::build: R_inner must be > 0");
  }
  if (R_outer <= R_inner) {
    throw std::invalid_argument("Grid::build: R_outer must be > R_inner");
  }
  if (K_SQ_arg == 0) {
    throw std::invalid_argument("Grid::build: K_SQ must be > 0");
  }
  if (R_outer > (std::uint64_t{1} << 32)) {
    throw std::invalid_argument(
        "Grid::build: R_outer too large (R_outer^2 must fit in uint64_t)");
  }

  Grid g{};
  g.R_inner = R_inner;
  g.R_outer = R_outer;
  g.R_inner_sq = R_inner * R_inner;
  g.R_outer_sq = R_outer * R_outer;
  g.K_SQ_value = K_SQ_arg;
  g.S_value = S;
  g.C_value = C;
  g.o_x = OFFSET_X;
  g.o_y = OFFSET_Y;

  g.i_min = find_i_min(g);
  if (g.i_min == std::numeric_limits<std::int32_t>::min()) {
    // No active tiles anywhere. Empty grid.
    g.i_max = g.i_min - 1;
    g.total_tiles = 0;
    return g;
  }
  g.i_max = find_i_max(g);
  if (g.i_max < g.i_min) {
    g.total_tiles = 0;
    return g;
  }

  const int n_cols = g.i_max - g.i_min + 1;
  g.j_low.assign(static_cast<std::size_t>(n_cols), 0);
  g.j_high.assign(static_cast<std::size_t>(n_cols), -1);
  g.tower_offset.assign(static_cast<std::size_t>(n_cols + 1), 0);

  std::int64_t running = 0;
  for (std::int32_t i = g.i_min; i <= g.i_max; ++i) {
    const std::size_t k = static_cast<std::size_t>(i - g.i_min);
    auto [lo, hi] = find_tower(i, g);
    g.j_low[k] = lo;
    g.j_high[k] = hi;
    g.tower_offset[k] = running;
    if (hi >= lo) running += (hi - lo + 1);
  }
  g.tower_offset[static_cast<std::size_t>(n_cols)] = running;
  g.total_tiles = running;

  // Runtime invariant checks (DEBUG only).
  assert_invariants(g);

  return g;
}

}  // namespace campaign
