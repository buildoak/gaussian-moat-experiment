"""
run.py — Tile Composition Verification Cases

Runs the full verification suite: for each test case, executes both the
tiled pipeline (Campaign) and the brute-force oracle (DirectVerifier),
compares their verdicts, verifies full component equivalence, and
generates visualizations.

Test cases:
1. Full lattice → must SPAN (trivial connectivity)
2. Horizontal moat → must be MOAT (barrier blocks all paths)
3. Moat with bridge → must SPAN (single narrow passage)
4. Sparse random removal → agreement test (stochastic)
5. Larger radius with moat → MOAT (r=sqrt(5))
6. Larger radius full → SPANNING (r=sqrt(5))
7. Sparse lattice r=sqrt(5) 30% removal → component equivalence stress
8. Sparse lattice r=sqrt(10) 20% removal → larger radius
9. Adversarial boundary removal → remove points ON tile boundaries

Grid size defaults to 8×4 tiles (2049×1025 points) for fast iteration.
Pass --full for the production-scale 64×32 grid (16385×8193 points).

Usage:
    python run.py          # fast mode (8×4 tiles)
    python run.py --full   # production scale (64×32 tiles)
"""

from __future__ import annotations

import math
import random
import sys
import time
from dataclasses import dataclass

import numpy as np

from campaign import Campaign, CampaignResult
from compositor import Compositor
from direct import DirectResult, DirectVerifier
from graph import Graph
from grid import Grid
from tile import TILE_SIZE
from uf import UnionFind
from visualize import Visualizer


# ---------------------------------------------------------------------------
# Component equivalence verification
# ---------------------------------------------------------------------------

@dataclass
class EquivalenceReport:
    """Detailed result of component-equivalence comparison."""
    equivalent: bool         # True if partition is identical
    tiled_components: int    # number of components in tiled pipeline
    direct_components: int   # number of components in direct UF
    total_points: int        # alive points compared
    mismatched_count: int    # points that disagree
    example_mismatches: list[tuple[int, int]]  # up to 10 (x,y) of mismatches


