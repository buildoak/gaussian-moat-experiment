#!/usr/bin/env python3
"""
Gaussian Moat Tile-Probe Visual Explainer
3 diagrams: tile concept, band_io vs acc_io concept, k²=26 probe profile
"""

import json
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch
from matplotlib.collections import LineCollection
from pathlib import Path

OUT = Path(__file__).parent

# ---------- shared style ----------
BG       = '#1a1a2e'
PANEL_BG = '#16213e'
GREEN    = '#00ff87'
RED      = '#ff4757'
YELLOW   = '#ffd32a'
CYAN     = '#00d2d3'
WHITE    = '#e8e8e8'
GRAY     = '#636e72'
DIM      = '#4a4a6a'
PINK_BG  = '#3d1a2e'
GREEN_BG = '#1a3d2e'

plt.rcParams.update({
    'figure.facecolor': BG,
    'axes.facecolor': PANEL_BG,
    'text.color': WHITE,
    'axes.labelcolor': WHITE,
    'xtick.color': GRAY,
    'ytick.color': GRAY,
    'axes.edgecolor': DIM,
    'font.family': 'monospace',
    'font.size': 11,
})


# ============================================================
# DIAGRAM 1 — Tile Concept
# ============================================================
def diagram1():
    fig, ax = plt.subplots(figsize=(14, 9))
    ax.set_xlim(-1.5, 17)
    ax.set_ylim(-2.5, 12)
    ax.set_aspect('equal')
    ax.axis('off')
    fig.suptitle("Tile-Probe: How We Test Connectivity",
                 fontsize=18, fontweight='bold', color=WHITE, y=0.98)

    # --- subtitle / explanation ---
    fig.text(0.5, 0.935,
             "Divide the complex plane into rectangular tiles. Find Gaussian primes in each tile.\n"
             "Build a graph (primes within step distance connected). Track which components touch which faces.",
             ha='center', fontsize=9.5, color=GRAY, linespacing=1.5)

    cols, rows = 4, 3
    tw, th = 3.0, 2.5  # tile width, height
    ox, oy = 0.5, 0.5  # origin offset

    # draw grid of tiles
    for r in range(rows):
        for c in range(cols):
            x0 = ox + c * tw
            y0 = oy + r * th
            rect = mpatches.FancyBboxPatch(
                (x0, y0), tw, th,
                boxstyle="round,pad=0.04",
                facecolor=PANEL_BG, edgecolor=DIM, linewidth=1.2
            )
            ax.add_patch(rect)

    # --- highlight tile [1,1] for face labeling ---
    ht_c, ht_r = 1, 1  # highlight tile column, row
    hx = ox + ht_c * tw
    hy = oy + ht_r * th

    highlight = mpatches.FancyBboxPatch(
        (hx, hy), tw, th,
        boxstyle="round,pad=0.04",
        facecolor='#1e2d4d', edgecolor=CYAN, linewidth=2.5
    )
    ax.add_patch(highlight)

    # face labels on highlighted tile
    label_kw = dict(fontsize=10, fontweight='bold', ha='center', va='center')
    # Inner (bottom)
    ax.text(hx + tw/2, hy - 0.30, 'I (Inner)', color=CYAN, **label_kw)
    ax.annotate('', xy=(hx + tw/2, hy), xytext=(hx + tw/2, hy - 0.20),
                arrowprops=dict(arrowstyle='->', color=CYAN, lw=1.5))
    # Outer (top)
    ax.text(hx + tw/2, hy + th + 0.30, 'O (Outer)', color=CYAN, **label_kw)
    ax.annotate('', xy=(hx + tw/2, hy + th), xytext=(hx + tw/2, hy + th + 0.20),
                arrowprops=dict(arrowstyle='->', color=CYAN, lw=1.5))
    # Left
    ax.text(hx - 0.40, hy + th/2, 'L', color=CYAN, fontsize=10, fontweight='bold',
            ha='center', va='center', rotation=90)
    # Right
    ax.text(hx + tw + 0.40, hy + th/2, 'R', color=CYAN, fontsize=10, fontweight='bold',
            ha='center', va='center', rotation=90)

    # --- scatter gaussian primes in all tiles ---
    rng = np.random.RandomState(42)
    for r in range(rows):
        for c in range(cols):
            x0 = ox + c * tw
            y0 = oy + r * th
            n_pts = rng.randint(12, 22)
            px = x0 + 0.25 + rng.rand(n_pts) * (tw - 0.5)
            py = y0 + 0.25 + rng.rand(n_pts) * (th - 0.5)
            ax.scatter(px, py, s=14, color=DIM, zorder=3, alpha=0.5)

    # --- draw a green I→O spanning component in tile [1,1] ---
    # manually place primes that form a chain from bottom to top
    io_pts = np.array([
        [hx + 1.0, hy + 0.15],    # touches Inner
        [hx + 1.3, hy + 0.55],
        [hx + 0.8, hy + 1.0],
        [hx + 1.5, hy + 1.3],
        [hx + 1.2, hy + 1.8],
        [hx + 1.6, hy + 2.1],
        [hx + 1.3, hy + th - 0.15],  # touches Outer
    ])
    ax.scatter(io_pts[:, 0], io_pts[:, 1], s=40, color=GREEN, zorder=5, edgecolors='white', linewidths=0.5)
    for i in range(len(io_pts) - 1):
        ax.plot([io_pts[i,0], io_pts[i+1,0]], [io_pts[i,1], io_pts[i+1,1]],
                color=GREEN, lw=2, alpha=0.8, zorder=4)

    # label
    ax.annotate('I→O component\n(spans bottom to top)',
                xy=(io_pts[3, 0], io_pts[3, 1]),
                xytext=(hx + tw + 1.2, hy + th/2 + 0.6),
                fontsize=9, color=GREEN, fontweight='bold',
                arrowprops=dict(arrowstyle='->', color=GREEN, lw=1.5),
                ha='left', va='center')

    # --- draw a gray L↔R component in tile [1,1] ---
    lr_pts = np.array([
        [hx + 0.12, hy + 0.7],   # touches Left
        [hx + 0.6,  hy + 0.9],
        [hx + 1.8,  hy + 0.6],
        [hx + tw - 0.12, hy + 0.8],  # touches Right
    ])
    ax.scatter(lr_pts[:, 0], lr_pts[:, 1], s=35, color=GRAY, zorder=5,
              edgecolors='white', linewidths=0.5)
    for i in range(len(lr_pts) - 1):
        ax.plot([lr_pts[i,0], lr_pts[i+1,0]], [lr_pts[i,1], lr_pts[i+1,1]],
                color=GRAY, lw=1.8, alpha=0.7, zorder=4)

    ax.annotate('L↔R component\n(horizontal only)',
                xy=(lr_pts[2, 0], lr_pts[2, 1]),
                xytext=(hx + tw + 1.2, hy + 0.2),
                fontsize=9, color=GRAY,
                arrowprops=dict(arrowstyle='->', color=GRAY, lw=1.5),
                ha='left', va='center')

    # --- COMPOSE ARROWS ---
    # Horizontal compose: L↔R merge
    arr_y = oy + rows * th + 0.7
    ax.annotate('', xy=(ox + 2*tw + 0.1, arr_y), xytext=(ox + tw - 0.1, arr_y),
                arrowprops=dict(arrowstyle='<->', color=YELLOW, lw=2.5))
    ax.text(ox + 1.5*tw, arr_y + 0.35, '← Compose Horizontally (L↔R seam) →',
            fontsize=10, color=YELLOW, fontweight='bold', ha='center')
    ax.text(ox + 1.5*tw, arr_y - 0.35, 'Strips merge into a BAND',
            fontsize=9, color=YELLOW, ha='center', alpha=0.7)

    # Vertical compose: I↔O stack — draw to the left of the grid
    arr_x = ox - 0.9
    ax.annotate('', xy=(arr_x, oy + 2*th + 0.1), xytext=(arr_x, oy + th - 0.1),
                arrowprops=dict(arrowstyle='<->', color=RED, lw=2.5))
    ax.text(arr_x - 0.15, oy + 1.5*th, 'Compose\nVertically\n(I↔O stack)',
            fontsize=9, color=RED, fontweight='bold', ha='right', va='center')
    ax.text(arr_x - 0.15, oy + 1.5*th - 0.85, 'Bands stack\ninto probe',
            fontsize=8, color=RED, ha='right', va='center', alpha=0.7)

    # --- axis labels for the grid ---
    ax.text(ox + cols*tw/2, oy - 1.1, '← Real axis (a) →',
            fontsize=10, color=GRAY, ha='center')
    ax.text(ox - 1.3, oy + rows*th/2, '← Imaginary axis (b) →',
            fontsize=10, color=GRAY, ha='center', va='center', rotation=90)

    # row / col labels
    for c in range(cols):
        ax.text(ox + c*tw + tw/2, oy - 0.5, f'Strip {c}', fontsize=8, color=DIM, ha='center')
    for r in range(rows):
        ax.text(ox + cols*tw + 0.3, oy + r*th + th/2, f'Band {r}', fontsize=8, color=DIM,
                ha='left', va='center')

    # --- KEY at bottom ---
    legend_y = -1.8
    ax.plot([2], [legend_y], 'o', color=GREEN, markersize=8)
    ax.text(2.5, legend_y, 'Prime in I→O component', fontsize=9, color=GREEN, va='center')
    ax.plot([8], [legend_y], 'o', color=GRAY, markersize=8)
    ax.text(8.5, legend_y, 'Prime in L↔R component', fontsize=9, color=GRAY, va='center')
    ax.plot([2, 2.3], [legend_y - 0.5, legend_y - 0.5], '-', color=GREEN, lw=2)
    ax.text(2.5, legend_y - 0.5, 'Edge = primes within step ≤ √k²', fontsize=9, color=WHITE, va='center')

    fig.tight_layout(rect=[0.05, 0.02, 0.95, 0.92])
    fig.savefig(OUT / '2026-03-20-tile-concept.png', dpi=300,
                facecolor=BG, bbox_inches='tight', pad_inches=0.3)
    plt.close(fig)
    print("✓ Diagram 1 saved")


