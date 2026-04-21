#!/usr/bin/env python3
"""
Single continuous map of Gaussian prime connectivity at the k²=26 moat.

ONE figure. Primes colored by per-tile IO face connectivity (inner/outer/both/neither),
with blocked radial bands shaded in red. The tile-boundary edges are softened by
rendering primes as small semi-transparent dots that blend visually.

The moat appears as a clear transition zone: below it, through-connected (green)
primes dominate; in the moat, only inner-only (teal, from below) or outer-only
(amber, from above) survive; above it, through-connectivity resumes sporadically.
"""

import json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patheffects as pe

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
JSON_PATH = "/tmp/moat-viz-continuous.json"
OUT_PATH = "/Users/otonashi/thinking/building/gaussian-moat-cuda/research/results/k26-moat-continuous.png"

MOAT_CENTER = 1015639

BG_COLOR = "#0a0a18"
DPI = 300
FIG_W = 9
FIG_H = 24

# Colors — vivid, high-saturation
C_THROUGH  = "#00ff88"   # bright green — IO through-connected
C_INNER    = "#00ccff"   # bright cyan — inner face only
C_OUTER    = "#ff9900"   # bright amber — outer face only
C_ISOLATED = "#333355"   # muted blue-gray — no face connectivity

# ---------------------------------------------------------------------------
# Load and classify primes
# ---------------------------------------------------------------------------
print("Loading JSON trace...")
with open(JSON_PATH) as f:
    data = json.load(f)

stripe = data["stripes"][0]
tiles = stripe["tiles"]

# Sort tiles by a_lo for contiguous processing
tiles_sorted = sorted(tiles, key=lambda t: t["a_lo"])

# Classify each prime by its tile's component face connectivity
all_a = []
all_b = []
all_cat = []   # 0=isolated, 1=inner, 2=outer, 3=through

for tile in tiles_sorted:
    primes = tile["primes"]
    comp_ids = tile["component_ids"]
    i_comps = set(tile["i_face_components"])
    o_comps = set(tile["o_face_components"])

    for (a, b), cid in zip(primes, comp_ids):
        ti = cid in i_comps
        to = cid in o_comps
        if ti and to:
            cat = 3
        elif ti:
            cat = 1
        elif to:
            cat = 2
        else:
            cat = 0
        all_a.append(a)
        all_b.append(b)
        all_cat.append(cat)

all_a = np.array(all_a, dtype=np.float64)
all_b = np.array(all_b, dtype=np.float64)
all_cat = np.array(all_cat, dtype=np.int8)
N = len(all_a)

print(f"Total primes: {N:,}")
for cat, label in [(0, "isolated"), (1, "inner-only"), (2, "outer-only"), (3, "through")]:
    n = np.sum(all_cat == cat)
    print(f"  {label}: {n:,} ({100*n/N:.1f}%)")

# Tile IO data for band shading
tile_io = []
for tile in tiles_sorted:
    tile_io.append({
        "a_lo": tile["a_lo"],
        "a_hi": tile["a_lo"] + tile["height"],
        "io": tile["io_count"],
    })

# Contiguous blocked regions
blocked = []
start = None
for td in tile_io:
    if td["io"] == 0:
        if start is None:
            start = td["a_lo"]
        end = td["a_hi"]
    else:
        if start is not None:
            blocked.append((start, end))
            start = None
if start is not None:
    blocked.append((start, end))

print(f"\nBlocked regions:")
for lo, hi in blocked:
    print(f"  a=[{lo:,}, {hi:,}] ({hi-lo:,} units)")

# ---------------------------------------------------------------------------
# Plot
# ---------------------------------------------------------------------------
print("\nRendering figure...")

fig, ax = plt.subplots(figsize=(FIG_W, FIG_H), facecolor=BG_COLOR)
ax.set_facecolor(BG_COLOR)

# --- Blocked region background shading ---
for lo, hi in blocked:
    ax.axhspan(lo, hi, color="#ff0000", alpha=0.045, zorder=0)
    ax.axhline(y=lo, color="#ff4444", linewidth=0.25, alpha=0.2, zorder=1)
    ax.axhline(y=hi, color="#ff4444", linewidth=0.25, alpha=0.2, zorder=1)

# --- Draw primes in layers (back to front) ---

# Layer 1: Isolated (dimmest, background)
mask0 = all_cat == 0
ax.scatter(all_b[mask0], all_a[mask0],
           c=C_ISOLATED, s=0.15, alpha=0.06,
           linewidths=0, marker=".", rasterized=True, zorder=2)

