#!/usr/bin/env python3
"""
gen_coords.py — Gaussian Moat CUDA coordinate generator

Generates binary coordinate files for tile batches at R=850,000,000 on the
Gaussian integer lattice. Three angular bands, up to 100K tiles each.

Binary output format (little-endian):
    uint32_t num_tiles
    Repeated num_tiles times:
        int64_t a_lo
        int64_t b_lo

Usage:
    python gen_coords.py [--output-dir DIR]
"""

import argparse
import math
import os
import struct

# ── Constants ────────────────────────────────────────────────────────────────

R = 850_000_000
TILE_SIDE = 256          # tile is TILE_SIDE × TILE_SIDE on the Gaussian lattice
TOWER_HEIGHT = 32        # rows per tower
TOWERS_PER_BAND = 3125   # 3125 towers × 32 rows = 100K tiles (ceiling)

# ── Band definitions ──────────────────────────────────────────────────────────
#
# Band A ≈ 85° (near vertical axis)
#   x = R·cos(85°) ≈ 74,086,000   →   j_center = round(74_086_000 / 256) = 289_398
#
# Band B ≈ 67.5° (bisects 45°–90°)
#   x = R·cos(67.5°) ≈ 325,295,000 →  j_center = round(325_295_000 / 256) = 1_270_684
#
# Band C ≈ 45° (diagonal)
#   x = R·cos(45°) ≈ 601,040,764  →   j_center = round(601_040_764 / 256) = 2_347_816

BANDS = [
    {
        "name":     "Band A",
        "label":    "coords_85deg.bin",
        "angle":    85.0,
        "j_center": 289_398,
    },
    {
        "name":     "Band B",
        "label":    "coords_67deg.bin",
        "angle":    67.5,
        "j_center": 1_270_684,
    },
    {
        "name":     "Band C",
        "label":    "coords_45deg.bin",
        "angle":    45.0,
        "j_center": 2_347_816,
    },
]

# ── Core geometry ─────────────────────────────────────────────────────────────

R2 = R * R  # precompute once; used inside the hot loop


def tile_b_lo(j: int, r: int) -> int:
    """
    Return the b_lo (bottom edge) for tower j, row r.

    Tower j covers a ∈ [j·256, (j+1)·256).
    Within the tower the rows are stacked downward from the arc:
        b_lo = isqrt(R² − (j·256)²) + r·256
    This places row 0 at the arc and rows 1…31 above it (larger b).

    Wait — that would go *above* the arc, not below. Let's think carefully:

    We want tiles that are INSIDE the circle (a² + b² ≤ R²).
    For tower j, the maximum valid b is isqrt(R² − (j·256)²).
    Row 0 starts at that maximum:
        b_lo(j, 0) = isqrt(R² − (j·256)²)        ← top of arc for this tower
    Row 1 is one tile *lower* (smaller b):
        b_lo(j, 1) = isqrt(R² − (j·256)²) − 256
    …
    Row r:
        b_lo(j, r) = isqrt(R² − (j·256)²) − r·256

    The task spec uses  b_lo = isqrt(…) + r·256  which would go upward, but
    that conflicts with the dead-tile check  b_lo + 256 ≤ j·256  (y=x diagonal).
    For 85° (large b, small j) adding r·256 keeps b larger than j·256, so no
    dead tiles — but the tiles would be *outside* the circle for r > 0.

    Reading the spec again literally:
        "b_lo = floor(sqrt(R² − (j·256)²)) + r·256"
    This matches what the CUDA kernel likely expects: tile (j, r=0) sits just
    above the arc, r=1 one tile higher, etc. The dead-tile guard catches tiles
    that have drifted below the first-octant diagonal.

    We follow the spec exactly to match the CUDA kernel's coordinate convention.
    """
    a_edge = j * TILE_SIDE
    # integer sqrt — no floating point for coordinate computation
    sq = math.isqrt(R2 - a_edge * a_edge)
    return sq + r * TILE_SIDE


