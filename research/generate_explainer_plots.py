#!/usr/bin/env python3
"""
Generate all plots for the Gaussian Moat problem visual explainer.
Output: research/plots/01_gaussian_primes.png through 10_rosetta_stone.png
Style: dark theme, crisp 150 DPI PNGs.
"""

import os
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as patches
import matplotlib.colors as mcolors
from matplotlib.patches import FancyArrowPatch, Arc
from matplotlib.collections import LineCollection
from sympy import isprime

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "plots")
os.makedirs(OUT, exist_ok=True)

# ── Style constants ──────────────────────────────────────────────────────
BG      = "#0a0a0f"
TEXT    = "#d8d8e0"
GRID    = "#1a1a2e"
BLUE    = "#4a9eff"
ORANGE  = "#ff6b35"
GREEN   = "#2ecc71"
PURPLE  = "#a66bff"
RED     = "#ff4757"
DPI     = 150

def dark_style(ax, title=None):
    """Apply dark theme to an axes."""
    ax.set_facecolor(BG)
    ax.tick_params(colors=TEXT, which="both")
    for spine in ax.spines.values():
        spine.set_color("#2a2a3e")
    ax.xaxis.label.set_color(TEXT)
    ax.yaxis.label.set_color(TEXT)
    if title:
        ax.set_title(title, color=TEXT, fontsize=14, fontweight="bold", pad=12)

def dark_fig(figsize, **kw):
    fig = plt.figure(figsize=figsize, facecolor=BG, **kw)
    return fig

def savefig(fig, name):
    fig.savefig(os.path.join(OUT, name), dpi=DPI, facecolor=BG,
                bbox_inches="tight", pad_inches=0.3)
    plt.close(fig)
    print(f"  ✓ {name}")


# ══════════════════════════════════════════════════════════════════════════
# Plot 1: Gaussian Primes  (1200×1000)
# ══════════════════════════════════════════════════════════════════════════
def plot_01():
    print("Plot 01: Gaussian primes...")
    R = 50
    primes = []
    for a in range(-R, R + 1):
        for b in range(-R, R + 1):
            if a * a + b * b > R * R:
                continue
            aa, bb = abs(a), abs(b)
            if aa == 0 and bb == 0:
                continue
            if aa == 0:
                if bb >= 2 and isprime(bb) and bb % 4 == 3:
                    primes.append((a, b))
            elif bb == 0:
                if aa >= 2 and isprime(aa) and aa % 4 == 3:
                    primes.append((a, b))
            else:
                n = a * a + b * b
                if isprime(n):
                    primes.append((a, b))
    primes = np.array(primes)
    dist = np.sqrt(primes[:, 0]**2 + primes[:, 1]**2)

    # Center prime and neighbors within sqrt(k) step
    cx, cy = 3, 1
    k = 40
    step = np.sqrt(k)
    neighbors = []
    for a, b in primes:
        d2 = (a - cx)**2 + (b - cy)**2
        if 0 < d2 <= k:
            neighbors.append((a, b))
    neighbors = np.array(neighbors)

    fig = dark_fig((12, 10))
    ax = fig.add_subplot(111)
    dark_style(ax, "Gaussian Primes — Can You Walk to Infinity?")

    # Color map: distance-based blue→orange
    cmap = mcolors.LinearSegmentedColormap.from_list("bo", [BLUE, ORANGE])
    norm = plt.Normalize(0, R)
    sc = ax.scatter(primes[:, 0], primes[:, 1], c=dist, cmap=cmap, norm=norm,
                    s=6, alpha=0.85, zorder=2, edgecolors="none")

    # Circle of radius sqrt(k) around center prime
    circle = plt.Circle((cx, cy), step, fill=False, edgecolor=GREEN,
                         linewidth=1.5, linestyle="--", alpha=0.7, zorder=3)
    ax.add_patch(circle)

    # Lines to neighbors
    for nx, ny in neighbors:
        ax.plot([cx, nx], [cy, ny], color=GREEN, linewidth=0.8,
                alpha=0.6, zorder=3)

    # Highlight center and neighbors
    ax.scatter([cx], [cy], s=120, color=GREEN, zorder=5, edgecolors="white",
               linewidths=1.2)
    ax.scatter(neighbors[:, 0], neighbors[:, 1], s=60, color=GREEN, zorder=4,
               edgecolors="white", linewidths=0.8, alpha=0.9)

    ax.set_xlabel("Re", fontsize=12)
    ax.set_ylabel("Im", fontsize=12)
    ax.set_aspect("equal")
    ax.set_xlim(-R - 2, R + 2)
    ax.set_ylim(-R - 2, R + 2)
    ax.grid(True, color=GRID, linewidth=0.5, alpha=0.5)

    # Annotation
    ax.annotate(f"3+i and its neighbors\nwithin √{k} ≈ {step:.1f}",
                xy=(cx, cy), xytext=(cx + 12, cy + 10),
                color=GREEN, fontsize=10,
                arrowprops=dict(arrowstyle="->", color=GREEN, lw=1.2))

    cbar = fig.colorbar(sc, ax=ax, shrink=0.7, pad=0.02)
    cbar.set_label("Distance from origin", color=TEXT, fontsize=10)
    cbar.ax.tick_params(colors=TEXT)

    savefig(fig, "01_gaussian_primes.png")


