"""
campaign.py — Full Tiled Pipeline Orchestrator

Orchestrates the complete tiled verification path:
    graph → grid → per-tile analysis → compositor → verdict

This is one of the two paths in the verification suite. The other is
DirectVerifier (direct.py), which runs brute-force union-find on the
full unpartitioned graph. If both paths produce the same verdict,
the tile abstraction is proven lossless for that input.

The Campaign is the "production path" — it mirrors what the CUDA pipeline
does (partition → per-tile GPU kernel → host-side composition), except
implemented in clear Python for verification.
"""

from __future__ import annotations

import time
from dataclasses import dataclass

from compositor import Compositor, CompositorResult
from graph import Graph
from grid import Grid


@dataclass
class CampaignResult:
    """Full output of the tiled pipeline."""
    verdict: str           # "SPANNING" or "MOAT"
    n_cols: int            # tile columns (towers)
    n_rows: int            # tile rows per tower
    tile_count: int        # total tiles
    total_groups: int      # per-tile groups before merging
    merge_count: int       # cross-tile union operations
    global_components: int # distinct components after composition
    elapsed_s: float       # wall-clock time in seconds
    # Opaque handles for component-equivalence verification.
    _compositor: object | None = None  # Compositor instance
    _grid: object | None = None        # Grid instance


class Campaign:
    """Orchestrates the full pipeline: graph → grid → tiles → compositor → verdict.

    Usage:
        result = Campaign(graph, n_cols=8, n_rows=4).run()
        print(result.verdict)  # "SPANNING" or "MOAT"
    """

    def __init__(self, graph: Graph, n_cols: int, n_rows: int,
                 collar: int | None = None) -> None:
        """Initialize the campaign.

        Args:
            graph:  the lattice graph to analyze
            n_cols: number of tile columns (towers)
            n_rows: number of tile rows per tower
            collar: sieve extension per tile (default: ceil(r))
        """
        self.graph = graph
        self.n_cols = n_cols
        self.n_rows = n_rows
        self.collar = collar

    def run(self, progress: bool = True) -> CampaignResult:
        """Execute the full tiled pipeline.

        Steps:
        1. Partition graph into a Grid of tiles.
        2. Analyze each tile (compute groups + face ports).
        3. Run the Compositor to merge groups across tile boundaries.
        4. Return the verdict.
        """
        t0 = time.perf_counter()

        if progress:
            print(f"[Campaign] Grid: {self.n_cols}×{self.n_rows} tiles "
                  f"({self.n_cols * self.n_rows} total)")

        # Step 1: create the grid.
        grid = Grid(self.graph, self.n_cols, self.n_rows, collar=self.collar)
        self._grid = grid  # Expose for component tracing.

        # Step 2: analyze all tiles.
        if progress:
            print("[Campaign] Analyzing tiles...")
        grid.analyze_all(progress=progress)

        total_groups = grid.total_groups()
        if progress:
            print(f"[Campaign] Total per-tile groups: {total_groups}")

        # Step 3: compose.
        if progress:
            print("[Campaign] Composing tiles...")
        compositor = Compositor(grid)
        comp_result: CompositorResult = compositor.compose()

        elapsed = time.perf_counter() - t0
        if progress:
            print(f"[Campaign] Verdict: {comp_result.verdict} "
                  f"({elapsed:.2f}s, {comp_result.merge_count} merges)")

        return CampaignResult(
            verdict=comp_result.verdict,
            n_cols=self.n_cols,
            n_rows=self.n_rows,
            tile_count=self.n_cols * self.n_rows,
            total_groups=comp_result.total_groups,
            merge_count=comp_result.merge_count,
            global_components=comp_result.global_components,
            elapsed_s=elapsed,
            _compositor=compositor,
            _grid=grid,
        )