def verify_component_equivalence(
    graph: Graph,
    campaign_result: CampaignResult,
    direct_result: DirectResult,
) -> EquivalenceReport:
    """Verify that every alive point has the same component membership
    under the tiled pipeline and the direct UF.

    Algorithm:
    1. Build tiled label map: for each alive point, trace through the tile's
       point_group → (col, row, group_id) → compositor global_idx → compositor UF root.
    2. Build direct label map: for each alive point, look up point_idx → direct UF root.
    3. Normalize both labelings to canonical sequential IDs.
    4. Compare: identical partitions iff normalized labels match everywhere.

    Returns EquivalenceReport with detailed diagnostics.
    """
    grid: Grid = campaign_result._grid
    compositor: Compositor = campaign_result._compositor
    direct_uf: UnionFind = direct_result._uf
    direct_point_idx: np.ndarray = direct_result._point_idx

    w, h = graph.width, graph.height

    # -- Build tiled label map -------------------------------------------------
    # For each alive point, find which tile(s) it belongs to (in nominal bounds),
    # look up its group in that tile's point_group, then trace through the
    # compositor's global UF to get a root.  Boundary points may appear in
    # multiple tiles' sieve domains, but we only need one path — the compositor
    # ensures they all merge to the same root.
    #
    # Strategy: iterate tiles, for each tile iterate its nominal-domain points,
    # assign tiled root.  Skip if already assigned (boundary overlap).

    tiled_root: dict[tuple[int, int], int] = {}

    for tile in grid.all_tiles():
        x_lo, x_hi = tile.x_lo, tile.x_hi
        y_lo, y_hi = tile.y_lo, tile.y_hi
        pg = tile.point_group  # (x,y) → group_id (sieve-domain labels)
        col, row = tile.col, tile.row

        for y in range(y_lo, y_hi + 1):
            for x in range(x_lo, x_hi + 1):
                pt = (x, y)
                if pt in tiled_root:
                    continue  # already assigned by an earlier tile
                if not graph.alive[y, x]:
                    continue
                gid = pg.get(pt)
                if gid is None:
                    # Point is alive but not in this tile's sieve domain?
                    # Should not happen if collar >= 0, but guard anyway.
                    continue
                key = (col, row, gid)
                gi = compositor.global_idx.get(key)
                if gi is not None:
                    tiled_root[pt] = compositor.uf.find(gi)

    # -- Build direct label map ------------------------------------------------
    direct_root: dict[tuple[int, int], int] = {}
    for y in range(h):
        for x in range(w):
            if graph.alive[y, x]:
                idx = int(direct_point_idx[y, x])
                if idx >= 0:
                    direct_root[(x, y)] = direct_uf.find(idx)

    # -- Normalize both labelings ----------------------------------------------
    def normalize(label_map: dict[tuple[int, int], int]) -> dict[tuple[int, int], int]:
        """Remap arbitrary root IDs to sequential 0..N-1."""
        root_to_canonical: dict[int, int] = {}
        counter = 0
        result: dict[tuple[int, int], int] = {}
        # Iterate in deterministic order (sorted by point).
        for pt in sorted(label_map):
            root = label_map[pt]
            if root not in root_to_canonical:
                root_to_canonical[root] = counter
                counter += 1
            result[pt] = root_to_canonical[root]
        return result

    norm_tiled = normalize(tiled_root)
    norm_direct = normalize(direct_root)

    # -- Compare ---------------------------------------------------------------
    all_points = sorted(set(norm_tiled.keys()) | set(norm_direct.keys()))
    total = len(all_points)

    # Count distinct components in each.
    tiled_components = len(set(norm_tiled.values())) if norm_tiled else 0
    direct_components = len(set(norm_direct.values())) if norm_direct else 0

    # Check membership equivalence: two points are "same component" in both
    # labelings iff their normalized labels agree.  But normalization depends
    # on iteration order — so instead compare by partition identity:
    # build equivalence classes and compare.
    #
    # Efficient approach: pick the tiled labeling as reference, build
    # mapping tiled_label → set of direct_labels.  If every tiled_label maps
    # to exactly one direct_label (and vice versa), the partitions are identical.

    tiled_to_direct: dict[int, set[int]] = {}
    direct_to_tiled: dict[int, set[int]] = {}
    mismatches: list[tuple[int, int]] = []

    for pt in all_points:
        tl = norm_tiled.get(pt)
        dl = norm_direct.get(pt)
        if tl is None or dl is None:
            # Point exists in one but not the other — should not happen
            # if both operate on the same alive mask.
            mismatches.append(pt)
            continue
        tiled_to_direct.setdefault(tl, set()).add(dl)
        direct_to_tiled.setdefault(dl, set()).add(tl)

    # Partition is equivalent iff every tiled label maps to exactly 1 direct label
    # and every direct label maps to exactly 1 tiled label.
    split_mismatches: list[tuple[int, int]] = []
    if not mismatches:
        # Check for splits: any tiled component maps to >1 direct components
        # (the tiled pipeline merged things it shouldn't have) or vice versa.
        bad_tiled = {t for t, ds in tiled_to_direct.items() if len(ds) > 1}
        bad_direct = {d for d, ts in direct_to_tiled.items() if len(ts) > 1}
        if bad_tiled or bad_direct:
            # Find example mismatch points.
            for pt in all_points:
                tl = norm_tiled[pt]
                dl = norm_direct[pt]
                if tl in bad_tiled or dl in bad_direct:
                    split_mismatches.append(pt)
                    if len(split_mismatches) >= 10:
                        break

    all_mismatches = mismatches + split_mismatches
    equivalent = len(all_mismatches) == 0 and tiled_components == direct_components

    return EquivalenceReport(
        equivalent=equivalent,
        tiled_components=tiled_components,
        direct_components=direct_components,
        total_points=total,
        mismatched_count=len(all_mismatches),
        example_mismatches=all_mismatches[:10],
    )


# ---------------------------------------------------------------------------
# Graph and test case helpers
# ---------------------------------------------------------------------------

def make_graph(n_cols: int, n_rows: int, r: float = math.sqrt(2)) -> Graph:
    """Create a full lattice graph sized for the given tile grid.

    Lattice dimensions: (n_cols * 256 + 1) × (n_rows * 256 + 1).
    The +1 is the fencepost: n tiles of 256 units produce n*256+1 points.
    """
    w = n_cols * TILE_SIZE + 1
    h = n_rows * TILE_SIZE + 1
    return Graph(w, h, r=r)