# ============================================================
# DIAGRAM 2 — band_io vs acc_io concept
# ============================================================
def diagram2():
    fig, ax = plt.subplots(figsize=(13, 11))
    ax.set_xlim(-3, 14)
    ax.set_ylim(-2.5, 14)
    ax.set_aspect('equal')
    ax.axis('off')
    fig.suptitle("band_io vs acc_io: Why Local Connectivity Matters",
                 fontsize=17, fontweight='bold', color=WHITE, y=0.97)
    fig.text(0.5, 0.940,
             "band_io = can you cross THIS band alone?   |   acc_io = is there an unbroken chain from bottom?",
             ha='center', fontsize=10, color=GRAY)

    bw = 8.0   # band width
    bh = 2.0   # band height
    gap = 0.15
    ox = 1.0
    oy = 0.5

    band_info = [
        # (label, band_io, bg_color, thread_color)
        ("Band 1", 3, GREEN_BG, GREEN),
        ("Band 2", 2, GREEN_BG, GREEN),
        ("Band 3", 1, GREEN_BG, GREEN),
        ("Band 4 — MOAT", 0, PINK_BG, None),
        ("Band 5", 2, GREEN_BG, GREEN),
    ]

    rng = np.random.RandomState(123)

    # acc_io: realistically, accumulated IO shrinks as bands compose (seam merges lose threads)
    # bands 1-3 are alive, moat kills it, band 5 is locally alive but chain is dead
    acc_io_values = [3, 1, 1, 0, 0]

    for i, (label, bio, bg, tc) in enumerate(band_info):
        y0 = oy + i * (bh + gap)

        # background rectangle
        rect = mpatches.FancyBboxPatch(
            (ox, y0), bw, bh,
            boxstyle="round,pad=0.06",
            facecolor=bg, edgecolor=DIM if bio > 0 else RED,
            linewidth=1.5 if bio > 0 else 2.5
        )
        ax.add_patch(rect)

        # band label on left
        ax.text(ox - 0.3, y0 + bh/2, label,
                fontsize=10, fontweight='bold', ha='right', va='center',
                color=RED if bio == 0 else WHITE)

        # I and O face labels
        if i == 0:
            ax.text(ox + bw/2, y0 - 0.35, 'I (Inner face)',
                    fontsize=8, color=CYAN, ha='center')
        if i == len(band_info) - 1:
            ax.text(ox + bw/2, y0 + bh + 0.35, 'O (Outer face)',
                    fontsize=8, color=CYAN, ha='center')

        # draw threads (I→O paths) inside band
        if bio > 0 and tc:
            for t in range(bio):
                # generate a wavy path from bottom to top of band
                x_start = ox + 0.8 + rng.rand() * (bw - 1.6)
                n_segs = 6
                xs = [x_start]
                ys = [y0 + 0.08]
                for s in range(1, n_segs):
                    xs.append(xs[-1] + rng.uniform(-0.6, 0.6))
                    xs[-1] = np.clip(xs[-1], ox + 0.3, ox + bw - 0.3)
                    ys.append(y0 + s * bh / n_segs)
                xs.append(xs[-1] + rng.uniform(-0.3, 0.3))
                ys.append(y0 + bh - 0.08)

                ax.plot(xs, ys, color=tc, lw=2.5, alpha=0.7, zorder=3)
                # dots at each node
                ax.scatter(xs, ys, s=20, color=tc, zorder=4, edgecolors='white', linewidths=0.3)
        elif bio == 0:
            # draw scattered disconnected dots — no chain
            n_dots = 25
            dx = ox + 0.4 + rng.rand(n_dots) * (bw - 0.8)
            dy = y0 + 0.3 + rng.rand(n_dots) * (bh - 0.6)
            ax.scatter(dx, dy, s=18, color=RED, alpha=0.4, zorder=3)
            # some short broken edges
            for j in range(0, n_dots - 1, 3):
                ax.plot([dx[j], dx[j+1]], [dy[j], dy[j+1]],
                        color=RED, lw=1.2, alpha=0.3, zorder=2)
            # big X or "NO PATH" label
            ax.text(ox + bw/2, y0 + bh/2, "NO I->O PATH",
                    fontsize=13, fontweight='bold', color=RED, ha='center', va='center',
                    alpha=0.9, zorder=5,
                    bbox=dict(boxstyle='round,pad=0.3', facecolor=BG, edgecolor=RED, alpha=0.8))

        # band_io annotation on the right
        if bio > 0:
            ax.text(ox + bw + 0.3, y0 + bh/2, f'band_io = {bio}',
                    fontsize=10, color=GREEN, fontweight='bold', ha='left', va='center')
        else:
            ax.text(ox + bw + 0.3, y0 + bh/2, 'band_io = 0',
                    fontsize=10, color=RED, fontweight='bold', ha='left', va='center')

    # --- draw inter-band connections (threads continuing across seams) ---
    # Connect bands 1→2→3 with vertical dashed lines
    for i in range(2):  # between bands 0-1, 1-2
        y_top = oy + (i + 1) * (bh + gap)  # bottom of upper band
        y_bot = oy + i * (bh + gap) + bh    # top of lower band
        for x_off in [2.0, 4.5, 6.5]:
            x = ox + x_off + rng.uniform(-0.3, 0.3)
            ax.plot([x, x + rng.uniform(-0.2, 0.2)], [y_bot, y_top],
                    color=GREEN, lw=1.8, alpha=0.5, linestyle='--', zorder=2)

    # NO connection across band 4 (the moat)
    # Band 5 has threads but no downward connections
    moat_y_bot = oy + 3 * (bh + gap) + bh  # top of band 3
    moat_y_top = oy + 3 * (bh + gap)        # bottom of band 4

    # --- acc_io annotation column (far right) ---
    anno_x = ox + bw + 3.8
    top_band_top = oy + 4*(bh+gap) + bh
    ax.text(anno_x, top_band_top + 0.5, 'acc_io', fontsize=12, fontweight='bold',
            color=YELLOW, ha='center', va='bottom')
    for i in range(5):
        y_center = oy + i * (bh + gap) + bh/2
        val = acc_io_values[i]
        c = GREEN if val > 0 else RED
        ax.text(anno_x, y_center, str(val),
                fontsize=14, fontweight='bold', color=c, ha='center', va='center',
                bbox=dict(boxstyle='round,pad=0.2', facecolor=BG, edgecolor=c, alpha=0.8))

    # --- draw the "chain alive" arrow on the left ---
    chain_x = ox - 2.2
    # green arrow for bands 1-3
    y_start = oy + bh/2
    y_end = oy + 2*(bh+gap) + bh/2
    ax.annotate('', xy=(chain_x, y_end), xytext=(chain_x, y_start),
                arrowprops=dict(arrowstyle='->', color=GREEN, lw=3))
    ax.text(chain_x, (y_start + y_end)/2, 'Chain\nalive',
            fontsize=9, color=GREEN, fontweight='bold', ha='center', va='center',
            bbox=dict(boxstyle='round,pad=0.2', facecolor=BG, edgecolor=GREEN, alpha=0.6))

    # red X at moat
    moat_y = oy + 3*(bh+gap) + bh/2
    ax.text(chain_x, moat_y, '✕', fontsize=24, color=RED, ha='center', va='center',
            fontweight='bold')
    ax.text(chain_x, moat_y - 0.7, 'Chain\nbroken!', fontsize=9, color=RED,
            fontweight='bold', ha='center', va='center')

    # gray for band 5 — locally alive but disconnected from chain
    band5_y = oy + 4*(bh+gap) + bh/2
    ax.text(chain_x, band5_y, '—', fontsize=18, color=GRAY, ha='center', va='center')
    ax.text(chain_x, band5_y - 0.6, 'Locally\nalive but\nisolated', fontsize=8,
            color=GRAY, ha='center', va='center')

    # --- bottom explanation ---
    expl_y = -1.3
    ax.text(ox + bw/2, expl_y,
            "Key insight: band_io = 0 is the TRUE moat signal.\n"
            "acc_io drops permanently once any band breaks the chain.\n"
            "But band_io fluctuates — most bands are locally crossable even after the chain breaks.",
            fontsize=10, color=WHITE, ha='center', va='top', linespacing=1.6,
            bbox=dict(boxstyle='round,pad=0.5', facecolor=BG, edgecolor=DIM, alpha=0.9))

    fig.tight_layout(rect=[0.02, 0.03, 0.98, 0.92])
    fig.savefig(OUT / '2026-03-20-band-io-concept.png', dpi=300,
                facecolor=BG, bbox_inches='tight', pad_inches=0.3)
    plt.close(fig)
    print("✓ Diagram 2 saved")


