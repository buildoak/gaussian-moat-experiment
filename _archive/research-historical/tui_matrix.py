#!/usr/bin/env python3
"""
Matrix-style TUI visualization for Gaussian moat tile connectivity data.

Renders a green-phosphor terminal grid showing per-tile I->O crossings
across shells in the moat region around k^2=26. Shell 0 at bottom,
highest shell at top -- like building upward toward the moat.

Usage:
    python3 tui_matrix.py [--static] [--width N] [--compact]

Data: 2026-03-20-k26-moat-region-tiles.json (auto-detected next to script)
"""

import json
import sys
import os
import time
import argparse
import shutil
import random

# ── Path defaults ────────────────────────────────────────────────────

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_JSON = os.path.join(SCRIPT_DIR, "2026-03-20-k26-moat-region-tiles.json")

# ── ANSI escape primitives ───────────────────────────────────────────

ESC = "\033"
CSI = f"{ESC}["

def cursor_home():      return f"{CSI}H"
def cursor_to(r, c):    return f"{CSI}{r};{c}H"
def clear_screen():     return f"{CSI}2J"
def clear_line():       return f"{CSI}2K"
def hide_cursor():      return f"{CSI}?25l"
def show_cursor():      return f"{CSI}?25h"
def rst():              return f"{CSI}0m"
def bld():              return f"{CSI}1m"
def dm():               return f"{CSI}2m"
def fg_rgb(r, g, b):    return f"{CSI}38;2;{r};{g};{b}m"
def bg_rgb(r, g, b):    return f"{CSI}48;2;{r};{g};{b}m"

# ── Color palette — Matrix phosphor aesthetic ────────────────────────

# Greens: bright → dark
G0 = (0, 255, 70)       # blazing phosphor
G1 = (0, 220, 55)       # bright
G2 = (0, 170, 40)       # medium
G3 = (0, 120, 28)       # dim
G4 = (0, 75, 18)        # faint (borders, guides)
G5 = (0, 45, 10)        # barely visible

# Reds: maroon for dead/disconnected
R0 = (140, 20, 20)      # bright-ish red
R1 = (90, 12, 12)       # medium maroon
R2 = (55, 8, 8)         # dark maroon

# Yellow/Amber: Tsuchimura moat
Y0 = (255, 200, 50)     # bright amber
Y1 = (200, 155, 35)     # medium
Y2 = (140, 110, 25)     # dim amber

# Grays
W0 = (220, 230, 220)    # near-white (greenish tint)
W1 = (140, 148, 140)    # mid gray
W2 = (80, 85, 80)       # dark gray
BK = (4, 6, 4)          # near-black with green cast

# Katakana + digits for Matrix rain
RAIN_GLYPHS = "ﾊﾐﾋﾎﾍﾑﾓﾒﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ012345789"

# Box drawing
H  = "\u2500"   # ─
V  = "\u2502"   # │
TL = "\u250c"   # ┌
TR = "\u2510"   # ┐
BL = "\u2514"   # └
BR = "\u2518"   # ┘
LJ = "\u251c"   # ├
RJ = "\u2524"   # ┤
DH = "\u2550"   # ═ (double horizontal for Tsuchimura)

# Blocks and dots
BF = "\u2588"   # █ full block
BH = "\u2593"   # ▓ dark shade
BM = "\u2592"   # ▒ medium shade
BL_ = "\u2591"  # ░ light shade
DOT = "\u00b7"  # ·

# ── Tsuchimura constant ─────────────────────────────────────────────

TSUCHIMURA_R = 1015639


# ── Data ─────────────────────────────────────────────────────────────

def load_data(path):
    with open(path) as f:
        return json.load(f)


# ── Cell rendering ───────────────────────────────────────────────────

def tile_glyph(io_val):
    """
    Returns (2-char string, fg_color) for a tile cell.
    io_val is the per-tile I->O crossing count.
    """
    if io_val == 0:
        return f"{DOT} ", R1            # dark maroon dot
    elif io_val == 1:
        return f"{BL_}{BL_}", G3        # dim green light shade
    elif io_val == 2:
        return f"{BM}{BM}", G2          # medium green
    elif io_val <= 5:
        return f"{BH}{BH}", G1          # bright green
    else:
        return f"{BF}{BF}", G0          # blazing phosphor


