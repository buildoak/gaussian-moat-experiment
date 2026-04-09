#!/usr/bin/env python3
"""Measure TileOp v2 packed-budget usage on census samples."""

from __future__ import annotations

import csv
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(__file__))

from analysis import overflow_reason, packed_budget_counts
from tile import process_tile


CENSUS_CSV = os.path.join(
    os.path.dirname(__file__),
    "..",
    "tile-cpp",
    "census_output",
    "census_R860000000_T3125.csv",
)
OUTPUT_CSV = os.path.join(
    os.path.dirname(__file__),
    "..",
    "tile-cpp",
    "census_output",
    "uncapped_R860000000.csv",
)


def measure_tile(a_lo: int, b_lo: int) -> dict[str, int | bool | str | None]:
    result = process_tile(a_lo, b_lo)
    budget = packed_budget_counts(result["ports"])
    reason = overflow_reason(result["ports"], result["group_count"])
    return {
        "a_lo": a_lo,
        "b_lo": b_lo,
        "face_I_ports": len(result["ports"]["I"]),
        "face_O_ports": len(result["ports"]["O"]),
        "face_L_ports": len(result["ports"]["L"]),
        "face_R_ports": len(result["ports"]["R"]),
        "max_face_ports": max(len(result["ports"][face]) for face in ("I", "O", "L", "R")),
        "total_ports": result["ports_after_pruning"],
        "group_count": result["group_count"],
        "packed_bytes_used": budget["packed_bytes_used"],
        "packed_bytes_slack": budget["packed_bytes_slack"],
        "would_fit_v2": reason is None,
        "overflow_reason": reason,
    }


def read_sample(limit: int = 200) -> list[tuple[int, int]]:
    coords: list[tuple[int, int]] = []
    with open(CENSUS_CSV, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            coords.append((int(row["a_lo"]), int(row["b_lo"])))
            if len(coords) >= limit:
                break
    return coords


def main() -> int:
    rows = [measure_tile(a_lo, b_lo) for a_lo, b_lo in read_sample()]
    if not rows:
        print("no rows sampled")
        return 1

    with open(OUTPUT_CSV, "w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    packed = [int(row["packed_bytes_used"]) for row in rows]
    over_16_but_fit = sum(
        1 for row in rows if int(row["max_face_ports"]) > 16 and bool(row["would_fit_v2"])
    )
    print(f"tiles={len(rows)} output={OUTPUT_CSV}")
    print(f"packed_bytes p50={statistics.median(packed)} p95={statistics.quantiles(packed, n=20)[18]}")
    print(f"tiles with max_face_ports>16 but would_fit_v2=true: {over_16_but_fit}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