# ══════════════════════════════════════════════════════════════════════════
# Plot 2: Annulus to Strip  (1400×600)
# ══════════════════════════════════════════════════════════════════════════
def plot_02():
    print("Plot 02: Annulus to strip...")
    R_center = 30
    thickness = 6
    r_lo, r_hi = R_center - thickness / 2, R_center + thickness / 2

    # Collect primes in the annulus
    pts = []
    limit = int(r_hi) + 2
    for a in range(-limit, limit + 1):
        for b in range(-limit, limit + 1):
            n2 = a * a + b * b
            r = np.sqrt(n2)
            if r < r_lo or r > r_hi:
                continue
            aa, bb = abs(a), abs(b)
            if aa == 0 and bb == 0:
                continue
            if aa == 0:
                if bb >= 2 and isprime(bb) and bb % 4 == 3:
                    pts.append((a, b))
            elif bb == 0:
                if aa >= 2 and isprime(aa) and aa % 4 == 3:
                    pts.append((a, b))
            else:
                if isprime(n2):
                    pts.append((a, b))
    pts = np.array(pts, dtype=float)
    angles = np.arctan2(pts[:, 1], pts[:, 0])  # -π to π → shift to 0..2π
    angles = angles % (2 * np.pi)
    radii = np.sqrt(pts[:, 0]**2 + pts[:, 1]**2)

    # Color by angle
    cmap = mcolors.LinearSegmentedColormap.from_list("bp", [BLUE, PURPLE, ORANGE])
    norm = plt.Normalize(0, 2 * np.pi)
    colors = cmap(norm(angles))

    fig = dark_fig((14, 6))
    ax1 = fig.add_subplot(121)
    ax2 = fig.add_subplot(122)
    dark_style(ax1, "Annulus (polar view)")
    dark_style(ax2, 'Unrolled Strip')

    # LEFT: annulus
    theta_ring = np.linspace(0, 2 * np.pi, 300)
    ax1.plot(r_lo * np.cos(theta_ring), r_lo * np.sin(theta_ring),
             color="#2a2a3e", linewidth=1)
    ax1.plot(r_hi * np.cos(theta_ring), r_hi * np.sin(theta_ring),
             color="#2a2a3e", linewidth=1)
    ax1.scatter(pts[:, 0], pts[:, 1], c=colors, s=12, zorder=3,
                edgecolors="none")
    ax1.set_aspect("equal")
    ax1.set_xlabel("Re", fontsize=11)
    ax1.set_ylabel("Im", fontsize=11)
    lim = r_hi + 4
    ax1.set_xlim(-lim, lim)
    ax1.set_ylim(-lim, lim)
    ax1.grid(True, color=GRID, linewidth=0.4, alpha=0.4)

    # RIGHT: unrolled
    ax2.scatter(angles, radii, c=colors, s=12, zorder=3, edgecolors="none")
    ax2.set_xlabel("Angle θ (radians)", fontsize=11)
    ax2.set_ylabel("Radius r", fontsize=11)
    ax2.set_xlim(-0.1, 2 * np.pi + 0.1)
    ax2.set_ylim(r_lo - 1, r_hi + 1)
    ax2.grid(True, color=GRID, linewidth=0.4, alpha=0.4)

    # Arrow between panels
    fig.text(0.50, 0.52, "Unroll →", color=GREEN, fontsize=16,
             fontweight="bold", ha="center", va="center",
             transform=fig.transFigure)

    # Caption
    fig.text(0.50, 0.02,
             "Far from origin, the annulus is locally flat (curvature ~ 1/R)",
             color=TEXT, fontsize=11, ha="center", style="italic",
             transform=fig.transFigure)

    fig.subplots_adjust(wspace=0.30)
    savefig(fig, "02_annulus_to_strip.png")