def run_case(
    name: str,
    graph: Graph,
    n_cols: int,
    n_rows: int,
    viz: Visualizer,
    case_idx: int,
    check_equivalence: bool = False,
    collar: int | None = None,
) -> tuple[bool, EquivalenceReport | None]:
    """Run one verification case.

    Returns (verdict_agree, equivalence_report).
    equivalence_report is None when check_equivalence is False.
    """
    print(f"\n{'='*60}")
    print(f"CASE {case_idx}: {name}")
    print(f"{'='*60}")
    print(f"Graph: {graph.width}x{graph.height}, "
          f"r={graph.r:.3f}, alive={graph.alive_count()}")
    if collar is not None:
        print(f"Collar: {collar}")

    # Tiled path.
    campaign = Campaign(graph, n_cols, n_rows, collar=collar)
    campaign_result = campaign.run(progress=True)

    # Direct path.
    direct_result = DirectVerifier(graph).run(progress=True)

    # Verdict comparison.
    agree = campaign_result.verdict == direct_result.verdict
    status = "PASS" if agree else "FAIL"
    print(f"\n  Tiled:  {campaign_result.verdict}")
    print(f"  Direct: {direct_result.verdict}")
    print(f"  Verdict: {status}")

    # Component equivalence.
    eq_report: EquivalenceReport | None = None
    if check_equivalence:
        eq_report = verify_component_equivalence(
            graph, campaign_result, direct_result,
        )
        eq_status = "PASS" if eq_report.equivalent else "FAIL"
        print(f"  Component equivalence: {eq_status}")
        print(f"    Tiled components:  {eq_report.tiled_components}")
        print(f"    Direct components: {eq_report.direct_components}")
        print(f"    Points compared:   {eq_report.total_points}")
        if eq_report.mismatched_count > 0:
            print(f"    Mismatched points: {eq_report.mismatched_count}")
            for pt in eq_report.example_mismatches[:5]:
                print(f"      ({pt[0]}, {pt[1]})")

    # Visualization.
    fname = f"case{case_idx}_{name.lower().replace(' ', '_').replace(',', '')}"
    path = viz.draw_verdict(
        case_name=f"Case {case_idx}: {name}",
        campaign_result=campaign_result,
        direct_result=direct_result,
        graph=graph,
        filename=fname,
    )
    print(f"  Visualization: {path}")

    return agree, eq_report


