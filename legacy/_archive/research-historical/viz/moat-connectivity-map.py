#!/usr/bin/env python3
"""
Gaussian prime connectivity map around a moat region.

Visualizes per-tile prime connectivity from ISE --export-detail output.
Each prime is colored by the face-reachability of its connected component:
  - Green:  component spans inner-to-outer (IO-connected)
  - Blue:   component reaches inner face only
  - Red/orange: component reaches outer face only
  - Dark:   component reaches neither inner nor outer face

Usage:
    # Generate data first:
    ise --k-squared 26 --r-min 1013000 --r-max 1019000 \
        --tile-width 1000 --tile-height 1000 --stripes 1 \
        --export-detail --json-trace /tmp/moat-viz-data.json

    # Then visualize:
    python moat-connectivity-map.py /tmp/moat-viz-data.json \
        -o k26-moat-visualization.png --dpi 300

    # With multi-stripe f(r) overlay:
    python moat-connectivity-map.py /tmp/moat-viz-data.json \
        -o k26-moat-visualization.png --fr-values 0.50,0.47,0.56,0.125,0.59,0.25
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import matplotlib.colors as mcolors
from matplotlib.collections import LineCollection
import numpy as np


# Face bitmask constants (ISE convention)
FACE_INNER = 1
FACE_OUTER = 2
FACE_LEFT  = 4
FACE_RIGHT = 8


def load_trace(path):
    with open(path) as f:
        return json.load(f)


def compute_component_face_masks(tile):
    """OR all per-prime face_assignments within each component."""
    comp_mask = defaultdict(int)
    for cid, fa in zip(tile["component_ids"], tile["face_assignments"]):
        comp_mask[cid] |= fa
    return dict(comp_mask)


def classify_prime(comp_mask_value):
    touches_inner = bool(comp_mask_value & FACE_INNER)
    touches_outer = bool(comp_mask_value & FACE_OUTER)
    if touches_inner and touches_outer:
        return "io"
    elif touches_inner:
        return "inner"
    elif touches_outer:
        return "outer"
    else:
        return "disconnected"


# ---------- color scheme (dark background) ----------
BG_COLOR = "#0a0e17"
PANEL_BG = "#0f1520"

COLORS = {
    "io":           "#22c55e",  # bright green
    "inner":        "#60a5fa",  # bright blue
    "outer":        "#f97316",  # orange (more visible on dark than red)
    "disconnected": "#334155",  # slate-700 (visible but recessive)
}

ALPHAS = {
    "io":           1.0,
    "inner":        0.85,
    "outer":        0.85,
    "disconnected": 0.4,
}

LABELS = {
    "io":           "IO-connected (path exists through shell)",
    "inner":        "Reaches inner face only (toward origin)",
    "outer":        "Reaches outer face only (away from origin)",
    "disconnected": "Isolated (no radial face contact)",
}

DRAW_ORDER = ["disconnected", "outer", "inner", "io"]


def build_figure(data, dpi=300, point_size=0.4, draw_edges=False, edge_alpha=0.03,
                 fr_values=None):
    """
    Build the full visualization figure.

    fr_values: optional list of f(r) from a multi-stripe run, one per shell,
               to annotate alongside the single-stripe detail data.
    """
    config = data["config"]
    shells = data["shells"]
    stripe = data["stripes"][0]
    tiles = stripe["tiles"]

    tiles_sorted = sorted(tiles, key=lambda t: t["a_lo"])
    shells_sorted = sorted(shells, key=lambda s: s["a_lo"])

    a_min = min(t["a_lo"] for t in tiles_sorted)
    a_max = max(t["a_lo"] + t["height"] for t in tiles_sorted)
    b_min = min(t["b_lo"] for t in tiles_sorted)
    b_max = max(t["b_lo"] + t["width"] for t in tiles_sorted)

    n_tiles = len(tiles_sorted)
    total_primes = 0
    total_edges = 0
    class_counts = defaultdict(int)

    # Figure: each panel is roughly square (1000x1000 data),
    # rendered at ~3.8 inches wide, giving comfortable point density.
    panel_w = 6.0
    panel_h = panel_w * (tiles_sorted[0]["height"] / tiles_sorted[0]["width"])
    fig_width = panel_w + 3.5   # room for right-side annotation
    fig_height = panel_h * n_tiles + 2.8  # title + legend

    fig = plt.figure(figsize=(fig_width, fig_height), dpi=dpi, facecolor=BG_COLOR)

    # Title
    fig.text(
        0.02, 0.995,
        f"Gaussian Prime Connectivity  |  k\u00b2 = {config['k_sq']}  |  "
        f"R \u2208 [{a_min:,}, {a_max:,}]  |  "
        f"strip width = {config['tile_width']}",
        fontsize=13, fontweight="bold", color="white",
        va="top", ha="left",
        fontfamily="monospace",
    )
    fig.text(
        0.02, 0.985,
        "Color = component face-reachability within a single strip. "
        "No green = no inner\u2194outer path = moat candidate.",
        fontsize=8, color="#94a3b8", va="top", ha="left",
    )

    # Create axes manually for tighter control
    left_margin = 0.09
    right_margin = 0.62  # fraction of fig_width used by plot
    bottom_start = 0.045
    top_end = 0.975
    available_height = top_end - bottom_start
    gap = 0.015
    ax_height = (available_height - gap * (n_tiles - 1)) / n_tiles

    axes = []
    for i in range(n_tiles):
        y0 = bottom_start + (n_tiles - 1 - i) * (ax_height + gap)
        ax = fig.add_axes([left_margin, y0, right_margin - left_margin, ax_height])
        axes.append(ax)

    for panel_idx, (tile, shell, ax) in enumerate(zip(tiles_sorted, shells_sorted, axes)):
        ax.set_facecolor(PANEL_BG)

        primes = np.array(tile["primes"])
        comp_ids = np.array(tile["component_ids"])
        edges = tile["edges"]

        n_primes = len(primes)
        total_primes += n_primes
        total_edges += len(edges)

        comp_mask = compute_component_face_masks(tile)
        classifications = np.array([classify_prime(comp_mask[cid]) for cid in comp_ids])

        for cls in DRAW_ORDER:
            class_counts[cls] += int(np.sum(classifications == cls))

        # Draw primes
        for cls in DRAW_ORDER:
            mask = classifications == cls
            if not np.any(mask):
                continue
            pts = primes[mask]
            ax.scatter(
                pts[:, 1], pts[:, 0],
                c=COLORS[cls],
                s=point_size,
                alpha=ALPHAS[cls],
                edgecolors="none",
                rasterized=True,
                zorder=2 if cls == "io" else 1,
            )

        # Optionally draw edges
        if draw_edges and len(edges) > 0:
            edge_arr = np.array(edges)
            if len(edge_arr) > 80000:
                rng = np.random.default_rng(42)
                idx = rng.choice(len(edge_arr), 80000, replace=False)
                edge_arr = edge_arr[idx]
            segments = []
            for e in edge_arr:
                p1, p2 = primes[e[0]], primes[e[1]]
                segments.append([(p1[1], p1[0]), (p2[1], p2[0])])
            lc = LineCollection(segments, colors="#475569", linewidths=0.15,
                                alpha=edge_alpha, zorder=0)
            ax.add_collection(lc)

        # Inner/outer face markers
        ax.axhline(y=tile["a_lo"], color="#fbbf24", linewidth=0.6,
                   linestyle="--", alpha=0.4, zorder=3)
        ax.axhline(y=tile["a_lo"] + tile["height"], color="#fbbf24",
                   linewidth=0.6, linestyle="--", alpha=0.4, zorder=3)

        ax.set_xlim(b_min - 10, b_max + 10)
        ax.set_ylim(tile["a_lo"] - 5, tile["a_lo"] + tile["height"] + 5)
        ax.set_aspect("equal")
        ax.tick_params(labelsize=6, colors="#64748b")
        ax.spines[:].set_color("#1e293b")
        ax.set_ylabel("a", fontsize=7, color="#94a3b8")
        if panel_idx == n_tiles - 1:
            ax.set_xlabel("b (lateral)", fontsize=7, color="#94a3b8")

        # ---------- right-side annotation ----------
        n_io = int(np.sum(classifications == "io"))
        n_inner = int(np.sum(classifications == "inner"))
        n_outer = int(np.sum(classifications == "outer"))
        n_disc = int(np.sum(classifications == "disconnected"))
        n_comp = len(comp_mask)

        # f(r) from multi-stripe run
        fr_str = ""
        fr_val = None
        if fr_values and panel_idx < len(fr_values):
            fr_val = fr_values[panel_idx]
            fr_str = f"f(r) = {fr_val:.3f}"
        else:
            fr_str = f"f(r) = {shell['f_r']:.3f}"

        # Build annotation text
        lines = [
            (f"Shell {panel_idx}", 11, "bold", "white"),
            (f"a \u2208 [{tile['a_lo']:,}, {tile['a_lo']+tile['height']:,}]", 8, "normal", "#cbd5e1"),
            (f"R \u2248 {shell['r_center']:,.0f}", 8, "normal", "#cbd5e1"),
            ("", 4, "normal", "#cbd5e1"),  # spacer
            (fr_str, 10, "bold",
             "#22c55e" if (fr_val and fr_val > 0.3) else
             "#f97316" if (fr_val and fr_val > 0) else "#ef4444"),
            ("", 4, "normal", "#cbd5e1"),
            (f"{n_primes:,} primes", 8, "normal", "#e2e8f0"),
            (f"{n_comp:,} components", 8, "normal", "#e2e8f0"),
            (f"{len(edges):,} edges", 8, "normal", "#94a3b8"),
            ("", 4, "normal", "#cbd5e1"),
            (f"\u25cf {n_io:,} IO", 8, "bold", COLORS["io"]),
            (f"\u25cf {n_inner:,} inner", 8, "normal", COLORS["inner"]),
            (f"\u25cf {n_outer:,} outer", 8, "normal", COLORS["outer"]),
            (f"\u25cf {n_disc:,} isolated", 8, "normal", COLORS["disconnected"]),
        ]

        # Place annotation to the right of the panel
        ann_x = 0.67
        ax_pos = ax.get_position()
        # Line spacing in figure coordinates -- spread lines evenly within panel
        line_step = ax_pos.height / (len(lines) + 1)
        line_idx = 0
        for text, fontsize, weight, color in lines:
            if text == "":
                line_idx += 1
                continue
            fig_y = ax_pos.y1 - (line_idx + 1) * line_step
            fig.text(ann_x, fig_y, text, fontsize=fontsize, fontweight=weight,
                    color=color, va="center", ha="left", fontfamily="monospace")
            line_idx += 1

    # ---------- legend at bottom ----------
    legend_patches = [
        mpatches.Patch(color=COLORS[cls], label=f"{LABELS[cls]} ({class_counts[cls]:,})")
        for cls in DRAW_ORDER
    ]
    fig.legend(
        handles=legend_patches,
        loc="lower center",
        ncol=2,
        fontsize=7,
        frameon=True,
        facecolor="#1e293b",
        edgecolor="#334155",
        labelcolor="white",
        bbox_to_anchor=(0.35, 0.001),
    )

    stats = {
        "total_primes": total_primes,
        "total_edges": total_edges,
        "class_counts": dict(class_counts),
        "n_tiles": n_tiles,
        "a_range": (a_min, a_max),
        "b_range": (b_min, b_max),
        "k_squared": config["k_sq"],
    }
    return fig, stats


def main():
    parser = argparse.ArgumentParser(
        description="Visualize Gaussian prime connectivity around a moat region"
    )
    parser.add_argument("json_trace", help="Path to ISE --export-detail JSON trace")
    parser.add_argument("-o", "--output", default="moat-connectivity.png",
                        help="Output PNG path")
    parser.add_argument("--dpi", type=int, default=300, help="Output DPI")
    parser.add_argument("--point-size", type=float, default=0.4,
                        help="Scatter point size")
    parser.add_argument("--draw-edges", action="store_true",
                        help="Draw edges between connected primes")
    parser.add_argument("--edge-alpha", type=float, default=0.03,
                        help="Edge line alpha")
    parser.add_argument("--fr-values", type=str, default=None,
                        help="Comma-separated f(r) values from multi-stripe run, one per shell")
    args = parser.parse_args()

    fr_values = None
    if args.fr_values:
        fr_values = [float(x) for x in args.fr_values.split(",")]

    print(f"Loading {args.json_trace} ...")
    data = load_trace(args.json_trace)
    print(f"  {len(data['shells'])} shells, {len(data['stripes'][0]['tiles'])} tiles")

    print("Building figure ...")
    fig, stats = build_figure(
        data,
        dpi=args.dpi,
        point_size=args.point_size,
        draw_edges=args.draw_edges,
        edge_alpha=args.edge_alpha,
        fr_values=fr_values,
    )

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    print(f"Saving to {out} at {args.dpi} DPI ...")
    fig.savefig(str(out), dpi=args.dpi, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)

    fsize = out.stat().st_size
    print(f"\nDone.")
    print(f"  File: {out}  ({fsize / 1024:.1f} KB)")
    print(f"  k\u00b2 = {stats['k_squared']}")
    print(f"  a range: [{stats['a_range'][0]:,}, {stats['a_range'][1]:,}]")
    print(f"  b range: [{stats['b_range'][0]:,}, {stats['b_range'][1]:,}]")
    print(f"  Total primes: {stats['total_primes']:,}")
    print(f"  Total edges: {stats['total_edges']:,}")
    for cls in DRAW_ORDER:
        print(f"    {cls}: {stats['class_counts'].get(cls, 0):,}")


if __name__ == "__main__":
    main()