# ══════════════════════════════════════════════════════════════════════════
# Plot 3: Strip Geometry  (1200×800)
# ══════════════════════════════════════════════════════════════════════════
def plot_03():
    print("Plot 03: Strip geometry...")
    fig = dark_fig((12, 8))
    ax_top = fig.add_subplot(211)
    ax_bot = fig.add_subplot(212)
    dark_style(ax_top, "Fixed Angle: width grows with R → not comparable")
    dark_style(ax_bot, "Fixed Width: Δθ = W/R → comparable local statistics")

    # TOP: fixed-angle wedge at R=10 and R=30 (using smaller radii for visual clarity)
    angle = np.deg2rad(25)
    for R, col, xoff in [(10, BLUE, 0), (30, ORANGE, 0)]:
        theta = np.linspace(-angle / 2, angle / 2, 100)
        x_inner = (R - 1) * np.cos(theta) + xoff
        y_inner = (R - 1) * np.sin(theta) + xoff
        x_outer = (R + 1) * np.cos(theta) + xoff
        y_outer = (R + 1) * np.sin(theta) + xoff
        ax_top.plot(x_inner, y_inner, color=col, linewidth=1.5)
        ax_top.plot(x_outer, y_outer, color=col, linewidth=1.5)
        ax_top.plot([x_inner[0], x_outer[0]], [y_inner[0], y_outer[0]],
                    color=col, linewidth=1)
        ax_top.plot([x_inner[-1], x_outer[-1]], [y_inner[-1], y_outer[-1]],
                    color=col, linewidth=1)
        # arc width annotation
        w = R * np.sin(angle / 2) * 2
        ax_top.annotate(f"R={R}, width≈{w:.0f}",
                        xy=(R * np.cos(0), R * np.sin(angle / 2) + 1),
                        color=col, fontsize=10, ha="center")
        # Dashed radial line from origin
        ax_top.plot([0, (R + 1) * np.cos(0)], [0, 0],
                    color="#2a2a3e", linewidth=0.5, linestyle=":")
    ax_top.set_aspect("equal")
    ax_top.set_xlim(-3, 38)
    ax_top.set_ylim(-8, 12)
    ax_top.grid(True, color=GRID, linewidth=0.3, alpha=0.3)

    # BOTTOM: fixed-width strip at R=10 and R=30
    W = 4  # physical width
    for R, col in [(10, BLUE), (30, ORANGE)]:
        dtheta = W / R
        theta = np.linspace(-dtheta / 2, dtheta / 2, 100)
        x_inner = (R - 1) * np.cos(theta)
        y_inner = (R - 1) * np.sin(theta)
        x_outer = (R + 1) * np.cos(theta)
        y_outer = (R + 1) * np.sin(theta)
        ax_bot.plot(x_inner, y_inner, color=col, linewidth=1.5)
        ax_bot.plot(x_outer, y_outer, color=col, linewidth=1.5)
        ax_bot.plot([x_inner[0], x_outer[0]], [y_inner[0], y_outer[0]],
                    color=col, linewidth=1)
        ax_bot.plot([x_inner[-1], x_outer[-1]], [y_inner[-1], y_outer[-1]],
                    color=col, linewidth=1)
        ax_bot.annotate(f"R={R}, Δθ={np.degrees(dtheta):.1f}°, width={W}",
                        xy=(R, W / 2 + 1.5), color=col, fontsize=10, ha="center")
        ax_bot.plot([0, (R + 1)], [0, 0],
                    color="#2a2a3e", linewidth=0.5, linestyle=":")
    ax_bot.set_aspect("equal")
    ax_bot.set_xlim(-3, 38)
    ax_bot.set_ylim(-6, 8)
    ax_bot.grid(True, color=GRID, linewidth=0.3, alpha=0.3)

    fig.subplots_adjust(hspace=0.40)
    savefig(fig, "03_strip_geometry.png")


