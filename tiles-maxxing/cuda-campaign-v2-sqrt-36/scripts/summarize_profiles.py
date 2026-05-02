#!/usr/bin/env python3
"""Summarize campaign_main_cuda --profile JSON files."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Any, Iterable, TextIO


MISSING_PREFIX = "MISSING:"

FIELDS = [
    "case",
    "verdict",
    "early_exit_taken",
    "active_tiles",
    "produced_tiles",
    "ingested_tiles",
    "total_s",
    "grid_s",
    "cuda_k1_k5_s",
    "compositor_s",
    "pipeline_tiles_s",
    "cuda_tileops_s",
    "compositor_tiles_s",
    "app_batches",
    "dispatcher_chunks",
    "dispatcher_slabs",
    "k1_cand_overflow_count",
    "k4_prime_overflow_count",
    "k4_group_overflow_count",
    "k5_port_overflow_count",
    "overflowed_tiles",
    "overflow_rate_percent",
]

OVERFLOW_FIELDS = [
    "k1_cand_overflow_count",
    "k4_prime_overflow_count",
    "k4_group_overflow_count",
    "k5_port_overflow_count",
]


def missing(path: str) -> str:
    return f"{MISSING_PREFIX}{path}"


def get_path(data: dict[str, Any], path: str) -> Any:
    current: Any = data
    for part in path.split("."):
        if not isinstance(current, dict) or part not in current:
            return missing(path)
        current = current[part]
    return current


def is_missing(value: Any) -> bool:
    return isinstance(value, str) and value.startswith(MISSING_PREFIX)


def as_number(value: Any, path: str) -> float | str:
    if is_missing(value):
        return value
    if isinstance(value, bool):
        return missing(path)
    try:
        return float(value)
    except (TypeError, ValueError):
        return missing(path)


def format_scalar(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return format_float(value)
    return str(value)


def format_float(value: float) -> str:
    return f"{value:.6f}".rstrip("0").rstrip(".")


def rate(numerator: Any, numerator_path: str, seconds: Any, seconds_path: str) -> str:
    num = as_number(numerator, numerator_path)
    sec = as_number(seconds, seconds_path)
    if is_missing(num):
        return str(num)
    if is_missing(sec):
        return str(sec)
    if sec == 0:
        return f"NA:zero:{seconds_path}"
    return format_float(num / sec)


def percent(numerator: Any, numerator_path: str, denominator: Any, denominator_path: str) -> str:
    num = as_number(numerator, numerator_path)
    den = as_number(denominator, denominator_path)
    if is_missing(num):
        return str(num)
    if is_missing(den):
        return str(den)
    if den == 0:
        return f"NA:zero:{denominator_path}"
    return format_float((num / den) * 100.0)


def case_name(path: Path) -> str:
    name = path.name
    if name.endswith(".profile.json"):
        return name[: -len(".profile.json")]
    if name.endswith(".json"):
        return name[: -len(".json")]
    return path.stem


def summarize(path: Path) -> dict[str, str]:
    try:
        data = json.loads(path.read_text())
    except OSError as exc:
        raise RuntimeError(f"{path}: could not read file: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"{path}: invalid JSON: {exc}") from exc

    if not isinstance(data, dict):
        raise RuntimeError(f"{path}: profile root must be a JSON object")

    active = get_path(data, "tiles.active")
    produced = get_path(data, "tiles.produced")
    ingested = get_path(data, "tiles.ingested")
    total_s = get_path(data, "timings_seconds.total")
    grid_s = get_path(data, "timings_seconds.grid")
    cuda_s = get_path(data, "timings_seconds.cuda_k1_k5")
    compositor_s = get_path(data, "timings_seconds.compositor")

    overflow_values = {
        name: get_path(data, f"overflow_counters.{name}") for name in OVERFLOW_FIELDS
    }
    overflow_total: int | str = 0
    for name, value in overflow_values.items():
        number = as_number(value, f"overflow_counters.{name}")
        if is_missing(number):
            overflow_total = str(number)
            break
        overflow_total += int(number)

    row: dict[str, str] = {
        "case": case_name(path),
        "verdict": format_scalar(get_path(data, "verdict")),
        "early_exit_taken": format_scalar(get_path(data, "early_exit_taken")),
        "active_tiles": format_scalar(active),
        "produced_tiles": format_scalar(produced),
        "ingested_tiles": format_scalar(ingested),
        "total_s": format_scalar(total_s),
        "grid_s": format_scalar(grid_s),
        "cuda_k1_k5_s": format_scalar(cuda_s),
        "compositor_s": format_scalar(compositor_s),
        "pipeline_tiles_s": rate(produced, "tiles.produced", total_s, "timings_seconds.total"),
        "cuda_tileops_s": rate(produced, "tiles.produced", cuda_s, "timings_seconds.cuda_k1_k5"),
        "compositor_tiles_s": rate(
            ingested, "tiles.ingested", compositor_s, "timings_seconds.compositor"
        ),
        "app_batches": format_scalar(get_path(data, "chunk.app_batches")),
        "dispatcher_chunks": format_scalar(get_path(data, "chunk.dispatcher_chunks")),
        "dispatcher_slabs": format_scalar(get_path(data, "chunk.dispatcher_slabs")),
        "overflowed_tiles": format_scalar(overflow_total),
        "overflow_rate_percent": percent(
            overflow_total, "overflowed_tiles", produced, "tiles.produced"
        ),
    }
    for name, value in overflow_values.items():
        row[name] = format_scalar(value)
    return row


def print_table(rows: list[dict[str, str]], output: TextIO) -> None:
    widths = {
        field: max(len(field), *(len(row[field]) for row in rows)) for field in FIELDS
    }
    output.write("  ".join(field.ljust(widths[field]) for field in FIELDS) + "\n")
    output.write("  ".join("-" * widths[field] for field in FIELDS) + "\n")
    for row in rows:
        output.write("  ".join(row[field].ljust(widths[field]) for field in FIELDS) + "\n")


def print_delimited(rows: Iterable[dict[str, str]], output: TextIO, delimiter: str) -> None:
    writer = csv.DictWriter(output, fieldnames=FIELDS, delimiter=delimiter, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize campaign_main_cuda --profile JSON files."
    )
    parser.add_argument(
        "--format",
        choices=("table", "csv", "tsv"),
        default="table",
        help="output format (default: table)",
    )
    parser.add_argument("profiles", nargs="+", type=Path, help="profile JSON file(s)")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        rows = [summarize(path) for path in args.profiles]
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if args.format == "csv":
        print_delimited(rows, sys.stdout, ",")
    elif args.format == "tsv":
        print_delimited(rows, sys.stdout, "\t")
    else:
        print_table(rows, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
