#!/usr/bin/env python3
"""Collect lightweight TileOp distribution stats from a snapshot."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import struct
from collections import Counter


HEADER_SIZE = 120
TILEOP_SIZE = 256
OVERFLOW_BIT = 0x01


def pct(values: list[int], p: float) -> int:
    if not values:
        return 0
    idx = math.ceil((p / 100.0) * len(values)) - 1
    return values[max(0, min(idx, len(values) - 1))]


def summarize(values: list[int]) -> dict:
    values = sorted(values)
    hist = Counter(values)
    return {
        "min": values[0] if values else 0,
        "p50": pct(values, 50),
        "p95": pct(values, 95),
        "p99": pct(values, 99),
        "max": values[-1] if values else 0,
        "histogram": {str(k): hist[k] for k in sorted(hist)},
    }


def highest_flag_label(flags: bytes) -> int:
    out = 0
    for byte_idx, value in enumerate(flags):
        for bit in range(8):
            if value & (1 << bit):
                out = max(out, byte_idx * 8 + bit + 1)
    return out


def collect(snapshot: pathlib.Path, manifest: pathlib.Path) -> dict:
    doc = json.loads(manifest.read_text())
    blob = snapshot.read_bytes()
    if len(blob) < HEADER_SIZE:
        raise RuntimeError("short snapshot")
    tile_count = struct.unpack_from("<Q", blob, 104)[0]
    bytes_per_tile = struct.unpack_from("<I", blob, 112)[0]
    if bytes_per_tile != TILEOP_SIZE:
        raise RuntimeError(f"unexpected bytes_per_tile={bytes_per_tile}")
    expected = HEADER_SIZE + tile_count * TILEOP_SIZE
    if len(blob) != expected:
        raise RuntimeError(f"snapshot size {len(blob)} != expected {expected}")

    port_counts: list[int] = []
    label_counts: list[int] = []
    overflow = 0
    for idx in range(tile_count):
        off = HEADER_SIZE + idx * TILEOP_SIZE
        tile = blob[off : off + TILEOP_SIZE]
        ns = list(tile[0:4])
        total_ports = sum(ns)
        port_counts.append(total_ports)
        labels = list(tile[4 : 4 + total_ports])
        max_label = max(labels, default=0)
        max_label = max(max_label, highest_flag_label(tile[196:212]))
        max_label = max(max_label, highest_flag_label(tile[212:228]))
        label_counts.append(max_label)
        if tile[228] & OVERFLOW_BIT:
            overflow += 1

    return {
        "snapshot": str(snapshot),
        "manifest": str(manifest),
        "tile_count": tile_count,
        "k_sq": doc.get("k_sq"),
        "r_inner": doc.get("r_inner"),
        "r_outer": doc.get("r_outer"),
        "port_count": summarize(port_counts),
        "label_count": summarize(label_counts),
        "overflow_counter": overflow,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshot", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--wall-time-1", type=float, required=True)
    parser.add_argument("--wall-time-n", type=float, required=True)
    parser.add_argument("--thread-count", type=int, required=True)
    args = parser.parse_args()

    stats = collect(args.snapshot, args.manifest)
    tiles = int(stats["tile_count"])
    stats["wall_time_sec"] = {
        "1-thread": args.wall_time_1,
        f"{args.thread_count}-thread": args.wall_time_n,
    }
    stats["tiles_per_sec"] = {
        "1-thread": tiles / args.wall_time_1 if args.wall_time_1 > 0 else 0.0,
        f"{args.thread_count}-thread": tiles / args.wall_time_n
        if args.wall_time_n > 0
        else 0.0,
    }
    stats["omp_speedup"] = (
        args.wall_time_1 / args.wall_time_n if args.wall_time_n > 0 else 0.0
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(stats, indent=2, sort_keys=True) + "\n")
    print(f"stats wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
