"""Expanded operating-point sampling for the canonical tile validator."""

from __future__ import annotations

import math

from tile import DEFAULT_K, TILE_SIDE, process_tile


ANGLES_DEG = [15, 22, 30, 38, 45]
RADII = [820_000_000, 850_000_000, 880_000_000]


def aligned_tile(r: int, angle_deg: int) -> tuple[int, int]:
    theta = math.radians(angle_deg)
    a = int(round(r * math.cos(theta)))
    b = int(round(r * math.sin(theta)))
    return (a // TILE_SIDE) * TILE_SIDE, (b // TILE_SIDE) * TILE_SIDE


def main():
    for radius in RADII:
        for angle in ANGLES_DEG:
            a_lo, b_lo = aligned_tile(radius, angle)
            result = process_tile(a_lo, b_lo, DEFAULT_K)
            counts = result["tileop_counts"]
            print(
                f"r~{radius} angle={angle} ({a_lo},{b_lo}) "
                f"primes={result['prime_count']} groups={result['group_count']} "
                f"ports={result['ports_after_pruning']} overflow={result['overflow']} "
                f"packed={None if counts is None else counts['o_cnt'] + counts['i_cnt'] + 2 * counts['l_cnt'] + 2 * counts['r_cnt']}"
            )


if __name__ == "__main__":
    main()
