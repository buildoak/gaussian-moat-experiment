#!/usr/bin/env python3
"""
Tile-based Gaussian moat probing: connectivity visualization.

Left panel:  Microscope view — actual Gaussian primes inside one tile,
             edges for step distance √k², face-port primes colored by boundary.
Right panel: Telescope view — 4×6 grid of tiles as coarse-graph nodes,
             edges between tiles sharing face-port connections, moat highlighted.

Output: 2026-03-20-tile-connectivity.png (300 DPI, 16×8 in, dark theme)
"""

import math
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch
from matplotlib.collections import LineCollection
from sympy import isprime

# ── Gaussian prime helpers ──────────────────────────────────────────

def is_gaussian_prime(a, b):
    if a == 0:
        return abs(b) > 1 and isprime(abs(b)) and abs(b) % 4 == 3
    if b == 0:
        return abs(a) > 1 and isprime(abs(a)) and abs(a) % 4 == 3
    norm = a * a + b * b
    return isprime(norm)


# ── Parameters ──────────────────────────────────────────────────────

K_SQ = 26
STEP = math.sqrt(K_SQ)          # ≈ 5.099
COLLAR = math.ceil(STEP)        # 6

# Tile bounds (real part = a, imag part = b)
A_MIN, A_MAX = 50, 60
B_MIN, B_MAX = 10, 20

BG = "#1a1a2e"
GRID_COLOR = "#2a2a4a"
TEXT_COLOR = "#e0e0e0"

# Face-port colors
C_INNER  = "#4da6ff"   # blue  — near a_min (inner radial face)
C_OUTER  = "#ff4d4d"   # red   — near a_max (outer radial face)
C_LEFT   = "#4dff88"   # green — near b_min
C_RIGHT  = "#ffaa33"   # orange— near b_max
C_INTERIOR = "#888888"  # gray

# Tile-graph colors
C_TRAVERSABLE = "#2ecc71"
C_WALL        = "#e74c3c"
C_PATH_EDGE   = "#f1c40f"
C_PATH_NODE   = "#f39c12"
C_EDGE_NORMAL = "#445566"


# ── Left Panel helpers ──────────────────────────────────────────────

def collect_primes(a_lo, a_hi, b_lo, b_hi):
    """Return list of (a, b) Gaussian primes in [a_lo, a_hi] × [b_lo, b_hi]."""
    primes = []
    for a in range(a_lo, a_hi + 1):
        for b in range(b_lo, b_hi + 1):
            if is_gaussian_prime(a, b):
                primes.append((a, b))
    return primes


def classify_prime(a, b, collar=COLLAR):
    """Return face tag(s) and single display color."""
    faces = set()
    if a - A_MIN < collar:
        faces.add("inner")
    if A_MAX - a < collar:
        faces.add("outer")
    if b - B_MIN < collar:
        faces.add("left")
    if B_MAX - b < collar:
        faces.add("right")

    # Priority: outer > inner > right > left  (arbitrary but consistent)
    if not faces:
        return "interior", C_INTERIOR
    if "outer" in faces:
        return "outer", C_OUTER
    if "inner" in faces:
        return "inner", C_INNER
    if "right" in faces:
        return "right", C_RIGHT
    return "left", C_LEFT


def build_edges(primes, k_sq):
    """Return list of ((a1,b1),(a2,b2)) pairs within step distance."""
    edges = []
    n = len(primes)
    for i in range(n):
        for j in range(i + 1, n):
            da = primes[i][0] - primes[j][0]
            db = primes[i][1] - primes[j][1]
            if da * da + db * db <= k_sq:
                edges.append((primes[i], primes[j]))
    return edges


# ── Right Panel: synthetic tile grid ────────────────────────────────

N_SHELLS = 4
N_STRIPS = 6

# Handcrafted crossing counts: row 2 (index 1 from top) is the "moat"
# Values: number of I→O crossing components
CROSSING_GRID = np.array([
    [3, 5, 2, 4, 3, 6],   # Shell 4 (outermost)
    [0, 0, 0, 0, 0, 0],   # Shell 3 — THE MOAT (all walls)
    [4, 2, 5, 1, 3, 2],   # Shell 2
    [2, 3, 1, 4, 2, 5],   # Shell 1 (innermost, near origin)
])

# Adjacency: lateral neighbors always connected unless one is a wall;
# radial neighbors connected if both have crossings.
def tile_connected(r1, c1, r2, c2, grid):
    """Are two adjacent tiles connected through face ports?"""
    v1 = grid[r1, c1]
    v2 = grid[r2, c2]
    if v1 == 0 or v2 == 0:
        return False
    # For the visualization, non-wall neighbors are connected
    return True