# ============================================================
# DIAGRAM 3 — k²=26 Probe Profile from real data
# ============================================================
def diagram3():
    with open(OUT / '2026-03-20-k26-probe-trace.json') as f:
        data = json.load(f)

    shells = data['shells']
    R      = np.array([s['r_center'] / 1000 for s in shells])   # in units of K
    bio    = np.array([s['band_io'] for s in shells])
    acc_c  = np.array([s['acc_components'] for s in shells])
    primes = np.array([s['primes'] for s in shells])

    TSUCHIMURA_R = 1015.639  # in K units

    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(16, 12), sharex=True,
                                         gridspec_kw={'height_ratios': [1.3, 1, 1]})
    fig.suptitle("k²=26 Probe Profile: R = 900K – 1.1M",
                 fontsize=18, fontweight='bold', color=WHITE, y=0.97)
    fig.text(0.5, 0.94,
             "Step distance ≤ √26 ≈ 5.10  |  Strip width = 240  |  Tile depth = 2000  |  64 strips per band",
             fontsize=10, color=GRAY, ha='center')

    # ----- Top: band_io bar chart -----
    colors = [GREEN if v > 0 else RED for v in bio]
    bar_width = (R[1] - R[0]) * 0.85
    ax1.bar(R, bio, width=bar_width, color=colors, alpha=0.85, edgecolor='none', zorder=3)
    ax1.axvline(TSUCHIMURA_R, color=YELLOW, linestyle='--', lw=2, zorder=5, label='Tsuchimura moat R≈1,015,639')
    ax1.set_ylabel('band_io\n(I→O crossings)', fontsize=11, fontweight='bold')
    ax1.set_title('Local Connectivity: How many components span Inner→Outer per band',
                  fontsize=11, color=GRAY, pad=8)
    ax1.legend(loc='upper right', fontsize=10, framealpha=0.7,
               facecolor=PANEL_BG, edgecolor=DIM)

    # highlight the moat zones (band_io = 0)
    moat_shells = [i for i, v in enumerate(bio) if v == 0]
    for idx in moat_shells:
        ax1.axvspan(R[idx] - bar_width/2, R[idx] + bar_width/2,
                    color=RED, alpha=0.12, zorder=1)

    # annotate the Tsuchimura cluster
    tsuch_idx = [i for i in moat_shells if abs(R[i] - TSUCHIMURA_R) < 10]
    if tsuch_idx:
        mid = tsuch_idx[len(tsuch_idx)//2]
        ax1.annotate('Tsuchimura moat\nband_io = 0 here',
                     xy=(R[mid], 0.3), xytext=(R[mid] - 30, 5),
                     fontsize=10, color=YELLOW, fontweight='bold',
                     arrowprops=dict(arrowstyle='->', color=YELLOW, lw=2),
                     ha='center',
                     bbox=dict(boxstyle='round,pad=0.3', facecolor=BG, edgecolor=YELLOW, alpha=0.9))

    # annotate first chain break (acc_io drops to 0)
    first_break_idx = next(i for i, s in enumerate(shells) if s['acc_io'] == 0)
    ax1.annotate(f'Chain breaks at R≈{R[first_break_idx]:.0f}K\n(acc_io → 0, never recovers)',
                 xy=(R[first_break_idx], bio[first_break_idx] + 0.2),
                 xytext=(R[first_break_idx] + 20, 6),
                 fontsize=9, color=CYAN,
                 arrowprops=dict(arrowstyle='->', color=CYAN, lw=1.5),
                 ha='center',
                 bbox=dict(boxstyle='round,pad=0.3', facecolor=BG, edgecolor=CYAN, alpha=0.9))

    ax1.set_ylim(-0.3, max(bio) + 1.5)
    ax1.grid(axis='y', color=DIM, alpha=0.3)

    # ----- Middle: accumulated components -----
    ax2.plot(R, acc_c, color=CYAN, lw=2.2, zorder=3)
    ax2.fill_between(R, acc_c, alpha=0.15, color=CYAN)
    ax2.axvline(TSUCHIMURA_R, color=YELLOW, linestyle='--', lw=1.5, zorder=5)
    ax2.set_ylabel('Accumulated\ncomponents', fontsize=11, fontweight='bold')
    ax2.set_title('Fragmentation: Total disconnected components in accumulated tile (monotonic growth)',
                  fontsize=11, color=GRAY, pad=8)
    ax2.grid(axis='y', color=DIM, alpha=0.3)

    # annotate the growth
    ax2.annotate(f'{acc_c[0]:,}', xy=(R[0], acc_c[0]), xytext=(R[0]+5, acc_c[0]+1000),
                 fontsize=9, color=CYAN,
                 arrowprops=dict(arrowstyle='->', color=CYAN, lw=1))
    ax2.annotate(f'{acc_c[-1]:,}', xy=(R[-1], acc_c[-1]), xytext=(R[-1]-15, acc_c[-1]-1500),
                 fontsize=9, color=CYAN,
                 arrowprops=dict(arrowstyle='->', color=CYAN, lw=1))

    # ----- Bottom: primes per shell (zoomed to show variation) -----
    p_M = primes / 1e6
    ax3.plot(R, p_M, color=YELLOW, lw=2.2, zorder=3)
    # fill from a baseline near the minimum so the area is visible
    p_floor = p_M.min() - 0.002
    ax3.fill_between(R, p_floor, p_M, alpha=0.15, color=YELLOW)
    ax3.axvline(TSUCHIMURA_R, color=YELLOW, linestyle='--', lw=1.5, zorder=5)
    ax3.set_ylabel('Primes per shell\n(millions)', fontsize=11, fontweight='bold')
    ax3.set_xlabel('R (thousands)', fontsize=12, fontweight='bold')
    ax3.set_title('Prime Density: Gaussian primes per 2000-deep annular shell (zoomed)',
                  fontsize=11, color=GRAY, pad=8)
    ax3.grid(axis='y', color=DIM, alpha=0.3)
    # zoom y-axis to the actual data range with padding
    y_pad = (p_M.max() - p_M.min()) * 0.25
    ax3.set_ylim(p_M.min() - y_pad, p_M.max() + y_pad)

    # annotate density drop
    ax3.annotate(f'{p_M[0]:.4f}M', xy=(R[0], p_M[0]),
                 xytext=(R[0]+10, p_M[0] + y_pad*0.5),
                 fontsize=9, color=YELLOW,
                 arrowprops=dict(arrowstyle='->', color=YELLOW, lw=1))
    ax3.annotate(f'{p_M[-1]:.4f}M', xy=(R[-1], p_M[-1]),
                 xytext=(R[-1]-20, p_M[-1] + y_pad*0.5),
                 fontsize=9, color=YELLOW,
                 arrowprops=dict(arrowstyle='->', color=YELLOW, lw=1))

    # density gradient annotation
    drop_pct = (1 - primes[-1] / primes[0]) * 100
    ax3.text(R[len(R)//2], p_M.min() - y_pad*0.6,
             f'Density drops {drop_pct:.1f}% across range (primes thin out at larger R)',
             fontsize=9, color=GRAY, ha='center', va='top')

    for a in [ax1, ax2, ax3]:
        a.tick_params(colors=GRAY)

    fig.tight_layout(rect=[0.0, 0.0, 1.0, 0.93])
    fig.savefig(OUT / '2026-03-20-k26-profile.png', dpi=300,
                facecolor=BG, bbox_inches='tight', pad_inches=0.3)
    plt.close(fig)
    print("✓ Diagram 3 saved")


# ============================================================
if __name__ == '__main__':
    diagram1()
    diagram2()
    diagram3()
    print("\nAll 3 diagrams generated in:", OUT)