# Layer 2: Inner-only (cyan, medium brightness)
mask1 = all_cat == 1
ax.scatter(all_b[mask1], all_a[mask1],
           c=C_INNER, s=0.5, alpha=0.55,
           linewidths=0, marker=".", rasterized=True, zorder=3)

# Layer 3: Outer-only (amber, medium brightness)
mask2 = all_cat == 2
ax.scatter(all_b[mask2], all_a[mask2],
           c=C_OUTER, s=0.5, alpha=0.55,
           linewidths=0, marker=".", rasterized=True, zorder=3)

# Layer 4: Through-connected (green, brightest, on top)
mask3 = all_cat == 3
ax.scatter(all_b[mask3], all_a[mask3],
           c=C_THROUGH, s=0.7, alpha=0.7,
           linewidths=0, marker=".", rasterized=True, zorder=4)

# --- Annotations ---

# Moat center
ax.axhline(y=MOAT_CENTER, color="#ff5555", linestyle="--",
           linewidth=0.6, alpha=0.35, zorder=5)

# Blocked region labels (right margin)
b_max = all_b.max()
for lo, hi in blocked:
    mid = (lo + hi) / 2
    w = hi - lo
    if w >= 1500:
        label = f"BLOCKED  {w:,}"
        fs, a = 7.5, 0.75
    elif w >= 1000:
        label = f"BLOCKED  {w:,}"
        fs, a = 6.5, 0.6
    else:
        label = f"blocked"
        fs, a = 5.5, 0.45
    ax.annotate(label, xy=(b_max - 10, mid),
                fontsize=fs, fontweight="bold", color="#ff5555", alpha=a,
                ha="right", va="center",
                path_effects=[pe.withStroke(linewidth=2.5, foreground=BG_COLOR)],
                zorder=6)

# IO-connected tile labels (right margin, smaller)
for td in tile_io:
    if td["io"] > 0:
        mid = (td["a_lo"] + td["a_hi"]) / 2
        ax.annotate(f"IO={td['io']}", xy=(b_max - 10, mid),
                    fontsize=4.5, color="#00ff88", alpha=0.4,
                    ha="right", va="center",
                    path_effects=[pe.withStroke(linewidth=1.5, foreground=BG_COLOR)],
                    zorder=6)

# --- Legend (custom, compact) ---
from matplotlib.lines import Line2D
legend_elements = [
    Line2D([0], [0], marker="o", color="none", markerfacecolor=C_THROUGH,
           markersize=6, label="Through-connected (IO)"),
    Line2D([0], [0], marker="o", color="none", markerfacecolor=C_INNER,
           markersize=6, label="Inner face only"),
    Line2D([0], [0], marker="o", color="none", markerfacecolor=C_OUTER,
           markersize=6, label="Outer face only"),
    Line2D([0], [0], marker="o", color="none", markerfacecolor=C_ISOLATED,
           markersize=6, label="Isolated"),
]
ax.legend(handles=legend_elements, loc="upper left", fontsize=7,
          framealpha=0.6, facecolor="#15152a", edgecolor="#333355",
          labelcolor="#cccccc")

# --- Axes ---
ax.set_xlim(all_b.min() - 15, all_b.max() + 15)
ax.set_ylim(all_a.min() - 300, all_a.max() + 300)

ax.set_xlabel("Lateral position (b)", fontsize=10, color="#cccccc", labelpad=8)
ax.set_ylabel("Distance from origin (a)", fontsize=10, color="#cccccc", labelpad=8)
ax.set_title(
    r"k² = 26 — Gaussian Prime Connectivity at the Moat",
    fontsize=14, fontweight="bold", color="#ffffff", pad=14
)

ax.tick_params(colors="#888888", labelsize=8)
for spine in ax.spines.values():
    spine.set_color("#333355")
ax.grid(False)

# Info footer
info = f"{N:,} primes  |  tile height: 500  |  step bound: sqrt(26)"
ax.text(0.5, -0.015, info, transform=ax.transAxes,
        fontsize=6, color="#555577", ha="center", va="top")

# ---------------------------------------------------------------------------
# Save
# ---------------------------------------------------------------------------
print(f"Saving to {OUT_PATH} ...")
fig.savefig(OUT_PATH, dpi=DPI, bbox_inches="tight", facecolor=BG_COLOR)
plt.close(fig)

print(f"Done. Output: {OUT_PATH}")
