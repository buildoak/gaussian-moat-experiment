#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "gmpy2>=2.2.1",
# ]
# ///
"""Empirical TileOp v3 sizing study.

Samples snapped-grid tiles from an octant annulus and measures the quantities
that drive the v3 TileOp budget: prime counts, face ports, and UF labels
referenced by ports.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

S = 256
FACES = ("I", "O", "L", "R")

try:
    import gmpy2

    def is_rational_prime(n: int) -> bool:
        return bool(gmpy2.is_prime(gmpy2.mpz(n)))

    PRIME_BACKEND = "gmpy2"
except ImportError:
    TRIAL_DIVISORS = (
        2,
        3,
        5,
        7,
        11,
        13,
        17,
        19,
        23,
        29,
        31,
        37,
        41,
        43,
        47,
        53,
        59,
        61,
        67,
        71,
        73,
        79,
        83,
        89,
        97,
    )

    def is_rational_prime(n: int) -> bool:
        if n < 2:
            return False
        for p in TRIAL_DIVISORS:
            if n == p:
                return True
            if n % p == 0:
                return False

        d = n - 1
        s = 0
        while d % 2 == 0:
            d //= 2
            s += 1

        # Deterministic for n < 2^64.
        for a in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
            if a >= n:
                continue
            x = pow(a, d, n)
            if x == 1 or x == n - 1:
                continue
            for _ in range(s - 1):
                x = (x * x) % n
                if x == n - 1:
                    break
            else:
                return False
        return True

    PRIME_BACKEND = "deterministic-mr"


@dataclass(frozen=True)
class TileCoord:
    i: int
    j: int
    a_lo: int
    b_lo: int


class UnionFind:
    __slots__ = ("parent",)

    def __init__(self, n: int):
        self.parent = list(range(n))

    def find(self, x: int) -> int:
        parent = self.parent
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(self, a: int, b: int) -> None:
        ra = self.find(a)
        rb = self.find(b)
        if ra == rb:
            return
        if ra > rb:
            ra, rb = rb, ra
        self.parent[rb] = ra

    def flattened_roots(self) -> list[int]:
        return [self.find(i) for i in range(len(self.parent))]


def floor_isqrt(n: int) -> int:
    return math.isqrt(n)


def is_gaussian_prime(a: int, b: int) -> bool:
    if a == 0 and b == 0:
        return False
    if a == 0:
        ab = abs(b)
        return ab % 4 == 3 and is_rational_prime(ab)
    if b == 0:
        ab = abs(a)
        return ab % 4 == 3 and is_rational_prime(ab)
    return is_rational_prime(a * a + b * b)


def edge_offsets(k_sq: int) -> list[tuple[int, int]]:
    reach = math.isqrt(k_sq)
    offsets: list[tuple[int, int]] = []
    for da in range(-reach, reach + 1):
        for db in range(-reach, reach + 1):
            if da == 0 and db >= 0:
                continue
            if da > 0:
                continue
            if da * da + db * db <= k_sq:
                offsets.append((da, db))
    return offsets


def enumerate_primes(tile: TileCoord, collar: int) -> tuple[list[tuple[int, int]], dict[tuple[int, int], int]]:
    primes: list[tuple[int, int]] = []
    index: dict[tuple[int, int], int] = {}
    for a in range(tile.a_lo - collar, tile.a_lo + S + collar + 1):
        for b in range(tile.b_lo - collar, tile.b_lo + S + collar + 1):
            if is_gaussian_prime(a, b):
                index[(a, b)] = len(primes)
                primes.append((a, b))
    return primes, index


def build_tile_uf(
    primes: list[tuple[int, int]],
    index: dict[tuple[int, int], int],
    offsets: Iterable[tuple[int, int]],
) -> tuple[UnionFind, list[int]]:
    uf = UnionFind(len(primes))
    for idx, (a, b) in enumerate(primes):
        for da, db in offsets:
            other = index.get((a + da, b + db))
            if other is not None:
                uf.union(idx, other)
    return uf, uf.flattened_roots()


def face_key(face: str, tile: TileCoord, prime: tuple[int, int]) -> tuple[int, int]:
    a, b = prime
    if face == "I":
        return (a - tile.a_lo, b - tile.b_lo)
    if face == "O":
        return (a - tile.a_lo, b - (tile.b_lo + S))
    if face == "L":
        return (b - tile.b_lo, a - tile.a_lo)
    if face == "R":
        return (b - tile.b_lo, a - (tile.a_lo + S))
    raise ValueError(face)


def face_prime_indices(
    face: str,
    tile: TileCoord,
    primes: list[tuple[int, int]],
    collar: int,
) -> list[int]:
    result: list[int] = []
    for idx, (a, b) in enumerate(primes):
        if face == "I" and abs(b - tile.b_lo) <= collar:
            result.append(idx)
        elif face == "O" and abs(b - (tile.b_lo + S)) <= collar:
            result.append(idx)
        elif face == "L" and abs(a - tile.a_lo) <= collar:
            result.append(idx)
        elif face == "R" and abs(a - (tile.a_lo + S)) <= collar:
            result.append(idx)
    result.sort(key=lambda idx: (*face_key(face, tile, primes[idx]), idx))
    return result


def face_ports(
    face: str,
    tile: TileCoord,
    primes: list[tuple[int, int]],
    prime_index: dict[tuple[int, int], int],
    face_indices: list[int],
    tile_roots: list[int],
    offsets: Iterable[tuple[int, int]],
) -> list[int]:
    if not face_indices:
        return []

    local_by_global = {global_idx: local_idx for local_idx, global_idx in enumerate(face_indices)}
    uf = UnionFind(len(face_indices))
    for local_idx, global_idx in enumerate(face_indices):
        a, b = primes[global_idx]
        for da, db in offsets:
            other_global = prime_index.get((a + da, b + db))
            if other_global is None:
                continue
            other_local = local_by_global.get(other_global)
            if other_local is not None:
                uf.union(local_idx, other_local)

    face_roots = uf.flattened_roots()
    members_by_root: dict[int, list[int]] = {}
    for local_idx, root in enumerate(face_roots):
        members_by_root.setdefault(root, []).append(face_indices[local_idx])

    port_reps: list[tuple[tuple[int, int, int, int], int]] = []
    for members in members_by_root.values():
        rep = min(members, key=lambda idx: (*face_key(face, tile, primes[idx]), idx))
        h, p_perp = face_key(face, tile, primes[rep])
        port_reps.append(((h, p_perp, p_perp, h), tile_roots[rep]))
    port_reps.sort(key=lambda item: item[0])
    return [tile_root for _, tile_root in port_reps]


def tile_stats(tile: TileCoord, k_sq: int) -> dict[str, int]:
    collar = floor_isqrt(k_sq)
    offsets = edge_offsets(k_sq)
    primes, prime_index = enumerate_primes(tile, collar)
    _, roots = build_tile_uf(primes, prime_index, offsets)
    all_groups = len(set(roots))

    port_counts: dict[str, int] = {}
    port_group_roots: set[int] = set()
    for face in FACES:
        indices = face_prime_indices(face, tile, primes, collar)
        ports = face_ports(face, tile, primes, prime_index, indices, roots, offsets)
        port_counts[face] = len(ports)
        port_group_roots.update(ports)

    sum_ports = sum(port_counts.values())
    max_label = len(port_group_roots)
    return {
        "i": tile.i,
        "j": tile.j,
        "a_lo": tile.a_lo,
        "b_lo": tile.b_lo,
        "k_sq": k_sq,
        "primes": len(primes),
        "dsu_groups": all_groups,
        "port_groups": max_label,
        "ports_I": port_counts["I"],
        "ports_O": port_counts["O"],
        "ports_L": port_counts["L"],
        "ports_R": port_counts["R"],
        "sum_n": sum_ports,
        "max_label": max_label,
        "overflow_128": int(sum_ports > 96 or max_label > 64),
        "overflow_256": int(sum_ports > 192 or max_label > 128),
    }


def tile_intersects_annulus(tile: TileCoord, r_inner_sq: int, r_outer_sq: int) -> bool:
    # Conservative integer scan over the closed 257x257 proper tile. This is
    # cheap for sampling and avoids floating-point boundary mistakes.
    for a in range(tile.a_lo, tile.a_lo + S + 1):
        for b in range(tile.b_lo, tile.b_lo + S + 1):
            norm = a * a + b * b
            if r_inner_sq <= norm <= r_outer_sq and a <= b:
                return True
    return False


def candidate_tiles(
    r_center: int,
    r_halfwidth: int,
    offset_x: int,
    offset_y: int,
) -> list[TileCoord]:
    r_inner = r_center - r_halfwidth
    r_outer = r_center + r_halfwidth
    r_inner_sq = r_inner * r_inner
    r_outer_sq = r_outer * r_outer
    i_min = math.floor((0 - offset_x) / S)
    i_max = math.floor((math.ceil(r_outer / math.sqrt(2)) - offset_x) / S)
    tiles: list[TileCoord] = []

    for i in range(i_min, i_max + 1):
        a_lo = offset_x + i * S
        a0 = max(0, a_lo)
        a1 = min(a_lo + S, math.ceil(r_outer / math.sqrt(2)))
        if a0 > a1:
            continue

        min_b = max(a0, math.isqrt(max(0, r_inner_sq - a1 * a1)))
        while min_b * min_b + a1 * a1 < r_inner_sq:
            min_b += 1
        max_b = math.isqrt(max(0, r_outer_sq - a0 * a0))
        j_min = math.floor((min_b - offset_y - S) / S) - 1
        j_max = math.floor((max_b - offset_y) / S) + 1

        for j in range(j_min, j_max + 1):
            tile = TileCoord(i=i, j=j, a_lo=a_lo, b_lo=offset_y + j * S)
            if tile.b_lo + S < tile.a_lo:
                continue
            if tile_intersects_annulus(tile, r_inner_sq, r_outer_sq):
                tiles.append(tile)
    return tiles


def sample_tiles(
    r_center: int,
    r_halfwidth: int,
    n_tiles: int,
    offset_x: int,
    offset_y: int,
    seed: int,
) -> list[TileCoord]:
    rng = random.Random(seed)
    r_inner = r_center - r_halfwidth
    r_outer = r_center + r_halfwidth
    r_inner_sq = r_inner * r_inner
    r_outer_sq = r_outer * r_outer
    samples: dict[tuple[int, int], TileCoord] = {}
    attempts = 0
    max_attempts = max(10_000, n_tiles * 1_000)

    while len(samples) < n_tiles and attempts < max_attempts:
        attempts += 1
        theta = rng.uniform(math.pi / 4.0, math.pi / 2.0)
        # Area-uniform in the annulus, then snapped to a tile.
        radius_sq = rng.randrange(r_inner_sq, r_outer_sq + 1)
        radius = math.sqrt(radius_sq)
        a = int(radius * math.cos(theta))
        b = int(radius * math.sin(theta))
        i = math.floor((a - offset_x) / S)
        j = math.floor((b - offset_y) / S)
        tile = TileCoord(i=i, j=j, a_lo=offset_x + i * S, b_lo=offset_y + j * S)
        key = (i, j)
        if key in samples:
            continue
        if tile_intersects_annulus(tile, r_inner_sq, r_outer_sq):
            samples[key] = tile

    if len(samples) < n_tiles:
        active = candidate_tiles(r_center, r_halfwidth, offset_x, offset_y)
        if len(active) < n_tiles:
            raise ValueError(f"only {len(active)} active tiles found; requested {n_tiles}")
        return rng.sample(active, n_tiles)
    return list(samples.values())


def percentile(values: list[int], pct: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return float(values[0])
    sorted_values = sorted(values)
    rank = (pct / 100.0) * (len(sorted_values) - 1)
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return float(sorted_values[lo])
    frac = rank - lo
    return sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac


def summarize(rows: list[dict[str, int]]) -> dict[str, dict[str, float]]:
    metrics = [
        "primes",
        "dsu_groups",
        "port_groups",
        "ports_I",
        "ports_O",
        "ports_L",
        "ports_R",
        "sum_n",
        "max_label",
    ]
    summary: dict[str, dict[str, float]] = {}
    for metric in metrics:
        values = [row[metric] for row in rows]
        summary[metric] = {
            "p50": percentile(values, 50),
            "p90": percentile(values, 90),
            "p99": percentile(values, 99),
            "p999": percentile(values, 99.9),
            "max": float(max(values) if values else 0),
            "mean": statistics.fmean(values) if values else 0.0,
        }
    summary["overflow"] = {
        "overflow_128_rate": sum(row["overflow_128"] for row in rows) / len(rows) if rows else 0.0,
        "overflow_256_rate": sum(row["overflow_256"] for row in rows) / len(rows) if rows else 0.0,
    }
    return summary


def parse_int(value: str) -> int:
    return int(float(value))


def default_regimes() -> list[tuple[int, int]]:
    return [(60_000_000, 36), (60_000_000, 40), (80_000_000, 36), (80_000_000, 40), (800_000_000, 36), (800_000_000, 40)]


def run_one(args: argparse.Namespace, r_center: int, k_sq: int) -> dict:
    seed = args.seed + r_center * 100 + k_sq
    tiles = sample_tiles(r_center, args.r_halfwidth, args.n_tiles, args.offset_x, args.offset_y, seed)
    rows: list[dict[str, int]] = []
    start = time.time()
    for idx, tile in enumerate(tiles, start=1):
        row = tile_stats(tile, k_sq)
        row["r_center"] = r_center
        row["r_halfwidth"] = args.r_halfwidth
        rows.append(row)
        if args.progress and (idx == 1 or idx % args.progress == 0 or idx == len(tiles)):
            elapsed = time.time() - start
            print(
                f"R={r_center} K={k_sq}: {idx}/{len(tiles)} tiles "
                f"({elapsed:.1f}s)",
                file=sys.stderr,
                flush=True,
            )
    return {
        "r_center": r_center,
        "r_halfwidth": args.r_halfwidth,
        "k_sq": k_sq,
        "n_tiles": len(rows),
        "offset_x": args.offset_x,
        "offset_y": args.offset_y,
        "prime_backend": PRIME_BACKEND,
        "rows": rows,
        "summary": summarize(rows),
    }


def write_outputs(results: list[dict], output_prefix: Path) -> None:
    output_prefix.parent.mkdir(parents=True, exist_ok=True)
    json_path = output_prefix.with_suffix(".json")
    csv_path = output_prefix.with_suffix(".csv")
    payload = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "tile_side": S,
        "face_order": FACES,
        "results": results,
    }
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    fieldnames = [
        "r_center",
        "r_halfwidth",
        "k_sq",
        "i",
        "j",
        "a_lo",
        "b_lo",
        "primes",
        "dsu_groups",
        "port_groups",
        "ports_I",
        "ports_O",
        "ports_L",
        "ports_R",
        "sum_n",
        "max_label",
        "overflow_128",
        "overflow_256",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            for row in result["rows"]:
                writer.writerow({name: row[name] for name in fieldnames})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--r-center", type=parse_int, help="annulus center radius")
    parser.add_argument("--r-halfwidth", type=parse_int, default=4096)
    parser.add_argument("--k-sq", type=int, choices=(36, 40), help="squared step bound")
    parser.add_argument("--n-tiles", type=int, default=100)
    parser.add_argument("--offset-x", type=int, default=0)
    parser.add_argument("--offset-y", type=int, default=0)
    parser.add_argument("--output-prefix", type=Path, default=Path("results/tileop_sizing_study"))
    parser.add_argument("--seed", type=int, default=20260420)
    parser.add_argument("--progress", type=int, default=10)
    parser.add_argument(
        "--default-regimes",
        action="store_true",
        help="run R in {60e6,80e6,800e6} x K in {36,40}",
    )
    args = parser.parse_args()

    if args.default_regimes:
        regimes = default_regimes()
    else:
        if args.r_center is None or args.k_sq is None:
            parser.error("--r-center and --k-sq are required unless --default-regimes is used")
        regimes = [(args.r_center, args.k_sq)]

    results = [run_one(args, r_center, k_sq) for r_center, k_sq in regimes]
    write_outputs(results, args.output_prefix)
    print(json.dumps({"results": [{k: r[k] for k in ("r_center", "k_sq", "summary")} for r in results]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