# ══════════════════════════════════════════════════════════════════════════
# Plot 4: Prime Angle Gaps  (1400×600)
# ══════════════════════════════════════════════════════════════════════════
def plot_04():
    print("Plot 04: Prime angle gaps (computing primes near R≈500)...")
    R = 500
    band = R  # norm² in [R²-R, R²+R]
    lo, hi = R * R - band, R * R + band

    # Gather first-quadrant primes by norm²
    angles = []
    limit = int(np.sqrt(hi)) + 2
    for a in range(1, limit):
        for b in range(1, limit):
            n2 = a * a + b * b
            if lo <= n2 <= hi and isprime(n2):
                angles.append(np.arctan2(b, a))

    angles = np.sort(angles)
    gaps = np.diff(angles)
    mean_gap = np.mean(gaps)
    s = gaps / mean_gap  # normalized gaps

    fig = dark_fig((14, 6))
    ax1 = fig.add_subplot(121)
    ax2 = fig.add_subplot(122)
    dark_style(ax1, f"Gaussian prime angle gaps (R≈{R})")
    dark_style(ax2, f"Uniform random angle gaps (same count)")

    bins = np.linspace(0, 5, 35)
    x_curve = np.linspace(0, 5, 200)
    poisson = np.exp(-x_curve)

    # LEFT: prime gaps
    ax1.hist(s, bins=bins, density=True, color=BLUE, alpha=0.7,
             edgecolor="#1a1a2e", linewidth=0.5)
    ax1.plot(x_curve, poisson, color=ORANGE, linewidth=2.5,
             label="exp(−s) (Poisson)")
    ax1.set_xlabel("Normalized gap s", fontsize=11)
    ax1.set_ylabel("Density", fontsize=11)
    ax1.legend(facecolor=BG, edgecolor="#2a2a3e", labelcolor=TEXT, fontsize=10)
    ax1.grid(True, color=GRID, linewidth=0.3, alpha=0.3)

    # RIGHT: random gaps
    n_pts = len(angles)
    rng = np.random.default_rng(42)
    rand_angles = np.sort(rng.uniform(0, np.pi / 2, n_pts))
    rand_gaps = np.diff(rand_angles)
    rand_mean = np.mean(rand_gaps)
    rs = rand_gaps / rand_mean

    ax2.hist(rs, bins=bins, density=True, color=PURPLE, alpha=0.7,
             edgecolor="#1a1a2e", linewidth=0.5)
    ax2.plot(x_curve, poisson, color=ORANGE, linewidth=2.5,
             label="exp(−s) (Poisson)")
    ax2.set_xlabel("Normalized gap s", fontsize=11)
    ax2.set_ylabel("Density", fontsize=11)
    ax2.legend(facecolor=BG, edgecolor="#2a2a3e", labelcolor=TEXT, fontsize=10)
    ax2.grid(True, color=GRID, linewidth=0.3, alpha=0.3)

    fig.suptitle("Rudnick–Waxman (2019): prime angle gaps ≈ random",
                 color=TEXT, fontsize=14, fontweight="bold", y=0.98)
    fig.subplots_adjust(wspace=0.25)
    savefig(fig, "04_prime_angle_gaps.png")


# ══════════════════════════════════════════════════════════════════════════
# Plot 5: Transfer Matrix  (1200×800)
# ══════════════════════════════════════════════════════════════════════════
def plot_05():
    print("Plot 05: Transfer matrix concept...")
    fig = dark_fig((12, 8))
    ax = fig.add_subplot(111)
    dark_style(ax)
    ax.set_xlim(0, 12)
    ax.set_ylim(0, 8)
    ax.set_aspect("equal")
    ax.axis("off")

    # Inner shell (bottom row)
    inner_x = np.linspace(1.5, 10.5, 8)
    inner_y = 5.5
    for i, x in enumerate(inner_x):
        ax.plot(x, inner_y, "o", color=BLUE, markersize=12, zorder=5)
        ax.text(x, inner_y - 0.5, f"i{i}", color=BLUE, fontsize=8,
                ha="center", va="top")

    # Outer shell (top row)
    outer_x = np.linspace(1, 11, 10)
    outer_y = 7.0
    alive = [True, True, False, True, True, True, False, True, False, True]
    for i, x in enumerate(outer_x):
        col = ORANGE if alive[i] else "#2a2a3e"
        ax.plot(x, outer_y, "o", color=col, markersize=12, zorder=5)
        label = f"o{i}" if alive[i] else "×"
        tcol = ORANGE if alive[i] else "#444"
        ax.text(x, outer_y + 0.45, label, color=tcol, fontsize=8,
                ha="center", va="bottom")

    # Connections
    connections = [
        (0, 0), (0, 1), (1, 1), (1, 3), (2, 3), (2, 4),
        (3, 4), (3, 5), (4, 5), (5, 5), (5, 7),
        (6, 7), (6, 9), (7, 9),
    ]
    for ii, oi in connections:
        if alive[oi]:
            ax.plot([inner_x[ii], outer_x[oi]],
                    [inner_y, outer_y],
                    color=GREEN, linewidth=1.2, alpha=0.6, zorder=3)

    ax.text(0.3, inner_y, "Inner\nshell", color=BLUE, fontsize=11,
            ha="center", va="center", fontweight="bold")
    ax.text(0.3, outer_y, "Outer\nshell", color=ORANGE, fontsize=11,
            ha="center", va="center", fontweight="bold")

    # Matrix below
    ax.text(6, 4.2, "Transfer Matrix T", color=TEXT, fontsize=14,
            ha="center", fontweight="bold")

    # Draw a simplified matrix grid
    mat_left, mat_right = 2.5, 9.5
    mat_top, mat_bot = 3.8, 1.5
    n_rows, n_cols = 8, 10
    cell_w = (mat_right - mat_left) / n_cols
    cell_h = (mat_top - mat_bot) / n_rows

    # Border
    rect = patches.Rectangle((mat_left, mat_bot),
                               mat_right - mat_left, mat_top - mat_bot,
                               linewidth=1.5, edgecolor="#2a2a3e",
                               facecolor="#0f0f1a", zorder=2)
    ax.add_patch(rect)

    # Fill connected cells
    for ii, oi in connections:
        if alive[oi]:
            cell = patches.Rectangle(
                (mat_left + oi * cell_w, mat_top - (ii + 1) * cell_h),
                cell_w, cell_h,
                facecolor=GREEN, alpha=0.5, zorder=3)
            ax.add_patch(cell)

    # Dead columns
    for oi in range(n_cols):
        if not alive[oi]:
            dead = patches.Rectangle(
                (mat_left + oi * cell_w, mat_bot),
                cell_w, mat_top - mat_bot,
                facecolor=RED, alpha=0.15, zorder=3)
            ax.add_patch(dead)

    ax.text(mat_left - 0.2, (mat_top + mat_bot) / 2, "inner →",
            color=BLUE, fontsize=9, ha="right", va="center", rotation=90)
    ax.text((mat_left + mat_right) / 2, mat_bot - 0.3, "← outer",
            color=ORANGE, fontsize=9, ha="center", va="top")

    ax.text(6, 0.8, "Green = reachable within step distance √k    "
            "Red columns = dead (no connections)",
            color=TEXT, fontsize=10, ha="center", style="italic")

    ax.set_title("The Transfer Matrix: inner boundary → outer boundary",
                 color=TEXT, fontsize=14, fontweight="bold", pad=10)

    savefig(fig, "05_transfer_matrix.png")


