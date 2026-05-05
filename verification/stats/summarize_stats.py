#!/usr/bin/env python3
"""Summarize profile stats_v2 blocks for quick shell inspection."""

from __future__ import annotations

import argparse
import json
from typing import Any
from pathlib import Path


def get_path(data: dict[str, Any], *path: str) -> Any:
    cur: Any = data
    for key in path:
        if not isinstance(cur, dict) or key not in cur:
            return None
        cur = cur[key]
    return cur


def first_value(*values: Any) -> Any:
    for value in values:
        if value is not None:
            return value
    return None


def distribution(stats: dict[str, Any], *names: str) -> Any:
    for name in names:
        for path in (
            (name,),
            ("distributions", name),
            ("histograms", name),
            ("distribution", name),
        ):
            value = get_path(stats, *path)
            if value is not None:
                return value
    return None


def top_n(stats: dict[str, Any], *names: str) -> Any:
    for name in names:
        for path in (
            (name,),
            ("top_n", name),
            ("top_tiles", name),
            ("ranked_tiles", name),
        ):
            value = get_path(stats, *path)
            if value is not None:
                return value
    return None


def short_value(value: Any, *, limit: int = 3) -> str:
    if value is None:
        return "-"
    if isinstance(value, dict):
        if "histogram" in value:
            return short_value(value["histogram"], limit=limit)
        items = list(value.items())[:limit]
        text = ",".join(f"{key}:{val}" for key, val in items)
        return text + (",..." if len(value) > limit else "")
    if isinstance(value, list):
        text = ",".join(short_value(item, limit=2) for item in value[:limit])
        return "[" + text + (",..." if len(value) > limit else "") + "]"
    return str(value)


def component_summary(stats: dict[str, Any]) -> str:
    census = first_value(
        stats.get("component_census"),
        get_path(stats, "components", "census"),
        get_path(stats, "component", "census"),
    )
    if not isinstance(census, dict):
        return "-"
    i_only = first_value(census.get("i_only_components"), census.get("I_only_components"))
    o_only = first_value(census.get("o_only_components"), census.get("O_only_components"))
    both = first_value(
        census.get("i_and_o_components"),
        census.get("I_and_O_components"),
        census.get("io_components"),
    )
    largest = first_value(census.get("largest_component_sizes"), census.get("largest_components"))
    parts = []
    if i_only is not None:
        parts.append(f"I:{i_only}")
    if o_only is not None:
        parts.append(f"O:{o_only}")
    if both is not None:
        parts.append(f"IO:{both}")
    if largest is not None:
        parts.append(f"largest:{short_value(largest)}")
    return " ".join(parts) if parts else short_value(census)


def seconds_summary(data: dict[str, Any]) -> str:
    timings = data.get("timings_seconds") if isinstance(data.get("timings_seconds"), dict) else {}
    total = first_value(timings.get("total"), data.get("total_seconds"))
    cuda = first_value(timings.get("cuda_k1_k5"), data.get("cuda_k1_k5_seconds"))
    compositor = first_value(timings.get("compositor"), data.get("compositor_seconds"))
    parts = []
    if total is not None:
        parts.append(f"total:{total}")
    if cuda is not None:
        parts.append(f"cuda:{cuda}")
    if compositor is not None:
        parts.append(f"comp:{compositor}")
    return " ".join(parts) if parts else "-"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("profiles", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.profiles:
        data = json.loads(path.read_text())
        stats = data.get("stats_v2") if isinstance(data.get("stats_v2"), dict) else data
        if not isinstance(stats, dict):
            stats = {}
        print(
            path.name,
            "verdict=", data.get("verdict"),
            "geo_i_tiles=", first_value(stats.get("geo_i_tiles"), get_path(stats, "geo_I", "tile_population")),
            "geo_o_tiles=", first_value(stats.get("geo_o_tiles"), get_path(stats, "geo_O", "tile_population")),
            "candidate_dist=", short_value(
                distribution(
                    stats,
                    "candidate_count_distribution",
                    "candidate_count",
                    "candidate_counts",
                    "candidates",
                )
            ),
            "prime_dist=", short_value(
                distribution(
                    stats,
                    "gaussian_prime_count_distribution",
                    "prime_count_distribution",
                    "gaussian_prime_count",
                    "gaussian_prime_counts",
                    "prime_counts",
                )
            ),
            "components=", component_summary(stats),
            "high_pressure=", short_value(
                top_n(stats, "high_pressure_tiles", "high_pressure", "high_pressure_top_n")
            ),
            "timings=", seconds_summary(data),
            "sample_manifest=", stats.get("sample_manifest_path"),
            "tile_sample=", stats.get("tile_sample_path"),
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