def main() -> None:
    full_mode = "--full" in sys.argv

    if full_mode:
        n_cols, n_rows = 64, 32
        print("FULL MODE: 64x32 tiles (16385x8193 lattice points)")
    else:
        n_cols, n_rows = 8, 4
        print("FAST MODE: 8x4 tiles (2049x1025 lattice points)")
        print("Pass --full for production-scale 64x32 grid.\n")

    viz = Visualizer(output_dir="output")
    results: list[tuple[str, bool, bool | None]] = []  # (name, verdict_ok, equiv_ok)
    t_start = time.perf_counter()

    # -- Case 1: Full lattice, no removals -> SPANNING --------------------
    graph1 = make_graph(n_cols, n_rows)
    ok, eq = run_case("Full lattice (expect SPANNING)", graph1, n_cols, n_rows, viz, 1)
    results.append(("Full lattice", ok, None))

    # -- Case 2: Horizontal moat -> MOAT ----------------------------------
    graph2 = make_graph(n_cols, n_rows)
    mid_y = graph2.height // 2
    graph2.remove_horizontal_band(mid_y - 1, mid_y + 1)
    ok, eq = run_case("Horizontal moat (expect MOAT)", graph2, n_cols, n_rows, viz, 2)
    results.append(("Horizontal moat", ok, None))

    # -- Case 3: Moat with bridge -> SPANNING ------------------------------
    graph3 = make_graph(n_cols, n_rows)
    mid_y = graph3.height // 2
    graph3.remove_horizontal_band(mid_y - 1, mid_y + 1)
    bridge_x = graph3.width // 2
    for y in range(mid_y - 1, mid_y + 2):
        graph3.restore_point(bridge_x, y)
    ok, eq = run_case("Moat with bridge (expect SPANNING)", graph3, n_cols, n_rows, viz, 3)
    results.append(("Moat with bridge", ok, None))

    # -- Case 4: Sparse random removal -> agreement test -------------------
    graph4 = make_graph(n_cols, n_rows)
    rng = random.Random(42)
    removal_rate = 0.20
    for y in range(graph4.height):
        for x in range(graph4.width):
            if rng.random() < removal_rate:
                graph4.remove_point(x, y)
    ok, eq = run_case("Sparse random 20% removal", graph4, n_cols, n_rows, viz, 4)
    results.append(("Sparse random", ok, None))

    # -- Case 5: Larger radius r=sqrt(5) with moat -> MOAT ----------------
    graph5 = make_graph(n_cols, n_rows, r=math.sqrt(5))
    mid_y = graph5.height // 2
    graph5.remove_horizontal_band(mid_y - 2, mid_y + 2)
    ok, eq = run_case("Radius sqrt(5) with moat (expect MOAT)", graph5, n_cols, n_rows, viz, 5)
    results.append(("Radius sqrt(5) moat", ok, None))

    # -- Case 6: Larger radius, full lattice -> SPANNING -------------------
    graph6 = make_graph(n_cols, n_rows, r=math.sqrt(5))
    ok, eq = run_case("Radius sqrt(5) full (expect SPANNING)", graph6, n_cols, n_rows, viz, 6)
    results.append(("Radius sqrt(5) full", ok, None))

    # -- Case 7: Sparse lattice, r=sqrt(5), 30% removal -------------------
    # This is the critical collar test: with r > sqrt(2), edges can jump
    # past the shared boundary row.  Without the collar, the tiled pipeline
    # misses these edges and produces WRONG component membership.
    graph7 = make_graph(n_cols, n_rows, r=math.sqrt(5))
    rng7 = random.Random(77)
    for y in range(graph7.height):
        for x in range(graph7.width):
            if rng7.random() < 0.30:
                graph7.remove_point(x, y)
    ok7, eq7 = run_case(
        "Sparse r=sqrt(5) 30% removal",
        graph7, n_cols, n_rows, viz, 7,
        check_equivalence=True,
    )
    eq7_ok = eq7.equivalent if eq7 else False
    results.append(("Sparse r=sqrt(5) 30%", ok7, eq7_ok))

    # -- Case 8: Sparse lattice, r=sqrt(10), 20% removal ------------------
    graph8 = make_graph(n_cols, n_rows, r=math.sqrt(10))
    rng8 = random.Random(88)
    for y in range(graph8.height):
        for x in range(graph8.width):
            if rng8.random() < 0.20:
                graph8.remove_point(x, y)
    ok8, eq8 = run_case(
        "Sparse r=sqrt(10) 20% removal",
        graph8, n_cols, n_rows, viz, 8,
        check_equivalence=True,
    )
    eq8_ok = eq8.equivalent if eq8 else False
    results.append(("Sparse r=sqrt(10) 20%", ok8, eq8_ok))

    # -- Case 9: Adversarial boundary removal ------------------------------
    # Remove every other point ON tile boundaries, r=sqrt(5).
    # This stress-tests the collar: boundary points that mediate
    # cross-tile connectivity are deliberately thinned out.
    graph9 = make_graph(n_cols, n_rows, r=math.sqrt(5))
    rng9 = random.Random(99)
    # Remove every other point on every tile boundary row and column.
    for col_idx in range(n_cols + 1):
        bx = col_idx * TILE_SIZE
        if bx >= graph9.width:
            continue
        for y in range(graph9.height):
            if y % 2 == 0:
                graph9.remove_point(bx, y)
    for row_idx in range(n_rows + 1):
        by = row_idx * TILE_SIZE
        if by >= graph9.height:
            continue
        for x in range(graph9.width):
            if x % 2 == 0:
                graph9.remove_point(x, by)
    ok9, eq9 = run_case(
        "Adversarial boundary removal r=sqrt(5)",
        graph9, n_cols, n_rows, viz, 9,
        check_equivalence=True,
    )
    eq9_ok = eq9.equivalent if eq9 else False
    results.append(("Adversarial boundary", ok9, eq9_ok))

    # -- Summary ----------------------------------------------------------
    elapsed = time.perf_counter() - t_start
    print(f"\n{'='*60}")
    print("VERIFICATION SUMMARY")
    print(f"{'='*60}")
    all_pass = True
    for name, v_ok, e_ok in results:
        parts = []
        parts.append(f"verdict={'PASS' if v_ok else 'FAIL'}")
        if e_ok is not None:
            parts.append(f"equiv={'PASS' if e_ok else 'FAIL'}")
        status_str = ", ".join(parts)
        combined_ok = v_ok and (e_ok is None or e_ok)
        symbol = "+" if combined_ok else "X"
        print(f"  [{symbol}] {name}: {status_str}")
        if not combined_ok:
            all_pass = False

    print(f"\nTotal time: {elapsed:.1f}s")
    print(f"Output: {viz.output_dir.resolve()}/")

    if all_pass:
        print("\nAll cases PASSED. Tile composition is lossless.")
    else:
        print("\nSome cases FAILED. Tile composition has a bug.")
        sys.exit(1)


if __name__ == "__main__":
    main()
