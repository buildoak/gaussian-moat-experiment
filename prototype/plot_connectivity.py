#!/usr/bin/env python3
"""Visualization suite for strip connectivity probe data.

Reads connectivity_data.json and generates four plots:
  1. channel_count_decay.png    — channels vs radial distance per k²
  2. moat_distance_vs_k.png     — moat distance vs k²
  3. origin_component_survival.png — horizontal bar of origin reach per k²
  4. channel_count_heatmap.png  — heatmap of channels over (R, k²)

Usage:
    python3 prototype/plot_connectivity.py
"""

from __future__ import annotations

import json
import math
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import numpy as np

_here = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = os.path.join(_here, "output")
os.makedirs(OUTPUT_DIR, exist_ok=True)

# ── Dark theme matching the explainer ──
DARK_BG = "#0d1117"
DARK_FACE = "#161b22"
DARK_TEXT = "#c9d1d9"
DARK_GRID = "#30363d"
ACCENT_COLORS = [
    "#58a6ff", "#3fb950", "#f0883e", "#bc8cff", "#f778ba",
    "#ff7b72", "#79c0ff", "#56d364", "#d2a8ff", "#ffa657",
    "#7ee787", "#a5d6ff", "#ffc680", "#d2a8ff", "#ff9bce",
    "#f85149", "#388bfd", "#2ea043", "#e3b341", "#8b949e",
]


def load_data():
    path = os.path.join(_here, "connectivity_data.json")
    with open(path) as f:
        return json.load(f)


def apply_dark_theme(fig, ax):
    fig.patch.set_facecolor(DARK_BG)
    ax.set_facecolor(DARK_FACE)
    ax.tick_params(colors=DARK_TEXT)
    ax.xaxis.label.set_color(DARK_TEXT)
    ax.yaxis.label.set_color(DARK_TEXT)
    ax.title.set_color(DARK_TEXT)
    for spine in ax.spines.values():
        spine.set_color(DARK_GRID)
    ax.grid(True, color=DARK_GRID, linewidth=0.4, alpha=0.6)


# ────────────────────────────────────────────────────────────────
# Plot 1: Channel count decay
# ────────────────────────────────────────────────────────────────

def plot_channel_count_decay(data):
    fig, ax = plt.subplots(figsize=(14, 8))
    apply_dark_theme(fig, ax)

    experiments = data["experiments"]
    for i, exp in enumerate(experiments):
        k_sq = exp["k_squared"]
        shells = exp["shells"]
        if not shells:
            continue

        # Use midpoint of each shell as x
        rs = [(s["r_inner"] + s["r_outer"]) / 2 for s in shells]
        channels = [s["channels"] for s in shells]
        color = ACCENT_COLORS[i % len(ACCENT_COLORS)]

        ax.plot(rs, channels, color=color, linewidth=1.0, alpha=0.8,
                label=f"k²={k_sq}")

        # Mark moat distance with vertical dashed line
        if exp["moat_found"] and exp["moat_distance"] is not None:
            md = exp["moat_distance"]
            ax.axvline(md, color=color, linewidth=0.8, linestyle="--", alpha=0.5)

    ax.set_xlabel("Radial distance R", fontsize=12)
    ax.set_ylabel("Crossing channels", fontsize=12)
    ax.set_title("Channel Count Decay — How Connectivity Thins Radially", fontsize=14)
    ax.legend(
        fontsize=7, ncol=3, loc="upper right",
        facecolor=DARK_FACE, edgecolor=DARK_GRID, labelcolor=DARK_TEXT,
    )
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)

    path = os.path.join(OUTPUT_DIR, "channel_count_decay.png")
    fig.savefig(path, dpi=150, bbox_inches="tight", facecolor=DARK_BG)
    plt.close(fig)
    print(f"  Saved: {path}")
    return path


# ────────────────────────────────────────────────────────────────
# Plot 2: Moat distance vs k²
# ────────────────────────────────────────────────────────────────

