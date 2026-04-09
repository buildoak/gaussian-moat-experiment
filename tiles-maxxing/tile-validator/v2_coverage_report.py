#!/usr/bin/env python3
"""Aggregate TileOp v2 coverage statistics from measure_v2_envelope output."""

from __future__ import annotations

import csv
import os
import statistics
from collections import Counter, defaultdict


INPUT_CSV = os.path.join(
    os.path.dirname(__file__),
    "..",
    "tile-cpp",
    "census_output",
    "measure_v2_envelope.csv",
)


def main() -> int:
    with open(INPUT_CSV, newline="") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        print("no rows found")
        return 1

    by_radius: dict[str, list[dict]] = defaultdict(list)
    by_angle: dict[str, list[dict]] = defaultdict(list)
    packed = []
    face_counts = []
    reasons: Counter[str] = Counter()
    for row in rows:
        by_radius[row["radius"]].append(row)
        by_angle[row["angle_deg"]].append(row)
        packed.append(int(row["packed_bytes_used"]))
        face_counts.extend(
            [int(row["face_I_ports"]), int(row["face_O_ports"]), int(row["face_L_ports"]), int(row["face_R_ports"])]
        )
        if row["overflow_reason"]:
            reasons[row["overflow_reason"]] += 1

    for radius, items in sorted(by_radius.items()):
        fit_rate = sum(row["overflow"] == "False" for row in items) / len(items)
        print(f"radius {radius}: fit_rate={fit_rate:.3f}")
    for angle, items in sorted(by_angle.items()):
        fit_rate = sum(row["overflow"] == "False" for row in items) / len(items)
        print(f"angle {angle}: fit_rate={fit_rate:.3f}")
    print(f"packed p50={statistics.median(packed)} p95={statistics.quantiles(packed, n=20)[18]} p99=max={max(packed)}")
    print(f"face_count p95={statistics.quantiles(face_counts, n=20)[18]} p99=max={max(face_counts)}")
    print(f"overflow_reasons={dict(reasons)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