def tile_glyph_tsuch(io_val, compact=False):
    """Cell on the Tsuchimura moat line — amber-tinted zeros."""
    if io_val == 0:
        if compact:
            return DH, Y2               # single amber dash
        return f"{DH}{DH}", Y2          # amber double-dash
    else:
        if compact:
            ch, _ = tile_glyph_compact(io_val)
        else:
            ch, _ = tile_glyph(io_val)
        return ch, G1


def tile_glyph_compact(io_val):
    """1-char variant for --compact mode."""
    if io_val == 0:
        return DOT, R1
    elif io_val == 1:
        return BL_, G3
    elif io_val == 2:
        return BM, G2
    elif io_val <= 5:
        return BH, G1
    else:
        return BF, G0


# ── Row rendering ────────────────────────────────────────────────────

def render_row(shell, compact=False, is_tsuch=False):
    """Render one grid row: left margin + border + cells + border + sidebar."""
    tile_io = shell["tile_io"]
    shell_n = shell["shell"]
    alive = shell["alive"]
    acc_io = shell["acc_io"]
    band_io = shell["band_io"]
    r_val = shell["r_center"]
    nz = sum(1 for x in tile_io if x > 0)
    n = len(tile_io)

    o = ""

    # ── Left margin: shell number ──
    num_color = G3 if alive else W2
    o += fg_rgb(*num_color) + f"{shell_n:>2} "

    # ── Left border ──
    border_color = Y0 if is_tsuch else G4
    o += fg_rgb(*border_color) + V

    # ── Tile cells ──
    for io_val in tile_io:
        if is_tsuch:
            ch, color = tile_glyph_tsuch(io_val, compact)
        elif compact:
            ch, color = tile_glyph_compact(io_val)
        else:
            ch, color = tile_glyph(io_val)
        o += fg_rgb(*color) + ch

    # ── Right border ──
    o += fg_rgb(*border_color) + V

    # ── Sidebar ──
    # Status icon
    if alive and acc_io > 0:
        si = fg_rgb(*G0) + " \u25c9"    # bright green filled circle
    elif alive:
        si = fg_rgb(*G3) + " \u25c9"    # dim green filled circle
    else:
        si = fg_rgb(*W2) + " \u25cb"    # gray open circle

    # R value
    r_str = f"{r_val/1e6:.4f}M"

    # band_io color
    if band_io >= 8:
        bc = G0
    elif band_io >= 5:
        bc = G2
    elif band_io >= 1:
        bc = G3
    else:
        bc = R1

    # acc_io color
    if acc_io >= 5:
        ac = G0
    elif acc_io >= 1:
        ac = G2
    else:
        ac = R1

    # Coverage ratio
    pct = nz / n * 100 if n > 0 else 0
    if pct >= 30:
        pc = G2
    elif pct >= 10:
        pc = G3
    else:
        pc = W2

    r_color = G4 if alive else W2
    o += si
    o += fg_rgb(*r_color) + f" {r_str}"
    o += fg_rgb(*bc) + f" b:{band_io:<2}"
    o += fg_rgb(*ac) + f" a:{acc_io:<2}"
    o += fg_rgb(*pc) + f" {nz:>2}/{n}"
    o += rst()

    return o


