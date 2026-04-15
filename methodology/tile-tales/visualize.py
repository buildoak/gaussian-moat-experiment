"""
visualize.py — Visualization for Tile Composition Verification

Matplotlib rendering of graphs, tiles, groups, and verdicts.
Designed for paper-quality output with clear colors, labels, and annotations.

For large grids (64×32 tiles = 16384×8192 points), direct point plotting
is impractical. We provide two modes:
- Overview: downsampled heatmap showing alive/dead regions and tile boundaries.
- Detail: full-resolution rendering of a small region (single tile or 2×2 block).

Color scheme uses a categorical palette for groups and red/green for verdicts.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import TYPE_CHECKING

import matplotlib
matplotlib.use("Agg")  # Non-interactive backend for headless rendering.

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
from matplotlib.colors import ListedColormap

if TYPE_CHECKING:
    from campaign import CampaignResult
    from direct import DirectResult
    from graph import Graph
    from grid import Grid
    from tile import Tile, TILE_SIZE


# A 20-color categorical palette — enough for most per-tile group counts.
# Deliberately high-contrast for readability.
GROUP_COLORS = [
    "#e6194b", "#3cb44b", "#4363d8", "#f58231", "#911eb4",
    "#42d4f4", "#f032e6", "#bfef45", "#fabed4", "#469990",
    "#dcbeff", "#9A6324", "#fffac8", "#800000", "#aaffc3",
    "#808000", "#ffd8b1", "#000075", "#a9a9a9", "#000000",
]


class Visualizer:
    """Matplotlib rendering of graphs, tiles, groups, and verdicts."""

    def __init__(self, output_dir: str = "output") -> None:
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)

    def _save(self, fig: plt.Figure, name: str) -> str:
        """Save figure and return the file path."""
        path = self.output_dir / f"{name}.png"
        fig.savefig(path, dpi=150, bbox_inches="tight", facecolor="white")
        plt.close(fig)
        return str(path)

    # -- Graph overview ---------------------------------------------------

    def draw_graph_overview(self, graph: "Graph", title: str = "Graph",
                            filename: str = "graph_overview") -> str:
        """Draw a downsampled overview of the graph's alive mask.

        White = alive, black = dead. Tile boundaries shown as gray grid.
        Suitable for large grids where per-point rendering is impractical.
        """
        from tile import TILE_SIZE

        fig, ax = plt.subplots(figsize=(12, 6))

        # Show the alive mask as an image. Origin='lower' so y=0 is at bottom.
        ax.imshow(graph.alive.astype(np.uint8), cmap="gray",
                  origin="lower", aspect="auto", interpolation="nearest")

        # Draw tile boundaries.
        for x in range(0, graph.width, TILE_SIZE):
            ax.axvline(x, color="#cccccc", linewidth=0.3, alpha=0.5)
        for y in range(0, graph.height, TILE_SIZE):
            ax.axhline(y, color="#cccccc", linewidth=0.3, alpha=0.5)

        ax.set_title(title, fontsize=14)
        ax.set_xlabel("x (lattice columns)")
        ax.set_ylabel("y (lattice rows)")
        return self._save(fig, filename)

    # -- Detail view: single tile -----------------------------------------

    def draw_tile_detail(self, tile: "Tile", filename: str | None = None) -> str:
        """Draw a single tile at full resolution: points, edges, and group colors.

        Each connected component gets a distinct color. Edges drawn as thin lines.
        Face-prime ports are highlighted with markers.
        """
        from tile import Face

        result = tile.result
        graph = tile.graph
        x_lo, x_hi = tile.x_lo, tile.x_hi
        y_lo, y_hi = tile.y_lo, tile.y_hi

        fig, ax = plt.subplots(figsize=(10, 10))

        # Collect alive points and their groups.
        # Rebuild point→group mapping (same logic as Tile.analyze).
        point_group: dict[tuple[int, int], int] = {}
        point_to_idx: dict[tuple[int, int], int] = {}
        idx_to_point: list[tuple[int, int]] = []

        for y in range(y_lo, y_hi + 1):
            for x in range(x_lo, x_hi + 1):
                if graph.alive[y, x]:
                    point_to_idx[(x, y)] = len(idx_to_point)
                    idx_to_point.append((x, y))

        # Re-run UF to get consistent group labels.
        from uf import UnionFind
        if idx_to_point:
            uf = UnionFind(len(idx_to_point))
            for idx, (px, py) in enumerate(idx_to_point):
                for nx, ny in graph.neighbors(px, py):
                    if x_lo <= nx <= x_hi and y_lo <= ny <= y_hi:
                        n_idx = point_to_idx.get((nx, ny))
                        if n_idx is not None:
                            uf.union(idx, n_idx)

            root_to_group: dict[int, int] = {}
            gid_counter = 0
            for idx, pt in enumerate(idx_to_point):
                root = uf.find(idx)
                if root not in root_to_group:
                    gid_counter += 1
                    root_to_group[root] = gid_counter
                point_group[pt] = root_to_group[root]

        # Draw edges (thin gray lines).
        for px, py in idx_to_point:
            for nx, ny in graph.neighbors(px, py):
                if x_lo <= nx <= x_hi and y_lo <= ny <= y_hi:
                    if (nx, ny) in point_to_idx:
                        # Only draw each edge once (forward direction).
                        if (nx, ny) > (px, py):
                            ax.plot([px, nx], [py, ny], color="#cccccc",
                                    linewidth=0.3, zorder=1)

        # Draw points colored by group.
        for pt, gid in point_group.items():
            color = GROUP_COLORS[(gid - 1) % len(GROUP_COLORS)]
            ax.plot(pt[0], pt[1], ".", color=color, markersize=2, zorder=2)

        # Highlight face-prime ports with larger markers.
        for face in Face:
            for port in result.face_ports[face]:
                ax.plot(port.abs_x, port.abs_y, "s",
                        color=GROUP_COLORS[(port.group_id - 1) % len(GROUP_COLORS)],
                        markersize=5, markeredgecolor="black",
                        markeredgewidth=0.5, zorder=3)

        # Draw tile boundary rectangles.
        # Nominal boundary in solid blue.
        rect = mpatches.Rectangle(
            (x_lo - 0.5, y_lo - 0.5), x_hi - x_lo + 1, y_hi - y_lo + 1,
            linewidth=2, edgecolor="blue", facecolor="none", zorder=4,
        )
        ax.add_patch(rect)

        # Sieve boundary (collar) in dashed orange if collar > 0.
        if hasattr(tile, 'collar') and tile.collar > 0:
            sx_lo, sx_hi = tile.sx_lo, tile.sx_hi
            sy_lo, sy_hi = tile.sy_lo, tile.sy_hi
            if (sx_lo != x_lo or sx_hi != x_hi or
                    sy_lo != y_lo or sy_hi != y_hi):
                sieve_rect = mpatches.Rectangle(
                    (sx_lo - 0.5, sy_lo - 0.5),
                    sx_hi - sx_lo + 1, sy_hi - sy_lo + 1,
                    linewidth=1.5, edgecolor="orange", facecolor="none",
                    linestyle="--", zorder=4, label="sieve (collar)",
                )
                ax.add_patch(sieve_rect)

        ax.set_title(f"Tile ({tile.col}, {tile.row}) — "
                     f"{result.group_count} groups, "
                     f"{result.alive_count} alive points",
                     fontsize=12)
        ax.set_xlabel("x")
        ax.set_ylabel("y")
        ax.set_xlim(x_lo - 1, x_hi + 1)
        ax.set_ylim(y_lo - 1, y_hi + 1)
        ax.set_aspect("equal")

        fname = filename or f"tile_{tile.col}_{tile.row}"
        return self._save(fig, fname)

    # -- Grid overview with groups ----------------------------------------

    def draw_grid_groups(self, grid: "Grid", title: str = "Grid Groups",
                         filename: str = "grid_groups") -> str:
        """Draw a downsampled view of the grid with group coloring.

        Each tile is rendered as a small block. Color = number of groups
        (heatmap). Good for seeing where complexity concentrates.
        """
        from tile import TILE_SIZE

        n_cols, n_rows = grid.n_cols, grid.n_rows
        group_map = np.zeros((n_rows, n_cols), dtype=np.int32)

        for col in range(n_cols):
            for row in range(n_rows):
                group_map[row, col] = grid.tile_at(col, row).result.group_count

        fig, ax = plt.subplots(figsize=(max(8, n_cols // 4), max(4, n_rows // 4)))
        im = ax.imshow(group_map, origin="lower", cmap="YlOrRd",
                       aspect="auto", interpolation="nearest")
        plt.colorbar(im, ax=ax, label="Groups per tile")
        ax.set_title(title, fontsize=14)
        ax.set_xlabel("Tower (column)")
        ax.set_ylabel("Row")
        return self._save(fig, filename)

    # -- Verdict comparison -----------------------------------------------

    def draw_verdict(self, case_name: str,
                     campaign_result: "CampaignResult",
                     direct_result: "DirectResult",
                     graph: "Graph",
                     filename: str = "verdict") -> str:
        """Side-by-side comparison of tiled and direct verdicts.

        Left panel: graph overview. Right panel: verdict summary table.
        Green background if verdicts agree, red if they disagree.
        """
        from tile import TILE_SIZE

        fig, (ax_graph, ax_table) = plt.subplots(
            1, 2, figsize=(16, 6),
            gridspec_kw={"width_ratios": [2, 1]},
        )

        # Left: graph overview.
        ax_graph.imshow(graph.alive.astype(np.uint8), cmap="gray",
                        origin="lower", aspect="auto", interpolation="nearest")
        for x in range(0, graph.width, TILE_SIZE):
            ax_graph.axvline(x, color="#cccccc", linewidth=0.3, alpha=0.5)
        for y in range(0, graph.height, TILE_SIZE):
            ax_graph.axhline(y, color="#cccccc", linewidth=0.3, alpha=0.5)
        ax_graph.set_title(f"{case_name} — Graph", fontsize=12)
        ax_graph.set_xlabel("x")
        ax_graph.set_ylabel("y")

        # Right: verdict table.
        agree = campaign_result.verdict == direct_result.verdict
        bg_color = "#d4edda" if agree else "#f8d7da"  # green / red tint
        ax_table.set_facecolor(bg_color)

        rows = [
            ["Tiled verdict", campaign_result.verdict],
            ["Direct verdict", direct_result.verdict],
            ["Agreement", "YES" if agree else "NO"],
            ["", ""],
            ["Tiles", f"{campaign_result.tile_count}"],
            ["Per-tile groups", f"{campaign_result.total_groups}"],
            ["Cross-tile merges", f"{campaign_result.merge_count}"],
            ["Tiled time", f"{campaign_result.elapsed_s:.2f}s"],
            ["Direct time", f"{direct_result.elapsed_s:.2f}s"],
            ["Alive points", f"{direct_result.n_points}"],
        ]

        table = ax_table.table(
            cellText=rows, colLabels=["Metric", "Value"],
            loc="center", cellLoc="left",
        )
        table.auto_set_font_size(False)
        table.set_fontsize(10)
        table.scale(1, 1.5)
        ax_table.axis("off")
        ax_table.set_title(
            f"{'PASS' if agree else 'FAIL'} — Verdicts {'agree' if agree else 'DISAGREE'}",
            fontsize=14, fontweight="bold",
            color="green" if agree else "red",
        )

        fig.suptitle(case_name, fontsize=16, fontweight="bold")
        fig.tight_layout()
        return self._save(fig, filename)

    # -- Composition detail -----------------------------------------------

    def draw_composition_detail(
        self, grid: "Grid",
        col_range: tuple[int, int] = (0, 2),
        row_range: tuple[int, int] = (0, 2),
        filename: str = "composition_detail",
    ) -> str:
        """Draw a small region of tiles showing face-prime connections.

        Shows tile boundaries, group colors, and port positions for a
        rectangular sub-region of the grid. Useful for understanding
        how composition works at the boundary level.
        """
        from tile import Face, TILE_SIZE

        col_lo, col_hi = col_range
        row_lo, row_hi = row_range

        # Lattice bounds for the sub-region.
        lx_lo = col_lo * TILE_SIZE
        lx_hi = col_hi * TILE_SIZE + TILE_SIZE
        ly_lo = row_lo * TILE_SIZE
        ly_hi = row_hi * TILE_SIZE + TILE_SIZE

        fig, ax = plt.subplots(figsize=(12, 12))
        graph = grid.graph

        # Draw alive points as small dots.
        for y in range(ly_lo, min(ly_hi + 1, graph.height)):
            for x in range(lx_lo, min(lx_hi + 1, graph.width)):
                if graph.alive[y, x]:
                    ax.plot(x, y, ".", color="#cccccc", markersize=0.5, zorder=1)

        # Draw tile boundaries and ports.
        for col in range(col_lo, col_hi):
            for row in range(row_lo, row_hi):
                if col >= grid.n_cols or row >= grid.n_rows:
                    continue
                tile = grid.tile_at(col, row)
                result = tile.result

                # Tile boundary rectangle (nominal).
                rect = mpatches.Rectangle(
                    (tile.x_lo - 0.5, tile.y_lo - 0.5),
                    TILE_SIZE + 1, TILE_SIZE + 1,
                    linewidth=1.5, edgecolor="blue", facecolor="none",
                    linestyle="--", zorder=3,
                )
                ax.add_patch(rect)

                # Sieve boundary (collar) in dashed orange if present.
                if hasattr(tile, 'collar') and tile.collar > 0:
                    sx_lo, sx_hi = tile.sx_lo, tile.sx_hi
                    sy_lo, sy_hi = tile.sy_lo, tile.sy_hi
                    if (sx_lo != tile.x_lo or sx_hi != tile.x_hi or
                            sy_lo != tile.y_lo or sy_hi != tile.y_hi):
                        sieve_rect = mpatches.Rectangle(
                            (sx_lo - 0.5, sy_lo - 0.5),
                            sx_hi - sx_lo + 1, sy_hi - sy_lo + 1,
                            linewidth=1, edgecolor="orange",
                            facecolor="none", linestyle=":",
                            zorder=3, alpha=0.7,
                        )
                        ax.add_patch(sieve_rect)

                # Tile label.
                cx = (tile.x_lo + tile.x_hi) / 2
                cy = (tile.y_lo + tile.y_hi) / 2
                ax.text(cx, cy, f"({col},{row})\n{result.group_count}g",
                        ha="center", va="center", fontsize=8,
                        color="blue", zorder=4)

                # Draw ports as colored squares on face edges.
                for face in Face:
                    for port in result.face_ports[face]:
                        color = GROUP_COLORS[(port.group_id - 1) % len(GROUP_COLORS)]
                        ax.plot(port.abs_x, port.abs_y, "s",
                                color=color, markersize=4,
                                markeredgecolor="black", markeredgewidth=0.3,
                                zorder=5)

        ax.set_title(f"Composition Detail — tiles [{col_lo},{col_hi})×[{row_lo},{row_hi})",
                     fontsize=12)
        ax.set_xlabel("x")
        ax.set_ylabel("y")
        ax.set_xlim(lx_lo - 5, lx_hi + 5)
        ax.set_ylim(ly_lo - 5, ly_hi + 5)
        ax.set_aspect("equal")
        return self._save(fig, filename)