# ══════════════════════════════════════════════════════════════════════════
# Plot 6: Four-Sided Tile  (1000×800)
# ══════════════════════════════════════════════════════════════════════════
def plot_06():
    print("Plot 06: Four-sided tile...")
    fig = dark_fig((10, 8))
    ax = fig.add_subplot(111)
    dark_style(ax)
    ax.set_xlim(-1, 11)
    ax.set_ylim(-1.5, 10)
    ax.set_aspect("equal")
    ax.axis("off")

    # Main rectangle
    rx, ry, rw, rh = 1.5, 1.5, 7, 6
    rect = patches.FancyBboxPatch((rx, ry), rw, rh,
                                   boxstyle="round,pad=0.1",
                                   linewidth=2, edgecolor="#3a3a5e",
                                   facecolor="#0f0f1a", zorder=2)
    ax.add_patch(rect)

    # Overlap collar (dashed)
    collar = 0.7
    collar_rect = patches.FancyBboxPatch(
        (rx - collar, ry - collar), rw + 2 * collar, rh + 2 * collar,
        boxstyle="round,pad=0.1",
        linewidth=1.5, edgecolor=PURPLE, linestyle="--",
        facecolor="none", zorder=1, alpha=0.7)
    ax.add_patch(collar_rect)
    ax.text(rx - collar - 0.1, ry + rh + collar + 0.3,
            f"Overlap collar (width √k)", color=PURPLE, fontsize=9,
            ha="left", style="italic")

    # Interior primes (gray dots)
    rng = np.random.default_rng(123)
    n_interior = 30
    ix = rng.uniform(rx + 1, rx + rw - 1, n_interior)
    iy = rng.uniform(ry + 1, ry + rh - 1, n_interior)
    ax.scatter(ix, iy, s=25, color="#555577", zorder=4, alpha=0.7)

    # Face labels and boundary ports
    # Left (L) — blue
    ly = np.linspace(ry + 0.8, ry + rh - 0.8, 5)
    ax.scatter([rx] * 5, ly, s=60, color=BLUE, zorder=5, edgecolors="white",
               linewidths=0.8)
    ax.text(rx - 0.6, ry + rh / 2, "L\n(left)", color=BLUE, fontsize=12,
            ha="center", va="center", fontweight="bold")

    # Right (R) — blue
    ry_pts = np.linspace(ry + 0.8, ry + rh - 0.8, 5)
    ax.scatter([rx + rw] * 5, ry_pts, s=60, color=BLUE, zorder=5,
               edgecolors="white", linewidths=0.8)
    ax.text(rx + rw + 0.6, ry + rh / 2, "R\n(right)", color=BLUE,
            fontsize=12, ha="center", va="center", fontweight="bold")

    # Top — outer (O) — orange
    tx = np.linspace(rx + 0.8, rx + rw - 0.8, 7)
    ax.scatter(tx, [ry + rh] * 7, s=60, color=ORANGE, zorder=5,
               edgecolors="white", linewidths=0.8)
    ax.text(rx + rw / 2, ry + rh + 0.55, "O (outer radial)", color=ORANGE,
            fontsize=12, ha="center", va="bottom", fontweight="bold")

    # Bottom — inner (I) — green
    bx = np.linspace(rx + 0.8, rx + rw - 0.8, 7)
    ax.scatter(bx, [ry] * 7, s=60, color=GREEN, zorder=5,
               edgecolors="white", linewidths=0.8)
    ax.text(rx + rw / 2, ry - 0.55, "I (inner radial)", color=GREEN,
            fontsize=12, ha="center", va="top", fontweight="bold")

    ax.set_title("Four-Sided Tile Operator\nT = (ports on I, O, L, R, connectivity, flow lift)",
                 color=TEXT, fontsize=13, fontweight="bold", pad=15)

    # Legend
    ax.text(rx + rw / 2, -1.0,
            "Gray = interior primes (eliminated via Schur complement)   "
            "Colored = boundary ports",
            color=TEXT, fontsize=9, ha="center", style="italic")

    savefig(fig, "06_four_sided_tile.png")