# ── Figure ──────────────────────────────────────────────────────────

fig, (ax_micro, ax_tele) = plt.subplots(
    1, 2, figsize=(16, 8),
    facecolor=BG,
    gridspec_kw={"width_ratios": [1, 1.1], "wspace": 0.12},
)

# =====================================================================
# LEFT PANEL — Microscope View
# =====================================================================

ax = ax_micro
ax.set_facecolor(BG)
ax.set_title(
    "Microscope View — Inside a Tile",
    color=TEXT_COLOR, fontsize=14, fontweight="bold", pad=12,
)

# Collect primes inside tile AND inside collar (for edges that cross boundary)
expanded_primes = collect_primes(
    A_MIN - COLLAR, A_MAX + COLLAR,
    B_MIN - COLLAR, B_MAX + COLLAR,
)
tile_primes = [(a, b) for a, b in expanded_primes
               if A_MIN <= a <= A_MAX and B_MIN <= b <= B_MAX]

# Edges among tile primes only
edges = build_edges(tile_primes, K_SQ)

# Draw collar zone (dotted)
collar_rect = mpatches.FancyBboxPatch(
    (A_MIN - COLLAR - 0.5, B_MIN - COLLAR - 0.5),
    (A_MAX - A_MIN) + 2 * COLLAR + 1,
    (B_MAX - B_MIN) + 2 * COLLAR + 1,
    boxstyle="round,pad=0.3",
    linewidth=1.2, edgecolor="#555577", facecolor="none", linestyle=":",
)
ax.add_patch(collar_rect)

# Draw tile boundary (dashed)
tile_rect = mpatches.FancyBboxPatch(
    (A_MIN - 0.5, B_MIN - 0.5),
    A_MAX - A_MIN + 1,
    B_MAX - B_MIN + 1,
    boxstyle="round,pad=0.2",
    linewidth=1.8, edgecolor="#aaaacc", facecolor="none", linestyle="--",
)
ax.add_patch(tile_rect)

# Draw face-port shading bands (subtle background rectangles)
band_alpha = 0.07
# Inner band (near a_min)
ax.axvspan(A_MIN - 0.5, A_MIN + COLLAR - 0.5, color=C_INNER, alpha=band_alpha)
# Outer band (near a_max)
ax.axvspan(A_MAX - COLLAR + 0.5, A_MAX + 0.5, color=C_OUTER, alpha=band_alpha)
# Left band (near b_min)
ax.axhspan(B_MIN - 0.5, B_MIN + COLLAR - 0.5, color=C_LEFT, alpha=band_alpha)
# Right band (near b_max)
ax.axhspan(B_MAX - COLLAR + 0.5, B_MAX + 0.5, color=C_RIGHT, alpha=band_alpha)

# Draw edges
edge_segments = [[(e[0][0], e[0][1]), (e[1][0], e[1][1])] for e in edges]
lc = LineCollection(edge_segments, colors="#556688", linewidths=0.8, alpha=0.5, zorder=1)
ax.add_collection(lc)

# Plot primes
for a, b in tile_primes:
    tag, color = classify_prime(a, b)
    marker = "o"
    size = 50 if tag != "interior" else 30
    edge_c = "white" if tag != "interior" else "#555555"
    ax.scatter(a, b, c=color, s=size, marker=marker, edgecolors=edge_c,
               linewidths=0.6, zorder=3)

# Axis labels & limits
pad = COLLAR + 2
ax.set_xlim(A_MIN - pad, A_MAX + pad)
ax.set_ylim(B_MIN - pad, B_MAX + pad)
ax.set_xlabel("Re (a)", color=TEXT_COLOR, fontsize=10)
ax.set_ylabel("Im (b)", color=TEXT_COLOR, fontsize=10)
ax.tick_params(colors=TEXT_COLOR, labelsize=8)
for spine in ax.spines.values():
    spine.set_color(GRID_COLOR)

# Legend
legend_handles = [
    mpatches.Patch(color=C_INNER, label="Inner face (a_min)"),
    mpatches.Patch(color=C_OUTER, label="Outer face (a_max)"),
    mpatches.Patch(color=C_LEFT, label="Left face (b_min)"),
    mpatches.Patch(color=C_RIGHT, label="Right face (b_max)"),
    mpatches.Patch(color=C_INTERIOR, label="Interior prime"),
]
leg = ax.legend(
    handles=legend_handles, loc="upper left", fontsize=7,
    framealpha=0.3, facecolor=BG, edgecolor=GRID_COLOR,
    labelcolor=TEXT_COLOR,
)