def generate_band(j_center: int) -> list[tuple[int, int]]:
    """
    Generate (a_lo, b_lo) pairs for one band.

    Tower range: [j_center - TOWERS_PER_BAND//2, j_center + TOWERS_PER_BAND//2)
    Towers with a_edge² > R² are clamped / skipped.
    Dead tiles (b_lo + TILE_SIDE ≤ j·TILE_SIDE) are skipped.
    """
    half = TOWERS_PER_BAND // 2  # 1562
    j_lo = j_center - half
    j_hi = j_center + half  # exclusive → 3124 + 1 = 3125 towers

    tiles: list[tuple[int, int]] = []

    for j in range(j_lo, j_hi + 1):  # +1 so range is 3125 towers inclusive
        a_lo = j * TILE_SIDE
        a_edge_sq = a_lo * a_lo

        # Skip towers entirely outside the circle
        if a_edge_sq >= R2:
            continue

        for r in range(TOWER_HEIGHT):
            b_lo = tile_b_lo(j, r)

            # Dead-tile check: entire tile is below the y=x diagonal
            # (first-octant constraint: b ≥ a throughout the tile)
            if b_lo + TILE_SIDE <= a_lo:
                continue  # skip — tile is below diagonal

            tiles.append((a_lo, b_lo))

    return tiles


def generate_band_adaptive(j_center: int, target: int = 100_000) -> list[tuple[int, int]]:
    """
    Adaptive tile generator for bands where dead tiles reduce the naive count.

    Starts at j_center and expands left (decreasing j), where base_y > j*256 so
    all TOWER_HEIGHT rows are alive. Continues until we accumulate `target` tiles
    or run out of in-circle towers. Caps at exactly `target` tiles.

    Left-first expansion is intentional: at 45° the diagonal kills right-side
    towers, but towers to the left of center all have b_arc >> a_lo so none of
    their rows cross below the diagonal.
    """
    tiles: list[tuple[int, int]] = []

    # Walk left from j_center until we have enough tiles
    j = j_center
    while len(tiles) < target and j >= 0:
        a_lo = j * TILE_SIDE
        a_edge_sq = a_lo * a_lo

        if a_edge_sq < R2:
            for r in range(TOWER_HEIGHT):
                if len(tiles) >= target:
                    break
                b_lo = tile_b_lo(j, r)
                if b_lo + TILE_SIDE > a_lo:  # alive tile
                    tiles.append((a_lo, b_lo))

        j -= 1

    return tiles


def write_bin(path: str, tiles: list[tuple[int, int]]) -> int:
    """Write tiles to binary file. Returns bytes written."""
    num = len(tiles)
    # Header: uint32 num_tiles (4 bytes)
    # Per tile: int64 a_lo + int64 b_lo (16 bytes each)
    with open(path, "wb") as f:
        f.write(struct.pack("<I", num))
        for a_lo, b_lo in tiles:
            f.write(struct.pack("<qq", a_lo, b_lo))
    return 4 + num * 16