def plot_moat_distance_vs_k(data):
    fig, ax = plt.subplots(figsize=(10, 6))
    apply_dark_theme(fig, ax)

    experiments = data["experiments"]
    k_vals = []
    moat_dists = []
    no_moat_k = []

    for exp in experiments:
        k_sq = exp["k_squared"]
        if exp["moat_found"] and exp["moat_distance"] is not None:
            k_vals.append(k_sq)
            moat_dists.append(exp["moat_distance"])
        else:
            no_moat_k.append(k_sq)

    # Plot moat distances
    ax.scatter(k_vals, moat_dists, c="#58a6ff", s=60, zorder=5,
               edgecolors="#79c0ff", linewidth=0.8, label="Moat found")
    ax.plot(k_vals, moat_dists, color="#58a6ff", linewidth=1.0, alpha=0.5, zorder=4)

    # Mark no-moat cases
    if no_moat_k:
        for k in no_moat_k:
            r_max = next(e["r_max"] for e in experiments if e["k_squared"] == k)
            ax.scatter([k], [r_max], c="#f85149", s=80, marker="^", zorder=5,
                       edgecolors="#ff7b72", linewidth=0.8)
            ax.annotate(f">{r_max}", (k, r_max), textcoords="offset points",
                        xytext=(8, 5), fontsize=8, color="#f85149")

    ax.set_xlabel("k² (squared step threshold)", fontsize=12)
    ax.set_ylabel("Moat distance (R)", fontsize=12)
    ax.set_title("Moat Distance vs Step Size — Exponential Growth", fontsize=14)
    ax.set_yscale("log")
    ax.legend(
        fontsize=9, loc="upper left",
        facecolor=DARK_FACE, edgecolor=DARK_GRID, labelcolor=DARK_TEXT,
    )

    # Annotate the trend
    if len(k_vals) > 4:
        ax.text(0.98, 0.05, "Red triangles: moat not found within R_max",
                transform=ax.transAxes, fontsize=8, color="#f85149",
                ha="right", va="bottom")

    path = os.path.join(OUTPUT_DIR, "moat_distance_vs_k.png")
    fig.savefig(path, dpi=150, bbox_inches="tight", facecolor=DARK_BG)
    plt.close(fig)
    print(f"  Saved: {path}")
    return path


# ────────────────────────────────────────────────────────────────
# Plot 3: Origin component survival (horizontal bars)
# ────────────────────────────────────────────────────────────────

def plot_origin_survival(data):
    fig, ax = plt.subplots(figsize=(14, 8))
    apply_dark_theme(fig, ax)

    experiments = data["experiments"]

    # Compute survival distance for each k²
    survival = []
    for exp in experiments:
        k_sq = exp["k_squared"]
        # Find last shell where origin was alive
        last_alive_r = 0
        for s in exp["shells"]:
            if s["origin_alive"]:
                last_alive_r = s["r_outer"]
        survival.append((k_sq, last_alive_r, exp["moat_found"], exp["r_max"]))

    # Sort by survival distance
    survival.sort(key=lambda x: x[1])

    y_labels = [f"k²={s[0]}" for s in survival]
    y_pos = range(len(survival))
    widths = [s[1] for s in survival]
    colors = []

    for s in survival:
        k_sq, dist, moat_found, r_max = s
        if not moat_found:
            colors.append("#f85149")  # red = survived past R_max
        elif dist < 100:
            colors.append("#3fb950")  # green = died early
        elif dist < 1000:
            colors.append("#f0883e")  # orange = moderate
        else:
            colors.append("#bc8cff")  # purple = survived far

    bars = ax.barh(y_pos, widths, color=colors, height=0.7, alpha=0.85,
                   edgecolor=[c for c in colors], linewidth=0.5)

    # Add distance labels on bars
    for bar, s in zip(bars, survival):
        w = bar.get_width()
        label = f"R={w:.0f}" if s[2] else f">{s[3]} (no moat)"
        ax.text(w + max(widths) * 0.01, bar.get_y() + bar.get_height() / 2,
                label, va="center", fontsize=8, color=DARK_TEXT)

    ax.set_yticks(y_pos)
    ax.set_yticklabels(y_labels, fontsize=9)
    ax.set_xlabel("Radial reach of origin component", fontsize=12)
    ax.set_title("Origin Component Survival — How Far Does It Reach?", fontsize=14)
    ax.set_xlim(right=max(widths) * 1.15)

    # Legend
    from matplotlib.patches import Patch
    legend_elements = [
        Patch(facecolor="#3fb950", label="Moat < 100"),
        Patch(facecolor="#f0883e", label="Moat 100-1000"),
        Patch(facecolor="#bc8cff", label="Moat > 1000"),
        Patch(facecolor="#f85149", label="No moat in range"),
    ]
    ax.legend(
        handles=legend_elements, fontsize=8, loc="lower right",
        facecolor=DARK_FACE, edgecolor=DARK_GRID, labelcolor=DARK_TEXT,
    )

    path = os.path.join(OUTPUT_DIR, "origin_component_survival.png")
    fig.savefig(path, dpi=150, bbox_inches="tight", facecolor=DARK_BG)
    plt.close(fig)
    print(f"  Saved: {path}")
    return path


