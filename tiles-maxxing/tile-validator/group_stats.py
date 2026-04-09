#!/usr/bin/env python3
"""Group taxonomy and TileOp v2 envelope stats for census samples."""

from __future__ import annotations

import csv
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

from analysis import category_histogram, classify_surviving_groups, overflow_reason, packed_budget_counts
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
    "group_stats_R860000000.csv",
)


def analyze_tile_groups(a_lo: int, b_lo: int) -> tuple[dict, list[dict]]:
    result = process_tile(a_lo, b_lo)
    classifications = classify_surviving_groups(result["ports"])
    budget = packed_budget_counts(result["ports"])
    summary = {
        "a_lo": a_lo,
        "b_lo": b_lo,
        "prime_count": result["prime_count"],
        "group_count": result["group_count"],
        "ports_before_pruning": result["ports_before_pruning"],
        "ports_after_pruning": result["ports_after_pruning"],
        "face_I_ports": len(result["ports"]["I"]),
        "face_O_ports": len(result["ports"]["O"]),
        "face_L_ports": len(result["ports"]["L"]),
        "face_R_ports": len(result["ports"]["R"]),
        "packed_bytes_used": budget["packed_bytes_used"],
        "packed_bytes_slack": budget["packed_bytes_slack"],
        "overflow": result["overflow"],
        "overflow_reason": overflow_reason(result["ports"], result["group_count"]),
    }
    return summary, classifications


def main() -> int:
    summaries: list[dict] = []
    group_rows: list[dict] = []
    with open(CENSUS_CSV, newline="") as fh:
        reader = csv.DictReader(fh)
        for index, row in enumerate(reader):
            if index >= 200:
                break
            summary, classifications = analyze_tile_groups(int(row["a_lo"]), int(row["b_lo"]))
            summaries.append(summary)
            for group in classifications:
                group_rows.append(
                    {
                        "a_lo": summary["a_lo"],
                        "b_lo": summary["b_lo"],
                        "group_id": group["group_id"],
                        "category": group["category"],
                        "faces": ",".join(group["faces"]),
                        "total_ports": group["total_ports"],
                    }
                )

    with open(OUTPUT_CSV, "w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=list(group_rows[0].keys()) if group_rows else ["a_lo"])
        writer.writeheader()
        writer.writerows(group_rows)

    merged_hist: dict[str, int] = {}
    for summary in summaries[:5]:
        _, classifications = analyze_tile_groups(summary["a_lo"], summary["b_lo"])
        for key, value in category_histogram(classifications).items():
            merged_hist[key] = merged_hist.get(key, 0) + value
    print(f"tiles={len(summaries)} groups_csv={OUTPUT_CSV}")
    print(f"classification_sample={merged_hist}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