# ══════════════════════════════════════════════════════════════════════════
# Plot 7: Three Observables  (1200×900)
# ══════════════════════════════════════════════════════════════════════════
def plot_07():
    print("Plot 07: Three observables...")
    rng = np.random.default_rng(77)
    n_shells = 20
    shells = np.arange(1, n_shells + 1)

    # Channel count: starts ~50, fluctuates, trending down
    channels = np.clip(
        50 - 1.5 * shells + rng.normal(0, 5, n_shells), 0, None
    ).astype(int)
    # Force a few to zero for drama
    channels[14] = 0
    channels[17] = 0

    crossing = channels > 0

    # Gap-run field: angular sectors × shells
    n_sectors = 30
    gap_field = rng.exponential(1.0, (n_shells, n_sectors))
    # Create a "danger corridor" in sectors 12-16
    for s in range(n_sectors):
        if 12 <= s <= 16:
            gap_field[:, s] *= 2.5
    # Where channels=0, make gaps huge
    for i in range(n_shells):
        if channels[i] == 0:
            gap_field[i, :] *= 3

    fig = dark_fig((12, 9))

    # TOP: Boolean crossing
    ax1 = fig.add_subplot(311)
    dark_style(ax1, "Boolean Crossing: does ANY path cross?")
    colors_bool = [GREEN if c else RED for c in crossing]
    ax1.bar(shells, [1] * n_shells, color=colors_bool, edgecolor="#1a1a2e",
            linewidth=0.5, width=0.8)
    ax1.set_yticks([])
    ax1.set_xlabel("Shell", fontsize=10)
    ax1.set_xticks(shells)
    # Labels
    for i, c in enumerate(crossing):
        ax1.text(shells[i], 0.5, "✓" if c else "✗",
                 ha="center", va="center", fontsize=11,
                 color="white" if c else "#ffcccc", fontweight="bold")

    # MIDDLE: Channel count
    ax2 = fig.add_subplot(312)
    dark_style(ax2, "Channel Count (independent crossing paths)")
    ax2.fill_between(shells, channels, alpha=0.3, color=BLUE)
    ax2.plot(shells, channels, color=BLUE, linewidth=2, marker="o",
             markersize=5, zorder=4)
    # Mark moat shells
    for i in range(n_shells):
        if channels[i] == 0:
            ax2.axvline(shells[i], color=RED, linewidth=1.5, alpha=0.5,
                        linestyle="--")
            ax2.text(shells[i], max(channels) * 0.8, "MOAT",
                     color=RED, fontsize=8, ha="center", fontweight="bold",
                     rotation=90)
    ax2.set_xlabel("Shell", fontsize=10)
    ax2.set_ylabel("Channels", fontsize=10)
    ax2.set_xticks(shells)
    ax2.grid(True, color=GRID, linewidth=0.3, alpha=0.3)

    # BOTTOM: Gap-run field heatmap
    ax3 = fig.add_subplot(313)
    dark_style(ax3, "Gap-Run Field (large gaps persist = danger)")
    cmap_heat = mcolors.LinearSegmentedColormap.from_list(
        "gap", ["#0a0a2e", BLUE, ORANGE, RED])
    im = ax3.imshow(gap_field, aspect="auto", cmap=cmap_heat,
                    extent=[0.5, n_sectors + 0.5, n_shells + 0.5, 0.5],
                    interpolation="nearest")
    ax3.set_xlabel("Angular sector", fontsize=10)
    ax3.set_ylabel("Shell", fontsize=10)
    cbar = fig.colorbar(im, ax=ax3, shrink=0.8, pad=0.02)
    cbar.set_label("Gap size", color=TEXT, fontsize=9)
    cbar.ax.tick_params(colors=TEXT)

    # Danger corridor annotation
    ax3.annotate("Danger\ncorridor", xy=(14, 10), xytext=(22, 5),
                 color=RED, fontsize=10, fontweight="bold",
                 arrowprops=dict(arrowstyle="->", color=RED, lw=1.5))

    fig.subplots_adjust(hspace=0.55)
    savefig(fig, "07_three_observables.png")


