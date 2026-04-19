#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# ///
"""Verify snapped-grid octant coverage for the campaign tower enumeration."""

from __future__ import annotations

import argparse
import math
import sys
import time
from dataclasses import dataclass


@dataclass(frozen=True)
class Args:
    r_inner: int
    r_outer: int
    k_sq: int
    offset_x: int
    offset_y: int
    tile_side: int
    theta_samples: int
    max_failures: int


@dataclass(frozen=True)
class Failure:
    kind: str
    detail: str


def ceil_div(a: int, b: int) -> int:
    return -((-a) // b)


def ceil_sqrt(n: int) -> int:
    if n <= 0:
        return 0
    root = math.isqrt(n)
    return root if root * root == n else root + 1


def row_low_for_closed_interval(y: int, offset_y: int, side: int) -> int:
    return ceil_div(y - offset_y, side) - 1


def row_high_for_closed_interval(y: int, offset_y: int, side: int) -> int:
    return (y - offset_y) // side


def point_cell(v: float, offset: int, side: int) -> int:
    return math.floor((v - offset) / side)


def enumerate_towers(args: Args) -> dict[int, tuple[int, int]]:
    """Enumerate blueprint towers for 0 <= a <= b.

    A tile is active when its closed proper region contains an integer lattice
    point in the collar-expanded annular octant. The collar is C=floor(sqrt(K)),
    matching the dispatcher-requested theta probes at R_inner-C and R_outer+C.
    """

    side = args.tile_side
    collar = math.isqrt(args.k_sq)
    coverage_inner = max(0, args.r_inner - collar)
    coverage_outer = args.r_outer + collar
    rin2 = coverage_inner * coverage_inner
    rout2 = coverage_outer * coverage_outer

    max_x = math.isqrt(rout2 // 2)
    i_min = max(0, row_low_for_closed_interval(0, args.offset_x, side))
    i_max = max(0, row_high_for_closed_interval(max_x, args.offset_x, side))
    towers: dict[int, tuple[int, int]] = {}

    for i in range(i_min, i_max + 1):
        x_lo = args.offset_x + i * side
        x_hi = x_lo + side
        scan_lo = max(0, x_lo)
        scan_hi = min(max_x, x_hi)
        if scan_lo > scan_hi:
            continue

        min_y: int | None = None
        max_y: int | None = None
        for x in range(scan_lo, scan_hi + 1):
            x2 = x * x
            upper_disk = rout2 - x2
            if upper_disk < 0:
                continue

            y_lo = max(x, ceil_sqrt(rin2 - x2))
            y_hi = math.isqrt(upper_disk)
            if y_lo > y_hi:
                continue

            if min_y is None or y_lo < min_y:
                min_y = y_lo
            if max_y is None or y_hi > max_y:
                max_y = y_hi

        if min_y is None or max_y is None:
            continue

        j_low = max(0, row_low_for_closed_interval(min_y, args.offset_y, side))
        j_high = row_high_for_closed_interval(max_y, args.offset_y, side)
        if j_low <= j_high:
            towers[i] = (j_low, j_high)

    return towers


def is_active(towers: dict[int, tuple[int, int]], cell: tuple[int, int]) -> bool:
    tower = towers.get(cell[0])
    return tower is not None and tower[0] <= cell[1] <= tower[1]


def verify_thickness(args: Args) -> list[Failure]:
    required = args.tile_side * math.sqrt(2.0) + 2.0 * math.sqrt(args.k_sq)
    actual = args.r_outer - args.r_inner
    if actual <= required:
        return [
            Failure(
                "thickness",
                f"R_outer - R_inner = {actual} <= {required:.6f}",
            )
        ]
    return []


def verify_invariants(towers: dict[int, tuple[int, int]]) -> list[Failure]:
    failures: list[Failure] = []
    columns = sorted(towers)

    for i in columns:
        j_low, j_high = towers[i]
        if j_low > j_high:
            failures.append(Failure("I1", f"column {i}: empty interval [{j_low}, {j_high}]"))

    for left, right in zip(columns, columns[1:]):
        if right != left + 1:
            continue
        lo_a, hi_a = towers[left]
        lo_b, hi_b = towers[right]
        if abs(lo_b - lo_a) > 1 or abs(hi_b - hi_a) > 1:
            failures.append(
                Failure(
                    "I2",
                    (
                        f"columns {left}->{right}: "
                        f"[{lo_a}, {hi_a}] -> [{lo_b}, {hi_b}]"
                    ),
                )
            )

    for left, right in zip(columns, columns[1:]):
        if right != left + 1:
            continue
        lo_a, hi_a = towers[left]
        lo_b, hi_b = towers[right]
        for j in range(lo_a, hi_a + 1):
            if lo_b <= j + 1 <= hi_b:
                if not ((lo_b <= j <= hi_b) or (lo_a <= j + 1 <= hi_a)):
                    failures.append(
                        Failure("I4", f"up-right pair ({left},{j}) -> ({right},{j + 1})")
                    )
            if lo_b <= j - 1 <= hi_b:
                if not ((lo_b <= j <= hi_b) or (lo_a <= j - 1 <= hi_a)):
                    failures.append(
                        Failure("I4", f"down-right pair ({left},{j}) -> ({right},{j - 1})")
                    )

    return failures


def verify_theta_coverage(args: Args, towers: dict[int, tuple[int, int]]) -> list[Failure]:
    failures: list[Failure] = []
    collar = math.isqrt(args.k_sq)
    radii = (
        args.r_inner - collar,
        args.r_inner,
        args.r_inner + 1,
        args.r_outer - 1,
        args.r_outer,
        args.r_outer + collar,
    )
    samples = max(1, args.theta_samples)

    for sample in range(samples + 1):
        theta = (math.pi / 4.0) * sample / samples
        cos_t = math.cos(theta)
        sin_t = math.sin(theta)
        for radius in radii:
            if radius < 0:
                continue
            a = radius * cos_t
            b = radius * sin_t
            folded_a = min(a, b)
            folded_b = max(a, b)
            cell = (
                point_cell(folded_a, args.offset_x, args.tile_side),
                point_cell(folded_b, args.offset_y, args.tile_side),
            )
            if not is_active(towers, cell):
                failures.append(
                    Failure(
                        "coverage",
                        (
                            f"theta_sample={sample}/{samples} theta={theta:.17g} "
                            f"R={radius} point=({a:.6f},{b:.6f}) "
                            f"folded=({folded_a:.6f},{folded_b:.6f}) cell={cell}"
                        ),
                    )
                )
                if len(failures) >= args.max_failures:
                    return failures

    return failures


def parse_args() -> Args:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--r-inner", type=int, required=True)
    parser.add_argument("--r-outer", type=int, required=True)
    parser.add_argument("--k-sq", type=int, required=True)
    parser.add_argument("--offset-x", type=int, default=0)
    parser.add_argument("--offset-y", type=int, default=0)
    parser.add_argument("--tile-side", type=int, default=256)
    parser.add_argument("--theta-samples", type=int, default=1_000_000)
    parser.add_argument("--max-failures", type=int, default=20)
    ns = parser.parse_args()
    return Args(
        r_inner=ns.r_inner,
        r_outer=ns.r_outer,
        k_sq=ns.k_sq,
        offset_x=ns.offset_x,
        offset_y=ns.offset_y,
        tile_side=ns.tile_side,
        theta_samples=ns.theta_samples,
        max_failures=ns.max_failures,
    )


def main() -> int:
    args = parse_args()
    started = time.monotonic()

    failures = verify_thickness(args)
    towers = enumerate_towers(args)
    failures.extend(verify_invariants(towers))
    failures.extend(verify_theta_coverage(args, towers))

    elapsed = time.monotonic() - started
    tile_count = sum(j_high - j_low + 1 for j_low, j_high in towers.values())
    print(
        "coverage_verifier "
        f"r_inner={args.r_inner} r_outer={args.r_outer} k_sq={args.k_sq} "
        f"side={args.tile_side} offset=({args.offset_x},{args.offset_y})"
    )
    collar = math.isqrt(args.k_sq)
    print(
        f"coverage_envelope=[{max(0, args.r_inner - collar)}, {args.r_outer + collar}] "
        f"collar={collar}"
    )
    print(f"towers={len(towers)} active_tiles={tile_count} elapsed_sec={elapsed:.3f}")

    if failures:
        print(f"status=FAIL failures={len(failures)}")
        for failure in failures[: args.max_failures]:
            print(f"{failure.kind}: {failure.detail}")
        return 1

    print("status=PASS failures=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