def render_tsuchimura_line(grid_width, compact=False):
    """Render the amber Tsuchimura moat marker line (between rows)."""
    label = f" Tsuchimura moat  R = {TSUCHIMURA_R:,} "
    fill = grid_width - len(label)
    left = max(fill // 2, 0)
    right = max(fill - left, 0)

    o = fg_rgb(*Y2) + "   "           # margin
    o += fg_rgb(*Y0) + bld() + LJ
    o += fg_rgb(*Y1) + (DH * left)
    o += fg_rgb(*Y0) + bld() + label
    o += rst() + fg_rgb(*Y1) + (DH * right)
    o += fg_rgb(*Y0) + RJ
    o += rst()
    return o


# ── Header ───────────────────────────────────────────────────────────

def render_header(data, grid_width, compact=False):
    """Top header block: title + strip ruler + separator."""
    k_sq = data["k_sq"]
    r_min = data["r_min"]
    r_max = data["r_max"]
    num_strips = data["num_strips"]
    cw = 1 if compact else 2

    lines = []

    # Top border
    lines.append(fg_rgb(*G4) + "   " + TL + H * grid_width + TR + rst())

    # Title
    title = f"k\u00b2={k_sq} TILE CONNECTIVITY MATRIX"
    sub = f"R = {r_min/1e6:.3f}M \u2192 {r_max/1e6:.3f}M   Tsuchimura moat: R={TSUCHIMURA_R:,}"
    lines.append(
        bld() + fg_rgb(*G0) + "   " + title + rst() +
        fg_rgb(*G3) + "  " + sub + rst()
    )

    # Strip ruler
    ruler = fg_rgb(*G4) + "   " + V
    for i in range(num_strips):
        if compact:
            if i % 8 == 0:
                ruler += fg_rgb(*G3) + f"{i % 100 // 10}"
            elif i % 4 == 0:
                ruler += fg_rgb(*G5) + "\u00b7"
            else:
                ruler += fg_rgb(*G5) + " "
        else:
            if i % 4 == 0:
                ruler += fg_rgb(*G3) + f"{i:>2}"
            else:
                ruler += fg_rgb(*G5) + f" {DOT}"
    ruler += fg_rgb(*G4) + V + rst()
    lines.append(ruler)

    # Separator
    lines.append(fg_rgb(*G4) + "   " + LJ + H * grid_width + RJ + rst())

    return "\n".join(lines) + "\n"


# ── Footer ───────────────────────────────────────────────────────────

def render_footer(data, grid_width):
    """Bottom border + legend + summary."""
    shells = data["shells"]
    k_sq = data["k_sq"]

    lines = []

    # Bottom border
    lines.append(fg_rgb(*G4) + "   " + BL + H * grid_width + BR + rst())

    # Legend
    items = [
        (BL_, G3,  "I\u2192O crossing"),
        (DOT, R1,  "disconnected"),
        (DH,  Y0,  "Tsuchimura moat"),
        ("\u25c9", G0, "alive"),
        ("\u25cb", W2, "dead"),
    ]
    legend = "   "
    for ch, color, label in items:
        legend += fg_rgb(*color) + ch + " " + fg_rgb(*W1) + label + "   "
    legend += rst()
    lines.append(legend)

    # Stats
    alive_n = sum(1 for s in shells if s["alive"])
    dead_n = len(shells) - alive_n
    total_p = sum(s["primes"] for s in shells)
    first_dead = next((s for s in shells if not s["alive"]), None)

    stats = (
        "   " +
        bld() + fg_rgb(*G0) + f"k\u00b2={k_sq}" + rst() +
        fg_rgb(*G4) + " \u2502 " +
        fg_rgb(*W0) + f"{len(shells)} shells" +
        fg_rgb(*G4) + " \u2502 " +
        fg_rgb(*G2) + f"{total_p:,} primes" +
        fg_rgb(*G4) + " \u2502 " +
        fg_rgb(*G0) + f"{alive_n} alive" +
        fg_rgb(*G4) + " / " +
        fg_rgb(*R0) + f"{dead_n} dead" +
        rst()
    )
    lines.append(stats)

    if first_dead:
        moat = (
            "   " +
            fg_rgb(*Y0) + bld() +
            f"Connectivity dies at shell {first_dead['shell']} "
            f"(R = {first_dead['r_center']:,.0f})" +
            rst() + fg_rgb(*G4) +
            f"  \u2014  Tsuchimura record: R = {TSUCHIMURA_R:,}" +
            rst()
        )
        lines.append(moat)

    # Radial axis hint
    lines.append(
        fg_rgb(*G5) +
        "   \u2191 radial axis (R increasing upward)   "
        "shell 0 = innermost ring" +
        rst()
    )

    return "\n".join(lines) + "\n"


# ── Static mode ──────────────────────────────────────────────────────

def run_static(data, term_width, term_height, compact=False):
    """Print the complete grid, no animation. Good for screenshots / piping."""
    shells = data["shells"]
    num_strips = data["num_strips"]
    cw = 1 if compact else 2
    grid_width = num_strips * cw

    # Find Tsuchimura-nearest shell
    tsuch_idx = min(
        range(len(shells)),
        key=lambda i: abs(shells[i]["r_center"] - TSUCHIMURA_R)
    )

    out = ""
    out += render_header(data, grid_width, compact)

    # Print top-to-bottom: highest shell first, shell 0 last
    for i in range(len(shells) - 1, -1, -1):
        shell = shells[i]
        is_tsuch = (i == tsuch_idx)

        # Tsuchimura marker ABOVE the moat shell
        if is_tsuch:
            out += render_tsuchimura_line(grid_width, compact) + "\n"

        out += render_row(shell, compact, is_tsuch) + "\n"

    out += render_footer(data, grid_width)
    sys.stdout.write(out)
    sys.stdout.flush()


# ── Animated mode ────────────────────────────────────────────────────

def run_animated(data, term_width, term_height, compact=False):
    """
    Matrix rain animation.

    Shells appear one at a time from bottom (shell 0) to top (shell N).
    Each shell "falls in" from the top of the grid like Matrix rain,
    landing in its target row position. Green phosphor glow.
    """
    shells = data["shells"]
    num_strips = data["num_strips"]
    num_shells = len(shells)
    cw = 1 if compact else 2
    grid_width = num_strips * cw

    # Tsuchimura shell
    tsuch_idx = min(
        range(num_shells),
        key=lambda i: abs(shells[i]["r_center"] - TSUCHIMURA_R)
    )

    # Terminal layout
    header_lines = 4
    # grid rows: one per shell + 1 for Tsuchimura marker
    grid_rows = num_shells + 1  # +1 for the Tsuchimura marker line
    footer_lines = 5

    grid_top = header_lines + 1  # 1-indexed terminal row

    # The Tsuchimura marker occupies an extra row above shell tsuch_idx
    # Display mapping: shell i -> row position
    # Shells are top-to-bottom: shell (N-1) at top, shell 0 at bottom
    # Plus the Tsuchimura marker between shells tsuch_idx and tsuch_idx+1

    def shell_to_row(shell_idx):
        """Map shell index to terminal row (1-indexed)."""
        # Position from top: highest shell = 0, lowest = num_shells - 1
        pos_from_top = (num_shells - 1) - shell_idx
        # Account for Tsuchimura marker line if shell is above it
        if shell_idx > tsuch_idx:
            pos_from_top += 0  # marker is below these shells
        elif shell_idx <= tsuch_idx:
            pos_from_top += 1  # marker pushes these down by 1
        return grid_top + pos_from_top

    tsuch_marker_row = grid_top + (num_shells - 1 - tsuch_idx)

    # ── Phase 1: Draw frame ──
    sys.stdout.write(clear_screen() + hide_cursor())
    sys.stdout.write(cursor_home())

    # Header
    sys.stdout.write(render_header(data, grid_width, compact))

    # Empty grid skeleton
    total_grid_rows = num_shells + 1
    for ri in range(total_grid_rows):
        r = grid_top + ri
        sys.stdout.write(cursor_to(r, 1))
        sys.stdout.write(fg_rgb(*G5) + "   " + V)
        sys.stdout.write(" " * grid_width)
        sys.stdout.write(fg_rgb(*G5) + V + rst())
    sys.stdout.flush()
    time.sleep(0.4)

    # ── Phase 2: Rain shells in, bottom to top ──
    # We animate shell 0 first (it lands at the bottom), then shell 1, etc.
    # Each shell has a brief rain effect then snaps into place.

    rain_pool = list(RAIN_GLYPHS)

    for step in range(num_shells):
        shell = shells[step]
        target = shell_to_row(step)
        is_tsuch = (step == tsuch_idx)

        # ── Rain drops ──
        # Brief cascade of green chars falling from above into the target row
        n_drops = random.randint(3, 6)
        drop_cols = random.sample(range(num_strips), min(n_drops, num_strips))

        for phase in range(4):
            # Each phase: show chars at descending rows
            for col in drop_cols:
                col_x = 4 + col * cw + 1  # +4 for "NN │"
                # Drop position: starts high, descends toward target
                drop_r = target - (3 - phase) * 2
                drop_r = max(grid_top, min(drop_r, grid_top + total_grid_rows - 1))

                ch = random.choice(rain_pool)

                # Color: bright head, dimming trail
                if phase == 3:
                    c = G0  # brightest at landing
                elif phase == 2:
                    c = G1
                elif phase == 1:
                    c = G3
                else:
                    c = G5

                sys.stdout.write(cursor_to(drop_r, col_x) + fg_rgb(*c) + ch)

                # Erase previous position (trail cleanup)
                if phase > 0:
                    prev_r = drop_r - 2
                    if grid_top <= prev_r <= grid_top + total_grid_rows - 1:
                        sys.stdout.write(cursor_to(prev_r, col_x) + " ")

            sys.stdout.flush()
            time.sleep(0.012)

        # Clean up any lingering rain chars in the column
        for col in drop_cols:
            col_x = 4 + col * cw + 1
            for cleanup_r in range(grid_top, grid_top + total_grid_rows):
                if cleanup_r != target:
                    sys.stdout.write(cursor_to(cleanup_r, col_x) + " ")

        # ── Place the actual row ──
        row_str = render_row(shell, compact, is_tsuch)
        sys.stdout.write(cursor_to(target, 1) + row_str)

        # ── Tsuchimura marker ──
        if is_tsuch:
            marker = render_tsuchimura_line(grid_width, compact)
            sys.stdout.write(cursor_to(tsuch_marker_row, 1) + marker)
            # Brief amber flash
            sys.stdout.flush()
            time.sleep(0.3)
        else:
            sys.stdout.flush()

        # Pacing
        if shell["alive"]:
            time.sleep(0.10)
        elif is_tsuch:
            time.sleep(0.20)
        else:
            time.sleep(0.04)

    # ── Phase 3: Footer ──
    footer_row = grid_top + total_grid_rows + 1
    sys.stdout.write(cursor_to(footer_row, 1))
    sys.stdout.write(render_footer(data, grid_width))
    sys.stdout.flush()

    # Hold for viewing
    time.sleep(3.0)

    # Restore terminal
    end_row = footer_row + footer_lines + 2
    sys.stdout.write(cursor_to(end_row, 1) + show_cursor() + rst())
    sys.stdout.flush()


# ── Main ─────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Matrix-style TUI for Gaussian moat tile connectivity"
    )
    parser.add_argument(
        "json_file", nargs="?", default=DEFAULT_JSON,
        help="Path to JSON tile data (default: auto-detect)"
    )
    parser.add_argument(
        "--static", action="store_true",
        help="Print static grid without animation"
    )
    parser.add_argument(
        "--compact", action="store_true",
        help="Use 1-char cells instead of 2-char (narrower output)"
    )
    parser.add_argument(
        "--width", type=int, default=0,
        help="Override terminal width"
    )
    parser.add_argument(
        "--height", type=int, default=0,
        help="Override terminal height"
    )
    args = parser.parse_args()

    if not os.path.exists(args.json_file):
        print(f"Error: {args.json_file} not found", file=sys.stderr)
        sys.exit(1)

    data = load_data(args.json_file)
    if not data.get("shells"):
        print("No shell data found.", file=sys.stderr)
        sys.exit(1)

    ts = shutil.get_terminal_size((160, 80))
    tw = args.width if args.width > 0 else ts.columns
    th = args.height if args.height > 0 else ts.lines

    if args.static:
        run_static(data, tw, th, compact=args.compact)
    else:
        try:
            run_animated(data, tw, th, compact=args.compact)
        except KeyboardInterrupt:
            sys.stdout.write(show_cursor() + rst() + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