# ══════════════════════════════════════════════════════════════════════════
# Plot 8: Anderson Localization  (1200×700)
# ══════════════════════════════════════════════════════════════════════════
def plot_08():
    print("Plot 08: Anderson localization...")
    fig = dark_fig((12, 7))

    # TOP: Decaying wave with random barriers
    ax1 = fig.add_subplot(211)
    dark_style(ax1, "Wave Propagation Through Random Barriers")
    x = np.linspace(0, 50, 1000)
    gamma = 0.06
    envelope = np.exp(-gamma * x)
    wave = envelope * np.sin(2 * np.pi * x / 3)

    ax1.fill_between(x, envelope, -envelope, alpha=0.1, color=BLUE)
    ax1.plot(x, wave, color=BLUE, linewidth=1.2, alpha=0.8)
    ax1.plot(x, envelope, color=ORANGE, linewidth=1.5, linestyle="--",
             label="Envelope exp(−γx)")
    ax1.plot(x, -envelope, color=ORANGE, linewidth=1.5, linestyle="--")

    # Random barriers
    rng = np.random.default_rng(42)
    barrier_x = rng.choice(np.arange(2, 48), size=12, replace=False)
    barrier_h = rng.uniform(0.3, 1.0, len(barrier_x))
    for bx, bh in zip(barrier_x, barrier_h):
        ax1.bar(bx, bh, width=0.4, bottom=-bh / 2, color="#555577",
                alpha=0.5, zorder=1)

    ax1.set_xlabel("Distance (shells)", fontsize=10)
    ax1.set_ylabel("Amplitude", fontsize=10)
    ax1.legend(facecolor=BG, edgecolor="#2a2a3e", labelcolor=TEXT,
               fontsize=10, loc="upper right")
    ax1.set_ylim(-1.3, 1.3)
    ax1.grid(True, color=GRID, linewidth=0.3, alpha=0.3)

    # BOTTOM: log(amplitude) vs distance
    ax2 = fig.add_subplot(212)
    dark_style(ax2, "Lyapunov Exponent γ = slope of log|amplitude|")

    # Three scenarios
    x_log = np.linspace(1, 50, 200)
    for gamma_val, color, label, lstyle in [
        (-0.02, GREEN, "γ < 0 (robust — signal grows)", "-"),
        (0.0, ORANGE, "γ = 0 (critical)", "--"),
        (0.06, RED, "γ > 0 (localized → MOAT)", "-"),
    ]:
        log_amp = -gamma_val * x_log
        # Add small noise for realism
        noise = rng.normal(0, 0.15, len(x_log))
        ax2.plot(x_log, log_amp + noise * 0.3, color=color, linewidth=2,
                 label=label, linestyle=lstyle, alpha=0.9)

    ax2.set_xlabel("Distance (shells)", fontsize=10)
    ax2.set_ylabel("log|amplitude|", fontsize=10)
    ax2.legend(facecolor=BG, edgecolor="#2a2a3e", labelcolor=TEXT,
               fontsize=10, loc="lower left")
    ax2.grid(True, color=GRID, linewidth=0.3, alpha=0.3)

    # Annotate slope
    ax2.annotate("slope = −γ", xy=(35, -35 * 0.06), xytext=(40, -0.5),
                 color=RED, fontsize=12, fontweight="bold",
                 arrowprops=dict(arrowstyle="->", color=RED, lw=1.5))

    fig.subplots_adjust(hspace=0.45)
    savefig(fig, "08_anderson_localization.png")