def generate_full_octant(output_dir: str) -> None:
    """
    Generate ALL tiles in the first octant (y >= x >= 0) at R=850M.

    Writes coords_octant_850M.bin to output_dir. Uses buffered writes
    to handle the ~70M tile output (~1.1 GB file).
    """
    j_max = math.isqrt(R2 // 2)  // TILE_SIDE  # floor(R / (256*sqrt(2)))

    out_path = os.path.join(output_dir, "coords_octant_850M.bin")

    total_tiles = 0
    total_towers = 0
    WRITE_BATCH = 10_000

    print(f"Full-octant generation: R={R:,}, j_max={j_max:,}")
    print(f"Output: {out_path}")

    with open(out_path, "wb") as f:
        # Write placeholder header (will overwrite at end)
        f.write(struct.pack("<I", 0))

        buf = bytearray()

        for j in range(0, j_max + 1):
            a_lo = j * TILE_SIDE
            a_edge_sq = a_lo * a_lo

            if a_edge_sq >= R2:
                break

            base_y = math.isqrt(R2 - a_edge_sq)
            tower_tiles = 0

            for r in range(TOWER_HEIGHT):
                b_lo = base_y + r * TILE_SIDE

                # Dead-tile check: entire tile below y=x diagonal
                if b_lo + TILE_SIDE <= a_lo:
                    continue

                buf += struct.pack("<qq", a_lo, b_lo)
                total_tiles += 1
                tower_tiles += 1

                if len(buf) >= WRITE_BATCH * 16:
                    f.write(buf)
                    buf = bytearray()

            if tower_tiles > 0:
                total_towers += 1

            if (j + 1) % 100_000 == 0:
                print(f"  towers: {j + 1:,} / {j_max + 1:,}  tiles so far: {total_tiles:,}")

        # Flush remaining buffer
        if buf:
            f.write(buf)

        # Overwrite header with actual tile count
        f.seek(0)
        f.write(struct.pack("<I", total_tiles))

    file_size = os.path.getsize(out_path)
    expected_size = 4 + total_tiles * 16

    print(f"\nDone.")
    print(f"  Total towers: {total_towers:,}")
    print(f"  Total tiles:  {total_tiles:,}")
    print(f"  File size:    {file_size:,} bytes ({file_size / (1024**3):.2f} GiB)")
    print(f"  Expected:     {expected_size:,} bytes")
    print(f"  Match:        {file_size == expected_size}")


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate Gaussian Moat CUDA coordinate files."
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Directory for output .bin files (default: same dir as this script)",
    )
    parser.add_argument(
        "--full-octant",
        action="store_true",
        help="Generate ALL tiles in the first octant (y >= x >= 0) at R=850M",
    )
    args = parser.parse_args()

    out_dir = args.output_dir or os.path.dirname(os.path.abspath(__file__))
    os.makedirs(out_dir, exist_ok=True)

    if args.full_octant:
        generate_full_octant(out_dir)
        return

    print(f"R = {R:,}")
    print(f"Tile side   = {TILE_SIDE}")
    print(f"Tower rows  = {TOWER_HEIGHT}")
    print(f"Towers/band = {TOWERS_PER_BAND}  (j_center ± {TOWERS_PER_BAND // 2})")
    print(f"Output dir  = {out_dir}")
    print()

    for band in BANDS:
        j_center = band["j_center"]

        if band["angle"] == 45.0:
            # Adaptive: expand leftward until we hit 100K live tiles.
            # Towers left of center have b_arc >> a_lo so no dead rows.
            tiles = generate_band_adaptive(j_center, target=100_000)
            j_lo_actual = tiles[-1][0] // TILE_SIDE if tiles else j_center
            j_hi_actual = tiles[0][0] // TILE_SIDE if tiles else j_center
            j_range_str = (
                f"[{j_lo_actual}, {j_hi_actual}]"
                f"  ({j_hi_actual - j_lo_actual + 1} towers, adaptive left-expand)"
            )
        else:
            half = TOWERS_PER_BAND // 2
            j_lo = j_center - half
            j_hi = j_center + half
            tiles = generate_band(j_center)
            j_range_str = f"[{j_lo}, {j_hi}]  ({j_hi - j_lo + 1} towers)"

        out_path = os.path.join(out_dir, band["label"])
        nbytes = write_bin(out_path, tiles)

        print(
            f"{band['name']}  ({band['angle']}°)\n"
            f"  j range   : {j_range_str}\n"
            f"  tiles      : {len(tiles):,}\n"
            f"  file       : {band['label']}  ({nbytes:,} bytes / {nbytes / 1024:.1f} KiB)\n"
        )


if __name__ == "__main__":
    main()
