#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# ///
"""Generate a region.json (tile list only) without running the full oracle.

The full `preflight-oracle.py` runs `process_tile` on every tile which is
expensive and does not scale past small N. Also, its `first_n_tiles` walks
`is_tile_active` per column from j=0 which is O(R/S) per column and
impractical at K=40 R=800M.

This helper computes the first N canonical active tiles using analytic
annulus/octant bounds (no 257x257 lattice scan). Equivalent to C++
grid.cpp enumeration but pure Python.

Correctness check: on the committed K=36 selfcheck region, this must match
preflight-oracle.py byte-for-byte.
"""

from __future__ import annotations

import argparse
import json
import pathlib


def floor_isqrt(n: int) -> int:
    """Integer sqrt via stdlib (Python 3.8+)."""
    if n < 0:
        raise ValueError("floor_isqrt requires n>=0")
    # math.isqrt is native and fast for bigints
    import math
    return math.isqrt(n)


def ceil_isqrt(n: int) -> int:
    f = floor_isqrt(n)
    return f if f * f == n else f + 1


def active_j_bounds(i: int, k_sq: int, r_inner: int, r_outer: int,
                    offset_x: int, offset_y: int, s: int) -> tuple[int, int] | None:
    """Compute [j_lo, j_hi] of active tiles in column i, or None if empty.

    A tile (i,j) is active iff there exists integer (a,b) with
      a in [a_lo, a_hi], b in [b_lo, b_hi], b >= a, r_in^2 <= a^2+b^2 <= r_out^2.
    Here a_lo=offset_x+i*s, a_hi=a_lo+s (proper region, closed box of 257^2 lattice points),
    b_lo=offset_y+j*s, b_hi=b_lo+s.

    Approach: for each a in [a_lo, a_hi], the valid b range is
        b_min_a = max(a, ceil_isqrt(max(0, r_in^2 - a^2)))
        b_max_a = floor_isqrt(r_out^2 - a^2)   (or skip if r_out^2 < a^2)
    We want the union over a of [b_min_a, b_max_a]. Then j values where the
    tile's b-box [b_lo, b_hi] overlaps this union are active.

    Monotonicity: b_min_a increases with a (inner arc moves out), b_max_a
    decreases with a (outer arc moves in). Minimum b_min across the tile is
    attained at a=a_lo (the smallest a); maximum b_max is attained at a=a_lo
    too (the smallest a gives largest b-ceiling from outer arc).

    Wait: actually b_min_a = max(a, ceil_isqrt(r_in^2 - a^2)) — for a small,
    r_in^2-a^2 is large so ceil_isqrt >> a and b_min_a ≈ r_in. For a large,
    r_in^2-a^2 small and b_min_a ≈ a (octant constraint dominates). So across
    [a_lo, a_hi]:
      smallest b_min = b_min at a=a_lo (since inner arc is higher for smaller a,
                        but as a grows b_min transitions from ≈r_in to ≈a)
    Hmm, b_min is not monotonic. For safety, sample both ends and take min of
    b_min_a over a-ends and max of b_max_a.

    Careful: we want the BROADEST union. For a monotone-decreasing function
    (b_max_a) we take max at a=a_lo. For b_min_a which is ~max(a, inner_arc):
    - inner_arc(a) = ceil_isqrt(r_in^2 - a^2) is decreasing in a (when a^2 < r_in^2)
    - a is increasing in a
    So b_min_a = max of a decreasing and an increasing function — U-shaped or
    monotone. Minimum is where they cross (where a = inner_arc(a), i.e., a^2 =
    r_in^2/2, the y=x intersection with inner arc).

    Safe computation: evaluate the union interval endpoints at a_lo, a_hi and
    the middle crossover point.
    """
    s64 = s
    a_lo = offset_x + i * s64
    a_hi = a_lo + s64
    r_in_sq = r_inner * r_inner
    r_out_sq = r_outer * r_outer

    if a_lo > r_outer:
        return None

    # For each a, compute b_min_a and b_max_a. We need the min of b_min_a over
    # a in [a_lo, a_hi] (smallest admissible b) and the max of b_max_a (largest
    # admissible b).
    candidates_a = []
    a0 = max(0, a_lo)
    candidates_a.append(a0)
    if a_hi >= 0 and a_hi > a0:
        candidates_a.append(a_hi)
    # Crossover where a = ceil_isqrt(r_in^2 - a^2) → 2 a^2 ≈ r_in^2.
    # a_cross ≈ floor(sqrt(r_in^2/2))
    a_cross = floor_isqrt(r_in_sq // 2)
    if a_lo <= a_cross <= a_hi:
        candidates_a.append(a_cross)
    # Include a_cross+1 to catch integer crossover
    if a_lo <= a_cross + 1 <= a_hi:
        candidates_a.append(a_cross + 1)

    b_min_global = None  # min of b_min_a
    b_max_global = None  # max of b_max_a

    for a in candidates_a:
        if a < 0:
            continue
        a2 = a * a
        if a2 > r_out_sq:
            continue  # no b satisfies outer arc
        # b_max_a
        b_max_a = floor_isqrt(r_out_sq - a2)
        # b_min_a
        rem_in = r_in_sq - a2
        if rem_in <= 0:
            b_min_from_arc = 0  # whole band inside inner arc radius for this a
            # but octant still requires b >= a
            b_min_a = a
        else:
            b_min_from_arc = ceil_isqrt(rem_in)
            b_min_a = max(a, b_min_from_arc)
        if b_min_a > b_max_a:
            continue  # no valid b at this a
        if b_min_global is None or b_min_a < b_min_global:
            b_min_global = b_min_a
        if b_max_global is None or b_max_a > b_max_global:
            b_max_global = b_max_a

    # Also include a = a_lo+1..a_hi-1 for tighter bound? No, the 3-4 sample
    # points above capture the extrema of b_min (U-shape with 1 minimum) and
    # b_max (monotone).

    if b_min_global is None or b_max_global is None:
        return None

    # Tile j is active iff [b_lo=offset_y+j*s, b_hi=b_lo+s] overlaps
    # [b_min_global, b_max_global] (sufficient condition based on sampled a).
    # Overlap iff b_lo <= b_max_global and b_hi >= b_min_global.
    # b_lo <= b_max_global ⇒ offset_y + j*s <= b_max_global ⇒ j <= (b_max_global - offset_y) / s
    # b_hi >= b_min_global ⇒ offset_y + j*s + s >= b_min_global ⇒ j >= (b_min_global - offset_y - s) / s

    j_lo_candidate = (b_min_global - offset_y - s64 + s64 - 1) // s64  # = ceil((b_min_global - offset_y - s) / s)
    # simpler: j_min such that b_hi = b_lo + s >= b_min
    #   b_lo = offset_y + j*s; b_lo + s >= b_min ⇒ j >= (b_min - s - offset_y) / s
    #   j_lo = ceil((b_min - s - offset_y) / s)
    j_lo_raw = (b_min_global - s64 - offset_y)
    # ceiling division that works for negative:
    j_lo = -(-j_lo_raw // s64)
    j_lo = max(j_lo, i)  # octant: b >= a implies b_lo + s >= a_lo, but j*s+offset_y+s >= i*s+offset_x ⇒ j+1 >= i (roughly); actually we enforce per-tile octant later

    # j_hi such that b_lo <= b_max: j*s + offset_y <= b_max ⇒ j <= (b_max - offset_y) / s
    j_hi = (b_max_global - offset_y) // s64

    if j_lo > j_hi:
        return None

    # Octant requirement: b_hi >= a_lo (otherwise no lattice point with b>=a).
    # b_hi = offset_y + j*s + s >= a_lo = offset_x + i*s
    # ⇒ j + 1 >= (a_lo - offset_y) / s + i*s  wait let me redo: offset_y + (j+1)*s >= offset_x + i*s
    # ⇒ (j+1)*s >= (i*s) + (offset_x - offset_y)  => j >= i + (offset_x - offset_y - s) / s
    # For offset_x=offset_y=1: j >= i - 1. But we also need b>=a at a lattice point in the tile. Safe: j >= i - 1.
    j_lo = max(j_lo, i - 1)
    if j_lo > j_hi:
        return None

    # Now PER-TILE exact activity check using the same analytic method on the
    # tile's a-range. The above union is a superset of real activity. We need
    # to trim false positives. But at the granularity of j (tile step), the
    # overlap condition is sufficient: if [b_min_global, b_max_global] truly
    # represents achievable (a, b) pairs, AND the tile contains integer points,
    # then overlap implies at least one valid (a, b) in the tile. This is true
    # because (a, b) can be chosen from the sampled a. Specifically, for the a
    # that attains b_max_global (a=a_lo typically), b values b_lo..b_hi clipped
    # to [b_min at that a, b_max at that a] form a contiguous integer set if
    # non-empty.
    #
    # Verify boundary tiles exactly by checking: for j_lo and j_hi, is there
    # really a lattice point in the tile that satisfies all constraints?
    # (Fine-grained false-positive trimming at the j_lo and j_hi edges.)
    def tile_has_lattice_point(i_: int, j_: int) -> bool:
        a_lo2 = offset_x + i_ * s64
        a_hi2 = a_lo2 + s64
        b_lo2 = offset_y + j_ * s64
        b_hi2 = b_lo2 + s64
        # For each a in [max(0,a_lo2), a_hi2], compute b range ∩ [b_lo2, b_hi2]
        # Use analytic bounds per a — constant work per a, total 257 ops.
        for a in range(max(0, a_lo2), a_hi2 + 1):
            a2 = a * a
            if a2 > r_out_sq:
                continue
            b_max_a = floor_isqrt(r_out_sq - a2)
            rem_in = r_in_sq - a2
            b_min_from_arc = ceil_isqrt(rem_in) if rem_in > 0 else 0
            b_min_a = max(a, b_min_from_arc)
            lo = max(b_min_a, b_lo2)
            hi = min(b_max_a, b_hi2)
            if lo <= hi:
                return True
        return False

    # Trim j_lo upward until a lattice point exists.
    while j_lo <= j_hi and not tile_has_lattice_point(i, j_lo):
        j_lo += 1
    if j_lo > j_hi:
        return None
    # Trim j_hi downward.
    while j_hi >= j_lo and not tile_has_lattice_point(i, j_hi):
        j_hi -= 1
    if j_hi < j_lo:
        return None
    return (j_lo, j_hi)


def first_n_tiles(n_tiles: int, k_sq: int, r_inner: int, r_outer: int,
                  offset_x: int = 1, offset_y: int = 1, s: int = 256
                  ) -> list[tuple[int, int]]:
    tiles: list[tuple[int, int]] = []
    # Binary search for first i with non-empty column
    # For i too small, column empty because the entire j range of the tile lies
    # below the annulus — no b in the tile reaches r_inner from the tile's a.
    # Specifically, column i is non-empty iff the annulus intersects some b in
    # [0, any]; combined with octant. First i is the smallest with a_hi >= 0
    # AND a_lo^2 + b_max_possible^2 >= r_in^2 etc.
    # Simpler: linear scan from i=0 with O(1) check per i (no inner scan).
    i = 0
    max_i = (r_outer - offset_x) // s + 2
    while len(tiles) < n_tiles:
        if i > max_i:
            raise RuntimeError(f"ran out of active tiles before N={n_tiles} (scanned to i={i})")
        bounds = active_j_bounds(i, k_sq, r_inner, r_outer, offset_x, offset_y, s)
        if bounds is not None:
            j_lo, j_hi = bounds
            for j in range(j_lo, j_hi + 1):
                tiles.append((i, j))
                if len(tiles) == n_tiles:
                    return tiles
        i += 1
    return tiles


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--k-sq", type=int, required=True)
    parser.add_argument("--r-inner", type=int, required=True)
    parser.add_argument("--r-outer", type=int, required=True)
    parser.add_argument("--n-tiles", type=int, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    tiles = first_n_tiles(args.n_tiles, args.k_sq, args.r_inner, args.r_outer)
    if len(tiles) != args.n_tiles:
        raise RuntimeError(f"expected {args.n_tiles} tiles, got {len(tiles)}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    doc = {"tiles": [{"i": i, "j": j} for (i, j) in tiles]}
    args.output.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")
    print(f"region wrote {args.output} ({len(tiles)} tiles)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