# Subtitle
ax.text(
    0.5, -0.09,
    "Face ports are the tile's 'pins' — the only data needed for composition",
    transform=ax.transAxes, ha="center", va="top",
    fontsize=9, fontstyle="italic", color="#aaaacc",
)

# Collar label
ax.annotate(
    f"collar = ⌈√{K_SQ}⌉ = {COLLAR}",
    xy=(A_MAX + COLLAR + 0.5, B_MAX + COLLAR + 0.5),
    xytext=(A_MAX + COLLAR + 2, B_MAX + COLLAR + 2),
    fontsize=7, color="#777799",
    arrowprops=dict(arrowstyle="->", color="#777799", lw=0.8),
)

# Step distance annotation
ax.text(
    0.98, 0.98,
    f"k² = {K_SQ},  step = √{K_SQ} ≈ {STEP:.2f}",
    transform=ax.transAxes, ha="right", va="top",
    fontsize=8, color="#aaaacc",
    bbox=dict(boxstyle="round,pad=0.3", facecolor=BG, edgecolor="#555577"),
)


# =====================================================================
# RIGHT PANEL — Telescope View
# =====================================================================

ax = ax_tele
ax.set_facecolor(BG)
ax.set_title(
    "Telescope View — Tiles as Nodes",
    color=TEXT_COLOR, fontsize=14, fontweight="bold", pad=12,
)

# Layout constants
TILE_W = 1.0
TILE_H = 0.8
GAP_X = 0.3
GAP_Y = 0.25
MARGIN = 0.8

# Compute tile centers
def tile_center(row, col):
    x = MARGIN + col * (TILE_W + GAP_X) + TILE_W / 2
    y = MARGIN + (N_SHELLS - 1 - row) * (TILE_H + GAP_Y) + TILE_H / 2
    return x, y

# Draw inter-tile edges first (behind everything)
edge_lines = []
edge_colors_list = []
for r in range(N_SHELLS):
    for c in range(N_STRIPS):
        # Right neighbor
        if c + 1 < N_STRIPS:
            if tile_connected(r, c, r, c + 1, CROSSING_GRID):
                x1, y1 = tile_center(r, c)
                x2, y2 = tile_center(r, c + 1)
                edge_lines.append([(x1, y1), (x2, y2)])
                edge_colors_list.append(C_EDGE_NORMAL)
        # Down neighbor (next shell outward = row - 1 in our layout)
        if r + 1 < N_SHELLS:
            if tile_connected(r, c, r + 1, c, CROSSING_GRID):
                x1, y1 = tile_center(r, c)
                x2, y2 = tile_center(r + 1, c)
                edge_lines.append([(x1, y1), (x2, y2)])
                edge_colors_list.append(C_EDGE_NORMAL)

lc2 = LineCollection(edge_lines, colors=edge_colors_list, linewidths=1.5, alpha=0.5, zorder=1)
ax.add_collection(lc2)

# Highlight a path from bottom-left toward top-right that gets BLOCKED by moat
# Path goes: (3,0) → (3,1) → (2,1) → (2,2) → (2,3) → hits moat at row 1
# Show it trying to go up and failing
path_cells = [(3, 0), (3, 1), (2, 1), (2, 2), (2, 3), (2, 4), (2, 5)]
for i in range(len(path_cells) - 1):
    r1, c1 = path_cells[i]
    r2, c2 = path_cells[i + 1]
    x1, y1 = tile_center(r1, c1)
    x2, y2 = tile_center(r2, c2)
    ax.annotate(
        "", xy=(x2, y2), xytext=(x1, y1),
        arrowprops=dict(
            arrowstyle="->,head_width=0.15,head_length=0.1",
            color=C_PATH_EDGE, lw=2.5, connectionstyle="arc3,rad=0.05",
        ),
        zorder=5,
    )

# Draw blocked arrow from path trying to cross into moat row
r_from, c_from = (2, 5)
r_to, c_to = (1, 5)
x1, y1 = tile_center(r_from, c_from)
x2, y2 = tile_center(r_to, c_to)
mid_x = (x1 + x2) / 2
mid_y = (y1 + y2) / 2
ax.annotate(
    "", xy=(mid_x, mid_y), xytext=(x1, y1),
    arrowprops=dict(
        arrowstyle="->,head_width=0.15,head_length=0.1",
        color=C_PATH_EDGE, lw=2.5, linestyle="--",
    ),
    zorder=5,
)
# Big red X at the blockage
ax.text(mid_x, mid_y, "✕", color=C_WALL, fontsize=18, fontweight="bold",
        ha="center", va="center", zorder=6)