# ══════════════════════════════════════════════════════════════════════════
# Plot 9: Cost Comparison  (1000×600)
# ══════════════════════════════════════════════════════════════════════════
def plot_09():
    print("Plot 09: Cost comparison...")
    fig = dark_fig((10, 6))
    ax = fig.add_subplot(111)
    dark_style(ax, "Computational Cost: Full Annulus vs. Strip Sampling")

    labels = [
        "Full annulus\n80M→10B",
        "One strip\nW=128",
        "One strip\nW=256",
        "Boundary ports\nper tile",
    ]
    values = [1e18, 3.7e10, 7.3e10, 25]
    colors = [RED, BLUE, BLUE, GREEN]

    bars = ax.bar(range(len(labels)), values, color=colors, alpha=0.85,
                  edgecolor="#1a1a2e", linewidth=1, width=0.6)
    ax.set_yscale("log")
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, fontsize=10)
    ax.set_ylabel("Scale (log)", fontsize=11)
    ax.grid(True, axis="y", color=GRID, linewidth=0.3, alpha=0.3)

    # Value labels on bars
    for i, (bar, val) in enumerate(zip(bars, values)):
        if val >= 1e6:
            txt = f"{val:.1e}"
        else:
            txt = str(int(val))
        ax.text(bar.get_x() + bar.get_width() / 2, val * 2, txt,
                ha="center", va="bottom", color=TEXT, fontsize=10,
                fontweight="bold")

    # Compression arrow
    ax.annotate("", xy=(2.6, 1e11), xytext=(0.4, 1e17),
                arrowprops=dict(arrowstyle="->", color=GREEN, lw=2.5,
                                connectionstyle="arc3,rad=0.2"))
    ax.text(1.5, 1e14, "~10⁷× compression", color=GREEN, fontsize=13,
            fontweight="bold", ha="center", rotation=-15)

    savefig(fig, "09_cost_comparison.png")


# ══════════════════════════════════════════════════════════════════════════
# Plot 10: Rosetta Stone  (1200×700)
# ══════════════════════════════════════════════════════════════════════════
def plot_10():
    print("Plot 10: Rosetta stone table...")
    rows = [
        ("Connectivity operator", "Transfer matrix",
         "Boundary-to-boundary reachability"),
        ("Translate to (0,0)", "Cylinder coordinates",
         "Local coords, curvature vanishes"),
        ("Flow / electricity", "Schur complement",
         "Eliminate interior, keep boundary"),
        ("Matrix operations", "Transfer matrix composition",
         "Multiply shells, extract Lyapunov"),
        ("Annulus throughput", "Conductance / Min-cut",
         "How much transport flows"),
        ("Measuring decay", "Lyapunov exponent",
         "Rate predicts WHERE the moat lives"),
        ("Bottleneck tree (PTO)", "EMST restriction",
         "Exact threshold connectivity"),
        ("Strip not annulus", "Fixed-width sampling",
         "Comparable local statistics"),
        ("Four-sided tile", "Four-face operator",
         "I/O/L/R + overlap collar"),
        ("Gap-run alarm", "Dangerous gap persistence",
         "Run-length across shells"),
    ]

    fig = dark_fig((12, 7))
    ax = fig.add_subplot(111)
    ax.set_facecolor(BG)
    ax.axis("off")

    ax.set_title("Rosetta Stone: Engineering ↔ Mathematics",
                 color=TEXT, fontsize=16, fontweight="bold", pad=20)

    # Table layout
    col_widths = [0.30, 0.30, 0.40]
    headers = ["Your Engineering Idea", "Formal Name", "One-Line Meaning"]
    header_colors = [BLUE, ORANGE, GREEN]

    # Draw table manually for full style control
    n_rows = len(rows) + 1  # +1 for header
    row_h = 0.065
    start_y = 0.88
    start_x = 0.05

    for ci, (header, hcolor, cw) in enumerate(
            zip(headers, header_colors, col_widths)):
        x = start_x + sum(col_widths[:ci])
        # Header
        ax.text(x + cw / 2, start_y, header,
                transform=ax.transAxes, fontsize=11, fontweight="bold",
                color=hcolor, ha="center", va="center")

    # Separator line
    ax.plot([start_x, start_x + sum(col_widths)],
            [start_y - row_h * 0.5, start_y - row_h * 0.5],
            transform=ax.transAxes, color="#2a2a3e", linewidth=1.5)

    for ri, (eng, formal, meaning) in enumerate(rows):
        y = start_y - (ri + 1) * row_h - row_h * 0.3
        # Alternating row background
        if ri % 2 == 0:
            ax.axhspan(y - row_h * 0.4, y + row_h * 0.4,
                       xmin=start_x, xmax=start_x + sum(col_widths),
                       facecolor="#12121f", alpha=0.5, zorder=0)

        vals = [eng, formal, meaning]
        val_colors = ["#c0c0d8", "#e0d0b0", "#b0e0c0"]
        for ci, (val, vcol, cw) in enumerate(
                zip(vals, val_colors, col_widths)):
            x = start_x + sum(col_widths[:ci])
            ax.text(x + cw / 2, y, val,
                    transform=ax.transAxes, fontsize=9.5,
                    color=vcol, ha="center", va="center")

    savefig(fig, "10_rosetta_stone.png")


# ══════════════════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════════════════
if __name__ == "__main__":
    print(f"Output directory: {OUT}")
    plot_01()
    plot_02()
    plot_03()
    plot_04()
    plot_05()
    plot_06()
    plot_07()
    plot_08()
    plot_09()
    plot_10()
    print("\nAll plots generated.")
