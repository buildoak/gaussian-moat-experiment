#!/usr/bin/env python3
"""Multi-radius TileOp v2 envelope census."""

from __future__ import annotations

import csv
import math
import os

from analysis import overflow_reason, packed_budget_counts
from tile import TILE_SIDE, process_tile


RADII = [820_000_000, 860_000_000, 920_000_000]
ANGLES_DEG = [30, 45]
OUTPUT_CSV = os.path.join(
    os.path.dirname(__file__),
    "..",
    "tile-cpp",
    "census_output",
    "measure_v2_envelope.csv",
)


def aligned_tile(radius: int, angle_deg: int) -> tuple[int, int]:
    theta = math.radians(angle_deg)
    a = int(round(radius * math.cos(theta)))
    b = int(round(radius * math.sin(theta)))
    return (a // TILE_SIDE) * TILE_SIDE, (b // TILE_SIDE) * TILE_SIDE


def measure(radius: int, angle_deg: int) -> dict[str, int | bool | str | None]:
    a_lo, b_lo = aligned_tile(radius, angle_deg)
    result = process_tile(a_lo, b_lo)
    budget = packed_budget_counts(result["ports"])
    return {
        "a_lo": a_lo,
        "b_lo": b_lo,
        "radius": int(round((a_lo * a_lo + b_lo * b_lo) ** 0.5)),
        "angle_deg": angle_deg,
        "face_I_ports": len(result["ports"]["I"]),
        "face_O_ports": len(result["ports"]["O"]),
        "face_L_ports": len(result["ports"]["L"]),
        "face_R_ports": len(result["ports"]["R"]),
        "total_groups_after_pruning": result["group_count"],
        "packed_bytes_used": budget["packed_bytes_used"],
        "packed_bytes_slack": budget["packed_bytes_slack"],
        "overflow": result["overflow"],
        "overflow_reason": overflow_reason(result["ports"], result["group_count"]),
        "max_face_ports": max(len(result["ports"][face]) for face in ("I", "O", "L", "R")),
    }


def main() -> int:
    rows = [measure(radius, angle) for radius in RADII for angle in ANGLES_DEG]
    with open(OUTPUT_CSV, "w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {len(rows)} rows to {OUTPUT_CSV}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
