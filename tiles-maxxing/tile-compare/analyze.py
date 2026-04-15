"""Statistical analysis of Gaussian Moat tile dumps.

Usage:
    python analyze.py <dump.bin> [--band-name NAME]

Outputs:
    - Human-readable text report to stdout
    - JSON summary to <dump>.stats.json

TileOp layout (v2):
    bytes[0]         = off_I  (3 + o_count)
    bytes[1]         = off_L
    bytes[2]         = off_R
    bytes[3..off_I)  = O-face group IDs (1 byte each, value 1-255)
    bytes[off_I..off_L) = I-face group IDs (1 byte each)
    bytes[off_L..off_R) = L-face group bytes (MSB = h1 bit8, low7 = group ID 1-127)
    bytes[off_R..h_start) = R-face group bytes (same encoding, trailing zeros trimmed)
    bytes[h_start..h_start+l_cnt) = L h1 low bytes
    bytes[h_start+l_cnt..h_start+l_cnt+r_live) = R h1 low bytes
    Dead:     off_I==off_L==off_R==3, bytes[3]==0
    Overflow: bytes[0]==0xFF (all 128 bytes are 0xFF)
    Total size: 128 bytes, payload budget: 125 bytes
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from collections import Counter
from pathlib import Path

# ─── TileOp constants ────────────────────────────────────────────────────────

TILEOP_SIZE = 128
HEADER_BYTES = 3
PAYLOAD_BUDGET = 125
OVERFLOW_BYTE = 0xFF
EMPTY_OFFSET = 3
FACES = ("O", "I", "L", "R")

# Packed-byte cost per face port:
#   O, I  → 1 byte each
#   L, R  → 2 bytes each (group byte + h1 byte)
FACE_COST = {"O": 1, "I": 1, "L": 2, "R": 2}


# ─── TileOp parsing ──────────────────────────────────────────────────────────

def _is_overflow(tileop: bytes) -> bool:
    return tileop == bytes([OVERFLOW_BYTE] * TILEOP_SIZE)


def _is_dead(tileop: bytes) -> bool:
    return (
        not _is_overflow(tileop)
        and tileop[0] == EMPTY_OFFSET
        and tileop[1] == EMPTY_OFFSET
        and tileop[2] == EMPTY_OFFSET
        and tileop[3] == 0
    )


def _parse_tileop(tileop: bytes) -> dict | None:
    """Parse a normal TileOp. Returns None for overflow/dead/invalid."""
    if _is_overflow(tileop) or _is_dead(tileop):
        return None

    off_I = tileop[0]
    off_L = tileop[1]
    off_R = tileop[2]

    if not (HEADER_BYTES <= off_I <= off_L <= off_R <= 127):
        return None

    o_cnt = off_I - HEADER_BYTES
    i_cnt = off_L - off_I
    l_cnt = off_R - off_L
    residual = PAYLOAD_BUDGET - o_cnt - i_cnt - 2 * l_cnt
    if residual < 0:
        return None
    r_cnt = residual >> 1
    h_start = off_R + r_cnt

    r_raw = list(tileop[off_R : off_R + r_cnt])
    live_r = len(r_raw)
    while live_r > 0 and r_raw[live_r - 1] == 0:
        live_r -= 1

    # Decode group IDs (strip h1 MSB for L/R)
    def gid(b: int, face: str) -> int:
        return b & 0x7F if face in ("L", "R") else b

    groups: dict[str, list[int]] = {
        "O": [gid(b, "O") for b in tileop[HEADER_BYTES:off_I]],
        "I": [gid(b, "I") for b in tileop[off_I:off_L]],
        "L": [gid(b, "L") for b in tileop[off_L:off_R]],
        "R": [gid(b, "R") for b in r_raw[:live_r]],
    }

    payload_used = o_cnt + i_cnt + 2 * l_cnt + 2 * live_r
    slack = PAYLOAD_BUDGET - payload_used

    return {
        "off_I": off_I,
        "off_L": off_L,
        "off_R": off_R,
        "o_cnt": o_cnt,
        "i_cnt": i_cnt,
        "l_cnt": l_cnt,
        "r_cnt": live_r,        # trimmed
        "groups": groups,
        "payload_used": payload_used,
        "slack": slack,
    }


# ─── distribution helpers ────────────────────────────────────────────────────

def _percentile(sorted_data: list[float | int], pct: float) -> float:
    """Compute percentile on a pre-sorted list (0-100 scale)."""
    if not sorted_data:
        return float("nan")
    n = len(sorted_data)
    idx = pct / 100.0 * (n - 1)
    lo = int(idx)
    hi = min(lo + 1, n - 1)
    frac = idx - lo
    return sorted_data[lo] * (1 - frac) + sorted_data[hi] * frac


def _dist_stats(values: list[int | float]) -> dict:
    """Compute summary statistics for a numeric list."""
    if not values:
        return {
            "count": 0,
            "min": None, "max": None,
            "mean": None, "median": None,
            "std": None,
            "p95": None, "p99": None,
        }
    sv = sorted(values)
    n = len(sv)
    mean = sum(sv) / n
    variance = sum((x - mean) ** 2 for x in sv) / n if n > 1 else 0.0
    std = math.sqrt(variance)
    return {
        "count": n,
        "min": sv[0],
        "max": sv[-1],
        "mean": round(mean, 4),
        "median": _percentile(sv, 50),
        "std": round(std, 4),
        "p95": _percentile(sv, 95),
        "p99": _percentile(sv, 99),
    }


def _histogram(values: list[int], bins: int = 10) -> list[dict]:
    """Return a simple equal-width histogram."""
    if not values:
        return []
    lo, hi = min(values), max(values)
    if lo == hi:
        return [{"lo": lo, "hi": hi, "count": len(values)}]
    width = (hi - lo) / bins
    buckets = [0] * bins
    for v in values:
        idx = min(int((v - lo) / width), bins - 1)
        buckets[idx] += 1
    result = []
    for i, count in enumerate(buckets):
        result.append({
            "lo": round(lo + i * width, 2),
            "hi": round(lo + (i + 1) * width, 2),
            "count": count,
        })
    return result


# ─── main analysis ───────────────────────────────────────────────────────────

def analyze_dump(path: Path, band_name: str | None = None) -> dict:
    """Load a dump and compute all statistics. Returns the full stats dict."""
    try:
        from dump_io import read_dump_iter
    except ImportError:
        sys.path.insert(0, str(Path(__file__).parent))
        from dump_io import read_dump_iter  # type: ignore[no-redef]

    # Per-tile accumulators
    total = 0
    dead_count = 0
    overflow_count = 0
    parse_error_count = 0

    groups_per_tile: list[int] = []
    ports_per_face: dict[str, list[int]] = {f: [] for f in FACES}
    total_ports_per_tile: list[int] = []
    prime_counts: list[int] = []
    payload_used_list: list[int] = []
    slack_list: list[int] = []

    for rec in read_dump_iter(path):
        total += 1
        tileop: bytes = rec["tileop"]

        if _is_overflow(tileop):
            overflow_count += 1
            prime_counts.append(rec["prime_count"])
            continue

        if _is_dead(tileop):
            dead_count += 1
            prime_counts.append(rec["prime_count"])
            continue

        parsed = _parse_tileop(tileop)
        if parsed is None:
            parse_error_count += 1
            continue

        prime_counts.append(rec["prime_count"])

        # Groups per tile: unique group IDs across all faces
        all_groups: set[int] = set()
        for face in FACES:
            for gid in parsed["groups"][face]:
                if gid != 0:
                    all_groups.add(gid)
        groups_per_tile.append(len(all_groups))

        # Ports per face and total
        tile_total_ports = 0
        for face in FACES:
            cnt = parsed[f"{face.lower()}_cnt"]
            ports_per_face[face].append(cnt)
            tile_total_ports += cnt
        total_ports_per_tile.append(tile_total_ports)

        payload_used_list.append(parsed["payload_used"])
        slack_list.append(parsed["slack"])

    # Derive stats
    normal_count = total - dead_count - overflow_count - parse_error_count

    stats: dict = {
        "band_name": band_name or path.stem,
        "dump_path": str(path),
        "tile_counts": {
            "total": total,
            "normal": normal_count,
            "dead": dead_count,
            "overflow_poisoned": overflow_count,
            "parse_errors": parse_error_count,
            "pct_dead": round(100.0 * dead_count / total, 3) if total else 0.0,
            "pct_poisoned": round(100.0 * overflow_count / total, 3) if total else 0.0,
        },
        "prime_count": _dist_stats(prime_counts),
        "groups_per_tile": {
            **_dist_stats(groups_per_tile),
            "histogram": _histogram(groups_per_tile),
        },
        "ports_per_tile_total": _dist_stats(total_ports_per_tile),
        "ports_per_face": {
            face: _dist_stats(ports_per_face[face]) for face in FACES
        },
        "payload_budget": {
            **_dist_stats(payload_used_list),
            "budget": PAYLOAD_BUDGET,
            "slack": _dist_stats(slack_list),
            "histogram_used": _histogram(payload_used_list),
        },
    }
    return stats


# ─── report formatting ───────────────────────────────────────────────────────

def _fmt_dist(d: dict, indent: str = "    ") -> list[str]:
    if d["count"] == 0:
        return [f"{indent}(no data)"]
    lines = [
        f"{indent}count  : {d['count']}",
        f"{indent}min    : {d['min']}",
        f"{indent}max    : {d['max']}",
        f"{indent}mean   : {d['mean']}",
        f"{indent}median : {d['median']}",
        f"{indent}std    : {d['std']}",
        f"{indent}p95    : {d['p95']}",
        f"{indent}p99    : {d['p99']}",
    ]
    return lines


def _fmt_histogram(hist: list[dict], label: str, indent: str = "    ") -> list[str]:
    if not hist:
        return []
    lines = [f"{indent}{label}:"]
    max_count = max(b["count"] for b in hist)
    bar_width = 30
    for b in hist:
        bar_len = int(bar_width * b["count"] / max_count) if max_count else 0
        bar = "#" * bar_len
        lines.append(f"{indent}  [{b['lo']:>6.1f}, {b['hi']:>6.1f})  {bar:<{bar_width}}  {b['count']}")
    return lines


def print_report(stats: dict) -> None:
    tc = stats["tile_counts"]
    total = tc["total"]

    def section(title: str) -> None:
        print()
        print(f"{'=' * 60}")
        print(f"  {title}")
        print(f"{'=' * 60}")

    def sub(title: str) -> None:
        print(f"\n  --- {title} ---")

    print(f"\nGaussian Moat Tile Dump Analysis")
    print(f"  Band / dump : {stats['band_name']}")
    print(f"  File        : {stats['dump_path']}")

    section("TILE COUNTS")
    print(f"  Total              : {total}")
    print(f"  Normal             : {tc['normal']}")
    print(f"  Dead               : {tc['dead']}  ({tc['pct_dead']:.2f}%)")
    print(f"  Overflow/Poisoned  : {tc['overflow_poisoned']}  ({tc['pct_poisoned']:.2f}%)")
    if tc["parse_errors"]:
        print(f"  Parse errors       : {tc['parse_errors']}")

    section("PRIME COUNT  (all tiles — note: CUDA includes collar, C++ does not)")
    for line in _fmt_dist(stats["prime_count"]):
        print(line)

    section("GROUPS PER TILE  (normal tiles only)")
    for line in _fmt_dist(stats["groups_per_tile"]):
        print(line)
    for line in _fmt_histogram(stats["groups_per_tile"].get("histogram", []), "histogram"):
        print(line)

    section("PORTS PER TILE — TOTAL  (normal tiles only)")
    for line in _fmt_dist(stats["ports_per_tile_total"]):
        print(line)

    section("PORTS PER FACE  (normal tiles only)")
    for face in FACES:
        d = stats["ports_per_face"][face]
        sub(f"Face {face}  (cost: {FACE_COST[face]} byte(s)/port)")
        for line in _fmt_dist(d, indent="      "):
            print(line)

    section("PAYLOAD BUDGET USAGE  (normal tiles only)")
    bd = stats["payload_budget"]
    print(f"  Budget          : {bd['budget']} bytes")
    for line in _fmt_dist({k: bd[k] for k in ("count","min","max","mean","median","std","p95","p99")}):
        print(line)
    sub("Slack (unused bytes)")
    for line in _fmt_dist(bd["slack"], indent="      "):
        print(line)
    for line in _fmt_histogram(bd.get("histogram_used", []), "histogram of bytes used"):
        print(line)

    print()
    print("=" * 60)
    print()


# ─── CLI ─────────────────────────────────────────────────────────────────────

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Statistical analysis of Gaussian Moat tile dumps."
    )
    parser.add_argument("dump", help="Binary dump file (.bin)")
    parser.add_argument(
        "--band-name", default=None, metavar="NAME",
        help="Optional label for the band/experiment (defaults to filename stem)",
    )
    parser.add_argument(
        "--no-json", action="store_true",
        help="Skip writing the .stats.json file",
    )
    args = parser.parse_args(argv)

    dump_path = Path(args.dump)
    if not dump_path.exists():
        print(f"ERROR: file not found: {dump_path}", file=sys.stderr)
        return 2

    print(f"Analyzing {dump_path} ...", file=sys.stderr)
    stats = analyze_dump(dump_path, band_name=args.band_name)

    print_report(stats)

    if not args.no_json:
        json_path = dump_path.with_suffix(dump_path.suffix + ".stats.json")
        with json_path.open("w") as f:
            json.dump(stats, f, indent=2)
        print(f"JSON summary written to: {json_path}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