# ────────────────────────────────────────────────────────────────
# Plot 4: Channel count heatmap
# ────────────────────────────────────────────────────────────────

def plot_channel_heatmap(data):
    fig, ax = plt.subplots(figsize=(12, 8))
    apply_dark_theme(fig, ax)

    experiments = data["experiments"]

    # Determine grid: X = radial bins, Y = k² values
    k_values = [exp["k_squared"] for exp in experiments]

    # Normalize radial axis: use fractional distance (R / R_max) so all
    # experiments are comparable despite different R_max
    n_bins = 100
    frac_bins = np.linspace(0, 1, n_bins)

    heatmap = np.zeros((len(k_values), n_bins))

    for i, exp in enumerate(experiments):
        r_max = exp["r_max"]
        for s in exp["shells"]:
            r_mid = (s["r_inner"] + s["r_outer"]) / 2
            frac = r_mid / r_max
            bin_idx = min(int(frac * n_bins), n_bins - 1)
            heatmap[i, bin_idx] = max(heatmap[i, bin_idx], s["channels"])

    # Custom colormap: dark -> blue -> cyan -> yellow
    cmap = matplotlib.colormaps.get_cmap("inferno").copy()
    cmap.set_under(DARK_FACE)

    im = ax.imshow(
        heatmap, aspect="auto", origin="lower",
        extent=[0, 100, 0, len(k_values)],
        cmap=cmap, vmin=0.5, interpolation="nearest",
    )

    # Y-axis labels
    ax.set_yticks(np.arange(len(k_values)) + 0.5)
    ax.set_yticklabels([str(k) for k in k_values], fontsize=8)

    # X-axis: percent of R_max
    ax.set_xlabel("Radial distance (% of R_max)", fontsize=12)
    ax.set_ylabel("k²", fontsize=12)
    ax.set_title("Channel Count Heatmap — Connectivity Landscape", fontsize=14)

    # Overlay moat distance markers
    for i, exp in enumerate(experiments):
        if exp["moat_found"] and exp["moat_distance"] is not None:
            frac = exp["moat_distance"] / exp["r_max"] * 100
            ax.plot(frac, i + 0.5, "w|", markersize=10, markeredgewidth=2)

    cbar = fig.colorbar(im, ax=ax, label="Channel count", pad=0.02)
    cbar.ax.yaxis.label.set_color(DARK_TEXT)
    cbar.ax.tick_params(colors=DARK_TEXT)

    path = os.path.join(OUTPUT_DIR, "channel_count_heatmap.png")
    fig.savefig(path, dpi=150, bbox_inches="tight", facecolor=DARK_BG)
    plt.close(fig)
    print(f"  Saved: {path}")
    return path


# ────────────────────────────────────────────────────────────────
# Main
# ────────────────────────────────────────────────────────────────

def main():
    print("Generating connectivity plots...")
    data = load_data()

    plot_channel_count_decay(data)
    plot_moat_distance_vs_k(data)
    plot_origin_survival(data)
    plot_channel_heatmap(data)

    print(f"\nAll plots saved to: {OUTPUT_DIR}/")


if __name__ == "__main__":
    main()
