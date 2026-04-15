"""
direct.py — Direct (Brute-Force) Verification

The ground-truth oracle. Runs union-find over the ENTIRE graph without
any tiling — just iterates all alive points, checks their neighbors,
and unites them. Then checks if any bottom-edge point connects to any
top-edge point.

This is the "obvious" algorithm that is clearly correct but doesn't
scale to GPU. The Campaign (tiled path) is the clever algorithm that
DOES scale — and the whole point of this suite is to prove they agree.

MEMORY NOTE:
Even though the full graph may have 134M+ points, we never materialize
the edge list. We iterate points in scan order, checking neighbors on
the fly. The union-find array is the only large allocation (~134M ints
for the full grid, ~500 MB). For the reduced test grids this is trivial.
"""

from __future__ import annotations

import time
from dataclasses import dataclass

import numpy as np

from graph import Graph
from uf import UnionFind


@dataclass
class DirectResult:
    """Output of the direct verification."""
    verdict: str       # "SPANNING" or "MOAT"
    n_points: int      # alive points processed
    n_components: int  # distinct connected components
    elapsed_s: float   # wall-clock time in seconds
    # Opaque handles for component-equivalence verification.
    _uf: object | None = None       # UnionFind instance
    _point_idx: object | None = None  # np.ndarray mapping (y,x) → UF index


class DirectVerifier:
    """Brute-force union-find on the full graph. The ground truth.

    No tiling, no composition — just raw connectivity over all edges.
    The result is the authoritative answer against which the tiled
    pipeline is validated.
    """

    def __init__(self, graph: Graph) -> None:
        self.graph = graph

    def run(self, progress: bool = True) -> DirectResult:
        """Run brute-force union-find over the entire graph.

        Steps:
        1. Assign a flat index to every alive point.
        2. Iterate all alive points; for each, check neighbors and unite.
        3. Collect roots of bottom-edge points and top-edge points.
        4. SPANNING if intersection is non-empty.

        The neighbor iteration uses only "forward" offsets (positive dx,
        or dx=0 with positive dy) to avoid processing each edge twice.
        But for correctness with the Graph's removed-edge set, we check
        all offsets — the UF's idempotent union handles duplicates cheaply.
        """
        t0 = time.perf_counter()
        graph = self.graph
        w, h = graph.width, graph.height

        if progress:
            print(f"[Direct] Graph: {w}×{h} = {w * h} lattice points")

        # Step 1: assign flat indices to alive points.
        # point_idx[y, x] = UF index if alive, -1 otherwise.
        # Using a 2D numpy array for fast lookup.
        point_idx = np.full((h, w), -1, dtype=np.int32)
        idx_counter = 0

        for y in range(h):
            for x in range(w):
                if graph.alive[y, x]:
                    point_idx[y, x] = idx_counter
                    idx_counter += 1

        n_points = idx_counter
        if progress:
            print(f"[Direct] Alive points: {n_points}")

        if n_points == 0:
            return DirectResult(
                verdict="MOAT", n_points=0,
                n_components=0, elapsed_s=time.perf_counter() - t0,
            )

        # Step 2: iterate and unite.
        uf = UnionFind(n_points)
        offsets = graph.offsets

        for y in range(h):
            if progress and y % max(1, h // 20) == 0:
                pct = 100 * y / h
                print(f"  scanning row {y}/{h} ({pct:.0f}%)",
                      flush=True)
            for x in range(w):
                if not graph.alive[y, x]:
                    continue
                idx = point_idx[y, x]
                # Check all neighbor offsets. Union is idempotent so
                # processing each edge from both endpoints is fine.
                for dx, dy in offsets:
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h:
                        n_idx = point_idx[ny, nx]
                        if n_idx >= 0:
                            # Also respect explicitly removed edges.
                            if graph.edge_exists(x, y, nx, ny):
                                uf.union(idx, n_idx)

        # Step 3: collect boundary roots.
        # Inner boundary = bottom row (y = 0).
        inner_roots: set[int] = set()
        for x in range(w):
            idx = point_idx[0, x]
            if idx >= 0:
                inner_roots.add(uf.find(idx))

        # Outer boundary = top row (y = h - 1).
        outer_roots: set[int] = set()
        for x in range(w):
            idx = point_idx[h - 1, x]
            if idx >= 0:
                outer_roots.add(uf.find(idx))

        # Step 4: verdict.
        spanning = bool(inner_roots & outer_roots)

        elapsed = time.perf_counter() - t0
        n_components = uf.component_count()

        if progress:
            print(f"[Direct] Verdict: {'SPANNING' if spanning else 'MOAT'} "
                  f"({elapsed:.2f}s, {n_components} components)")

        return DirectResult(
            verdict="SPANNING" if spanning else "MOAT",
            n_points=n_points,
            n_components=n_components,
            elapsed_s=elapsed,
            _uf=uf,
            _point_idx=point_idx,
        )
