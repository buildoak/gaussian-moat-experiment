"""Visualization for Gaussian moat experiments."""

from __future__ import annotations

import math
import os
from typing import Dict, List, Optional, Set, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

from .primes import gaussian_primes_in_rect
from .tiling import TileOperator
from .moat_finder import MoatResult

OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "output")
os.makedirs(OUTPUT_DIR, exist_ok=True)


def plot_primes_with_tiles(
    result: MoatResult,
    filename: str = "primes_tiles.png",
    max_plot_radius: Optional[int] = None,
) -> str:
    """Plot Gaussian primes with tile grid overlay, colored by connectivity to origin.

    Args:
        result: MoatResult from a moat search.
        filename: output filename.
        max_plot_radius: limit plot range (useful for large R).

    Returns:
        Path to saved plot.
    """
    R = max_plot_radius or result.radius
    k_sq = result.k_sq
    tile_size = result.tile_size

    # Generate primes for plotting
    primes = gaussian_primes_in_rect(-R, R, -R, R)

    fig, ax = plt.subplots(1, 1, figsize=(10, 10))

    # Draw tile grid
    for x in range(-R, R + 1, tile_size):
        ax.axvline(x, color="lightgray", linewidth=0.3, zorder=0)
    for y in range(-R, R + 1, tile_size):
        ax.axhline(y, color="lightgray", linewidth=0.3, zorder=0)

    # Color primes: find which are connected to origin via BFS
    if len(primes) > 0:
        # Build adjacency for the plot region and find origin component
        from .unionfind import UnionFind

        n = len(primes)
        uf = UnionFind(n)
        collar = math.ceil(math.sqrt(k_sq))
        cell_side = max(collar, 1)

        prime_list = [(int(primes[i, 0]), int(primes[i, 1])) for i in range(n)]
        cells: Dict[Tuple[int, int], List[int]] = {}
        for idx, (a, b) in enumerate(prime_list):
            cx, cy = a // cell_side, b // cell_side
            cells.setdefault((cx, cy), []).append(idx)

        for idx, (a, b) in enumerate(prime_list):
            cx, cy = a // cell_side, b // cell_side
            for dcx in range(-2, 3):
                for dcy in range(-2, 3):
                    nbr_cell = (cx + dcx, cy + dcy)
                    if nbr_cell not in cells:
                        continue
                    for jdx in cells[nbr_cell]:
                        if jdx <= idx:
                            continue
                        da = prime_list[jdx][0] - a
                        db = prime_list[jdx][1] - b
                        if da * da + db * db <= k_sq:
                            uf.union(idx, jdx)

        # Find origin component
        origin_neighbors = [
            idx for idx, (a, b) in enumerate(prime_list) if a * a + b * b <= k_sq
        ]
        origin_comp = None
        if origin_neighbors:
            for i in range(1, len(origin_neighbors)):
                uf.union(origin_neighbors[0], origin_neighbors[i])
            origin_comp = uf.find(origin_neighbors[0])

        # Color: origin component = red, others = blue
        colors = []
        for idx in range(n):
            if origin_comp is not None and uf.find(idx) == origin_comp:
                colors.append("red")
            else:
                colors.append("steelblue")

        ax.scatter(
            primes[:, 0],
            primes[:, 1],
            c=colors,
            s=max(1, 100 // (R // 10 + 1)),
            alpha=0.7,
            zorder=2,
        )

    # Mark origin
    ax.plot(0, 0, "k*", markersize=12, zorder=5)

    # Draw step circle at origin
    step = math.sqrt(k_sq)
    circle = plt.Circle((0, 0), step, fill=False, color="green", linewidth=1.5, linestyle="--", zorder=3)
    ax.add_patch(circle)

    ax.set_xlim(-R - 1, R + 1)
    ax.set_ylim(-R - 1, R + 1)
    ax.set_aspect("equal")
    ax.set_title(
        f"Gaussian Primes — k²={k_sq} (step={step:.2f}), R={R}\n"
        f"Red = origin component, Blue = other, Moat={'YES' if result.moat_found else 'NO'}"
    )
    ax.set_xlabel("Re(z)")
    ax.set_ylabel("Im(z)")

    path = os.path.join(OUTPUT_DIR, filename)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_frontier(
    result: MoatResult,
    filename: str = "frontier.png",
    max_plot_radius: Optional[int] = None,
) -> str:
    """Plot the boundary/frontier of the origin's connected component.

    Shows just the origin component primes and highlights the outermost ones.
    """
    R = max_plot_radius or result.radius
    k_sq = result.k_sq

    primes = gaussian_primes_in_rect(-R, R, -R, R)

    fig, ax = plt.subplots(1, 1, figsize=(10, 10))

    if len(primes) > 0:
        from .unionfind import UnionFind

        n = len(primes)
        uf = UnionFind(n)
        collar = math.ceil(math.sqrt(k_sq))
        cell_side = max(collar, 1)

        prime_list = [(int(primes[i, 0]), int(primes[i, 1])) for i in range(n)]
        cells: Dict[Tuple[int, int], List[int]] = {}
        for idx, (a, b) in enumerate(prime_list):
            cx, cy = a // cell_side, b // cell_side
            cells.setdefault((cx, cy), []).append(idx)

        for idx, (a, b) in enumerate(prime_list):
            cx, cy = a // cell_side, b // cell_side
            for dcx in range(-2, 3):
                for dcy in range(-2, 3):
                    nbr_cell = (cx + dcx, cy + dcy)
                    if nbr_cell not in cells:
                        continue
                    for jdx in cells[nbr_cell]:
                        if jdx <= idx:
                            continue
                        da = prime_list[jdx][0] - a
                        db = prime_list[jdx][1] - b
                        if da * da + db * db <= k_sq:
                            uf.union(idx, jdx)

        origin_neighbors = [
            idx for idx, (a, b) in enumerate(prime_list) if a * a + b * b <= k_sq
        ]
        if origin_neighbors:
            for i in range(1, len(origin_neighbors)):
                uf.union(origin_neighbors[0], origin_neighbors[i])
            origin_comp = uf.find(origin_neighbors[0])

            # Extract origin component
            comp_mask = np.array([uf.find(i) == origin_comp for i in range(n)])
            comp_primes = primes[comp_mask]

            if len(comp_primes) > 0:
                # Find frontier: primes in origin component at max distance from origin
                dists = np.sqrt(comp_primes[:, 0].astype(float) ** 2 + comp_primes[:, 1].astype(float) ** 2)
                max_dist = np.max(dists)
                frontier_mask = dists > max_dist * 0.9

                # Plot interior
                ax.scatter(
                    comp_primes[~frontier_mask, 0],
                    comp_primes[~frontier_mask, 1],
                    c="salmon",
                    s=max(2, 80 // (R // 10 + 1)),
                    alpha=0.5,
                    label="Interior",
                    zorder=2,
                )
                # Plot frontier
                ax.scatter(
                    comp_primes[frontier_mask, 0],
                    comp_primes[frontier_mask, 1],
                    c="darkred",
                    s=max(3, 100 // (R // 10 + 1)),
                    alpha=0.9,
                    label="Frontier (>90% max dist)",
                    zorder=3,
                )

                # Draw max-distance circle
                circle = plt.Circle(
                    (0, 0), max_dist, fill=False, color="red",
                    linewidth=1, linestyle=":", zorder=4,
                )
                ax.add_patch(circle)

                ax.set_title(
                    f"Origin Component Frontier — k²={k_sq}, R={R}\n"
                    f"Component size: {len(comp_primes)}, Max reach: {max_dist:.1f}"
                )
            else:
                ax.set_title(f"Origin Component — EMPTY (moated)")
        else:
            ax.set_title(f"No primes near origin — trivially moated")

    ax.plot(0, 0, "k*", markersize=12, zorder=5)
    ax.set_xlim(-R - 1, R + 1)
    ax.set_ylim(-R - 1, R + 1)
    ax.set_aspect("equal")
    ax.set_xlabel("Re(z)")
    ax.set_ylabel("Im(z)")
    ax.legend(loc="upper right")

    path = os.path.join(OUTPUT_DIR, filename)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return path


def plot_composition_tree(
    n_rows: int, n_cols: int, n_levels: int,
    filename: str = "composition_tree.png",
) -> str:
    """Plot a schematic of the hierarchical composition tree."""
    fig, ax = plt.subplots(1, 1, figsize=(12, 6))

    # Draw bottom level tiles
    max_tiles = min(n_rows * n_cols, 32)  # cap for readability
    cols_show = min(n_cols, 8)
    rows_show = min(n_rows, 4)

    y_offset = 0
    level_heights = []

    # Level 0: individual tiles
    for r in range(rows_show):
        for c in range(cols_show):
            rect = mpatches.FancyBboxPatch(
                (c * 1.5, y_offset + r * 1.2), 1.0, 0.8,
                boxstyle="round,pad=0.05", facecolor="lightblue",
                edgecolor="navy", linewidth=0.5,
            )
            ax.add_patch(rect)
            ax.text(c * 1.5 + 0.5, y_offset + r * 1.2 + 0.4, f"T{r},{c}",
                    ha="center", va="center", fontsize=6)

    level_heights.append(y_offset)
    y_offset += rows_show * 1.2 + 0.5

    # Show merge levels schematically
    cur_cols, cur_rows = cols_show, rows_show
    colors = ["lightyellow", "lightgreen", "lightsalmon", "plum"]
    level = 0
    while cur_cols > 1 or cur_rows > 1:
        color = colors[level % len(colors)]
        if cur_cols > 1:
            new_cols = (cur_cols + 1) // 2
            for r in range(cur_rows):
                for c in range(new_cols):
                    rect = mpatches.FancyBboxPatch(
                        (c * 1.5 * (cols_show / new_cols), y_offset + r * 1.2),
                        1.0 * (cols_show / new_cols) - 0.2, 0.8,
                        boxstyle="round,pad=0.05", facecolor=color,
                        edgecolor="black", linewidth=0.5,
                    )
                    ax.add_patch(rect)
            cur_cols = new_cols
            y_offset += cur_rows * 1.2 + 0.3

        if cur_rows > 1:
            new_rows = (cur_rows + 1) // 2
            for r in range(new_rows):
                for c in range(cur_cols):
                    rect = mpatches.FancyBboxPatch(
                        (c * 1.5 * (cols_show / cur_cols), y_offset + r * 1.2),
                        1.0 * (cols_show / cur_cols) - 0.2, 0.8,
                        boxstyle="round,pad=0.05", facecolor=color,
                        edgecolor="black", linewidth=0.5,
                    )
                    ax.add_patch(rect)
            cur_rows = new_rows
            y_offset += cur_rows * 1.2 + 0.3

        level += 1

    ax.set_xlim(-0.5, cols_show * 1.5 + 0.5)
    ax.set_ylim(-0.5, y_offset + 1)
    ax.set_aspect("equal")
    ax.set_title(f"Hierarchical Composition Tree\n{n_rows}x{n_cols} tiles -> {n_levels} levels")
    ax.axis("off")

    path = os.path.join(OUTPUT_DIR, filename)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return path