# Draw tile boxes
for r in range(N_SHELLS):
    for c in range(N_STRIPS):
        cx, cy = tile_center(r, c)
        crossings = CROSSING_GRID[r, c]

        if crossings > 0:
            # Green, brightness proportional to count
            intensity = 0.3 + 0.7 * min(crossings / 6, 1.0)
            face_color = matplotlib.colors.to_rgba(C_TRAVERSABLE, alpha=intensity * 0.6)
            edge_color = C_TRAVERSABLE
        else:
            face_color = matplotlib.colors.to_rgba(C_WALL, alpha=0.5)
            edge_color = C_WALL

        # Highlight path cells
        lw = 1.2
        if (r, c) in path_cells:
            edge_color = C_PATH_EDGE
            lw = 2.5

        box = FancyBboxPatch(
            (cx - TILE_W / 2, cy - TILE_H / 2),
            TILE_W, TILE_H,
            boxstyle="round,pad=0.06",
            facecolor=face_color,
            edgecolor=edge_color,
            linewidth=lw,
            zorder=3,
        )
        ax.add_patch(box)

        # Crossing count label
        label = str(crossings) if crossings > 0 else "0"
        ax.text(
            cx, cy, label,
            ha="center", va="center",
            fontsize=11, fontweight="bold",
            color="white" if crossings > 0 else "#ffaaaa",
            zorder=4,
        )

# Shell labels (left side)
shell_labels = ["Shell 1\n(inner)", "Shell 2", "Shell 3", "Shell 4\n(outer)"]
for r in range(N_SHELLS):
    _, cy = tile_center(r, 0)
    ax.text(
        MARGIN - 0.2, cy,
        shell_labels[N_SHELLS - 1 - r],
        ha="right", va="center",
        fontsize=8, color="#aaaacc",
    )

# Strip labels (top)
for c in range(N_STRIPS):
    cx, _ = tile_center(0, c)
    top_y = MARGIN + (N_SHELLS - 1) * (TILE_H + GAP_Y) + TILE_H + 0.15
    ax.text(
        cx, top_y,
        f"Strip {c + 1}",
        ha="center", va="bottom",
        fontsize=8, color="#aaaacc",
    )

# Moat annotation
moat_row = 1  # row index of the moat
_, moat_y = tile_center(moat_row, 0)
right_x = tile_center(moat_row, N_STRIPS - 1)[0] + TILE_W / 2 + 0.15
ax.annotate(
    "← MOAT\n   (no I→O\n    crossings)",
    xy=(right_x, moat_y),
    fontsize=9, fontweight="bold",
    color=C_WALL, ha="left", va="center",
)

# Axis off, set limits
x_max = MARGIN + N_STRIPS * (TILE_W + GAP_X) + 1.2
y_max = MARGIN + N_SHELLS * (TILE_H + GAP_Y) + 0.5
ax.set_xlim(-0.3, x_max)
ax.set_ylim(-0.1, y_max)
ax.set_aspect("equal")
ax.axis("off")

# Subtitle
ax.text(
    0.5, -0.05,
    "Each tile = one node.  Face ports = edges.  Find the moat in the coarse graph.",
    transform=ax.transAxes, ha="center", va="top",
    fontsize=9, fontstyle="italic", color="#aaaacc",
)

# Legend for right panel
legend_r = [
    mpatches.Patch(facecolor=C_TRAVERSABLE, edgecolor=C_TRAVERSABLE,
                   alpha=0.6, label="Traversable (I→O > 0)"),
    mpatches.Patch(facecolor=C_WALL, edgecolor=C_WALL,
                   alpha=0.5, label="Wall (I→O = 0)"),
    mpatches.Patch(facecolor="none", edgecolor=C_PATH_EDGE,
                   linewidth=2, label="Attempted path"),
]
leg2 = ax.legend(
    handles=legend_r, loc="lower left", fontsize=7,
    framealpha=0.3, facecolor=BG, edgecolor=GRID_COLOR,
    labelcolor=TEXT_COLOR,
)

# ── Save ────────────────────────────────────────────────────────────

out = "/Users/otonashi/thinking/building/gaussian-moat-cuda/research/2026-03-20-tile-connectivity.png"
fig.savefig(out, dpi=300, bbox_inches="tight", facecolor=BG, pad_inches=0.3)
plt.close(fig)
print(f"Saved → {out}")
print(f"  Tile primes found: {len(tile_primes)}")
print(f"  Edges drawn: {len(edges)}")
