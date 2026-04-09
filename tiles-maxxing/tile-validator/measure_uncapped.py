#!/usr/bin/env python3
"""Measure true (uncapped) port-per-face distribution for overflow tiles.

Reads the census CSV, samples overflow tiles, runs the full Python pipeline
without any 16-port cap, and reports the true distribution.

Also processes ~100 non-overflow tiles for calibration (should match C++ counts).
"""

from __future__ import annotations

import csv
import os
import sys
import time
import statistics
from collections import defaultdict

# Ensure our package is importable
sys.path.insert(0, os.path.dirname(__file__))

from primes import is_gaussian_prime, BACKEND as PRIME_BACKEND
from uf import UnionFind, make_backward_offsets

# --- Constants (matching tile.py and constants.h) ---
TILE_SIDE = 256
COLLAR = 7
TILE_POINTS = TILE_SIDE + 1       # 257 lattice points per axis
SIDE_EXP = TILE_POINTS + 2 * COLLAR  # 271
K_SQ = 40
FACES = ("I", "O", "L", "R")
BACKWARD_OFFSETS = make_backward_offsets(K_SQ)

CENSUS_CSV = os.path.join(
    os.path.dirname(__file__),
    "..", "tile-cpp", "census_output", "census_R860000000_T3125.csv",
)
OUTPUT_CSV = os.path.join(
    os.path.dirname(__file__),
    "..", "tile-cpp", "census_output", "uncapped_R860000000.csv",
)


# ---------------------------------------------------------------------------
# Core pipeline (reused from diagnose_tile.py, no cap)
# ---------------------------------------------------------------------------

def sieve_bitmap(a_lo: int, b_lo: int) -> list[int]:
    word_count = (SIDE_EXP * SIDE_EXP + 31) // 32
    bitmap = [0] * word_count
    base_a = a_lo - COLLAR
    base_b = b_lo - COLLAR
    for row in range(SIDE_EXP):
        a = base_a + row
        row_base = row * SIDE_EXP
        for col in range(SIDE_EXP):
            b = base_b + col
            if is_gaussian_prime(a, b):
                pos = row_base + col
                bitmap[pos >> 5] |= 1 << (pos & 31)
    return bitmap


def compact_bitmap(bitmap: list[int]) -> tuple[int, list[int], list[int]]:
    counts = [word.bit_count() for word in bitmap]
    prefix = [0] * len(bitmap)
    running = 0
    for i, count in enumerate(counts):
        prefix[i] = running
        running += count
    prime_pos = [0] * running
    for word_index, word in enumerate(bitmap):
        idx = prefix[word_index]
        cursor = word
        while cursor:
            bit = (cursor & -cursor).bit_length() - 1
            prime_pos[idx] = word_index * 32 + bit
            idx += 1
            cursor &= cursor - 1
    return running, prefix, prime_pos


def bitmap_test(bitmap: list[int], pos: int) -> bool:
    return bool(bitmap[pos >> 5] & (1 << (pos & 31)))


def bitmap_pos_to_compact_index(bitmap: list[int], prefix: list[int], pos: int) -> int:
    word = pos >> 5
    bit = pos & 31
    mask = (1 << bit) - 1
    return prefix[word] + (bitmap[word] & mask).bit_count()


def build_components(bitmap, prefix, prime_pos, count):
    if count == 0:
        return []
    uf = UnionFind(count)
    for i, pos in enumerate(prime_pos):
        row = pos // SIDE_EXP
        col = pos % SIDE_EXP
        for dr, dc in BACKWARD_OFFSETS:
            nr = row + dr
            nc = col + dc
            if nr < 0 or nr >= SIDE_EXP or nc < 0 or nc >= SIDE_EXP:
                continue
            npos = nr * SIDE_EXP + nc
            if bitmap_test(bitmap, npos):
                j = bitmap_pos_to_compact_index(bitmap, prefix, npos)
                uf.union(i, j)
    return uf.flatten()


def collect_face_primes(prime_pos, a_lo, b_lo):
    faces = {f: [] for f in FACES}
    base_a = a_lo - COLLAR
    base_b = b_lo - COLLAR
    for prime_index, pos in enumerate(prime_pos):
        row = pos // SIDE_EXP
        col = pos % SIDE_EXP
        tile_row = row - COLLAR
        tile_col = col - COLLAR
        if not (0 <= tile_row <= TILE_SIDE and 0 <= tile_col <= TILE_SIDE):
            continue
        a = base_a + row
        b = base_b + col
        if tile_row < COLLAR:
            faces["I"].append({
                "prime_index": prime_index, "row": row, "col": col,
                "a": a, "b": b, "h": tile_col, "depth": tile_row,
            })
        if tile_row >= TILE_SIDE - COLLAR + 1:
            faces["O"].append({
                "prime_index": prime_index, "row": row, "col": col,
                "a": a, "b": b, "h": tile_col, "depth": TILE_SIDE - tile_row,
            })
        if tile_col < COLLAR:
            faces["L"].append({
                "prime_index": prime_index, "row": row, "col": col,
                "a": a, "b": b, "h": tile_row, "depth": tile_col,
            })
        if tile_col >= TILE_SIDE - COLLAR + 1:
            faces["R"].append({
                "prime_index": prime_index, "row": row, "col": col,
                "a": a, "b": b, "h": tile_row, "depth": TILE_SIDE - tile_col,
            })
    for f in FACES:
        faces[f].sort(key=lambda p: (p["h"], p["depth"], p["row"], p["col"], p["prime_index"]))
    return faces


def cluster_into_ports(face_primes, face, component_roots):
    if not face_primes:
        return []
    ports = []
    current_cluster = [face_primes[0]]
    for i in range(1, len(face_primes)):
        prev = face_primes[i - 1]
        curr = face_primes[i]
        dx = curr["col"] - prev["col"]
        dy = curr["row"] - prev["row"]
        dist_sq = dx * dx + dy * dy
        if dist_sq <= K_SQ:
            current_cluster.append(curr)
        else:
            root = component_roots[current_cluster[0]["prime_index"]]
            ports.append({
                "face": face,
                "component_root": root,
                "primes": current_cluster,
                "size": len(current_cluster),
            })
            current_cluster = [curr]
    root = component_roots[current_cluster[0]["prime_index"]]
    ports.append({
        "face": face,
        "component_root": root,
        "primes": current_cluster,
        "size": len(current_cluster),
    })
    return ports


def assign_groups(all_ports_by_face):
    root_to_group = {}
    next_group = 1
    result = {f: [] for f in FACES}
    for face in FACES:
        for port in all_ports_by_face[face]:
            root = port["component_root"]
            if root not in root_to_group:
                root_to_group[root] = next_group
                next_group += 1
            port["group"] = root_to_group[root]
            result[face].append(port)
    return result, root_to_group, next_group - 1


def compute_group_face_incidence(ports_by_face):
    incidence = defaultdict(lambda: defaultdict(int))
    for face in FACES:
        for port in ports_by_face[face]:
            incidence[port["group"]][face] += 1
    return incidence


def identify_dead_ends(incidence):
    dead_ends = set()
    for group, face_counts in incidence.items():
        total_faces = len(face_counts)
        total_ports = sum(face_counts.values())
        if total_faces == 1 and total_ports == 1:
            dead_ends.add(group)
    return dead_ends


def prune(ports_by_face, dead_ends):
    result = {f: [] for f in FACES}
    for face in FACES:
        for port in ports_by_face[face]:
            if port["group"] not in dead_ends:
                result[face].append(port)
    return result


def measure_tile(a_lo: int, b_lo: int) -> dict:
    """Run full pipeline on a tile, return uncapped per-face port counts."""
    bitmap = sieve_bitmap(a_lo, b_lo)
    count, prefix, prime_pos = compact_bitmap(bitmap)
    roots = build_components(bitmap, prefix, prime_pos, count)
    face_primes = collect_face_primes(prime_pos, a_lo, b_lo)

    raw_ports = {f: cluster_into_ports(face_primes[f], f, roots) for f in FACES}
    ports_by_face, root_to_group, total_groups = assign_groups(raw_ports)
    incidence = compute_group_face_incidence(ports_by_face)
    dead_ends = identify_dead_ends(incidence)
    pruned = prune(ports_by_face, dead_ends)

    face_counts = {f: len(pruned[f]) for f in FACES}
    total_ports = sum(face_counts.values())
    max_face = max(face_counts.values())
    surviving_groups = len(set(
        p["group"] for f in FACES for p in ports_by_face[f]
        if p["group"] not in dead_ends
    ))

    return {
        "a_lo": a_lo,
        "b_lo": b_lo,
        "face_I_ports": face_counts["I"],
        "face_O_ports": face_counts["O"],
        "face_L_ports": face_counts["L"],
        "face_R_ports": face_counts["R"],
        "max_face_ports": max_face,
        "total_ports": total_ports,
        "group_count": surviving_groups,
    }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print(f"Primality backend: {PRIME_BACKEND}")
    print(f"Reading census: {os.path.abspath(CENSUS_CSV)}")

    # Read census
    overflow_tiles = []
    non_overflow_tiles = []
    with open(CENSUS_CSV, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rec = {
                "a_lo": int(row["a_lo"]),
                "b_lo": int(row["b_lo"]),
                "ports_before": int(row["ports_before"]),
                "overflow": int(row["overflow"]),
                "cpp_face_I": int(row["face_I_ports"]),
                "cpp_face_O": int(row["face_O_ports"]),
                "cpp_face_L": int(row["face_L_ports"]),
                "cpp_face_R": int(row["face_R_ports"]),
                "cpp_group_count": int(row["group_count"]),
            }
            if rec["overflow"] == 1:
                overflow_tiles.append(rec)
            else:
                non_overflow_tiles.append(rec)

    print(f"Total tiles in census: {len(overflow_tiles) + len(non_overflow_tiles)}")
    print(f"  Overflow:     {len(overflow_tiles)}")
    print(f"  Non-overflow: {len(non_overflow_tiles)}")

    # --- Sample overflow tiles ---
    N_OVERFLOW = 500
    # Sort by ports_before descending to ensure we get the densest
    overflow_tiles.sort(key=lambda r: r["ports_before"], reverse=True)

    # Take top 50 densest
    top_dense = overflow_tiles[:50]

    # Uniform sample from the rest
    remaining = overflow_tiles[50:]
    uniform_count = N_OVERFLOW - len(top_dense)
    if len(remaining) <= uniform_count:
        uniform_sample = remaining
    else:
        step = len(remaining) / uniform_count
        uniform_sample = [remaining[int(i * step)] for i in range(uniform_count)]

    overflow_sample = top_dense + uniform_sample
    print(f"\nOverflow sample: {len(overflow_sample)} tiles")
    print(f"  Top 50 densest (ports_before): {top_dense[0]['ports_before']}..{top_dense[-1]['ports_before']}")
    print(f"  Uniform sample from remaining: {len(uniform_sample)} tiles")

    # --- Sample non-overflow tiles ---
    N_CALIBRATION = 100
    if len(non_overflow_tiles) <= N_CALIBRATION:
        calibration_sample = non_overflow_tiles
    else:
        step = len(non_overflow_tiles) / N_CALIBRATION
        calibration_sample = [non_overflow_tiles[int(i * step)] for i in range(N_CALIBRATION)]

    print(f"Calibration sample: {len(calibration_sample)} non-overflow tiles")

    # --- Process overflow tiles ---
    print(f"\n{'='*70}")
    print(f"Processing {len(overflow_sample)} overflow tiles...")
    print(f"{'='*70}")

    overflow_results = []
    t0 = time.time()
    for idx, tile in enumerate(overflow_sample):
        result = measure_tile(tile["a_lo"], tile["b_lo"])
        overflow_results.append(result)
        if (idx + 1) % 50 == 0 or idx == 0:
            elapsed = time.time() - t0
            rate = (idx + 1) / elapsed
            eta = (len(overflow_sample) - idx - 1) / rate
            print(f"  [{idx+1}/{len(overflow_sample)}] "
                  f"({elapsed:.1f}s elapsed, {rate:.1f} tiles/s, ETA {eta:.0f}s) "
                  f"tile ({tile['a_lo']}, {tile['b_lo']}): "
                  f"max_face={result['max_face_ports']}")

    t_overflow = time.time() - t0
    print(f"\nOverflow processing: {t_overflow:.1f}s ({len(overflow_results)/t_overflow:.1f} tiles/s)")

    # --- Process calibration tiles ---
    print(f"\n{'='*70}")
    print(f"Processing {len(calibration_sample)} calibration (non-overflow) tiles...")
    print(f"{'='*70}")

    calibration_results = []
    mismatches = 0
    t0 = time.time()
    for idx, tile in enumerate(calibration_sample):
        result = measure_tile(tile["a_lo"], tile["b_lo"])
        calibration_results.append(result)

        # Check against C++ census
        match = True
        if tile["cpp_face_I"] != -1:  # -1 means C++ capped/overflow
            if result["face_I_ports"] != tile["cpp_face_I"]:
                match = False
            if result["face_O_ports"] != tile["cpp_face_O"]:
                match = False
            if result["face_L_ports"] != tile["cpp_face_L"]:
                match = False
            if result["face_R_ports"] != tile["cpp_face_R"]:
                match = False
        if not match:
            mismatches += 1
            print(f"  MISMATCH tile ({tile['a_lo']}, {tile['b_lo']}): "
                  f"Python I={result['face_I_ports']} O={result['face_O_ports']} "
                  f"L={result['face_L_ports']} R={result['face_R_ports']} vs "
                  f"C++ I={tile['cpp_face_I']} O={tile['cpp_face_O']} "
                  f"L={tile['cpp_face_L']} R={tile['cpp_face_R']}")

        if (idx + 1) % 25 == 0 or idx == 0:
            elapsed = time.time() - t0
            rate = (idx + 1) / elapsed
            eta = (len(calibration_sample) - idx - 1) / rate
            print(f"  [{idx+1}/{len(calibration_sample)}] "
                  f"({elapsed:.1f}s, {rate:.1f} tiles/s, ETA {eta:.0f}s) "
                  f"max_face={result['max_face_ports']}")

    t_calib = time.time() - t0
    print(f"\nCalibration: {t_calib:.1f}s, {mismatches} mismatches out of {len(calibration_results)}")

    # --- Save CSV ---
    all_results = overflow_results + calibration_results
    with open(OUTPUT_CSV, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "a_lo", "b_lo", "face_I_ports", "face_O_ports",
            "face_L_ports", "face_R_ports", "max_face_ports",
            "total_ports", "group_count",
        ])
        writer.writeheader()
        for r in all_results:
            writer.writerow(r)
    print(f"\nResults saved to: {os.path.abspath(OUTPUT_CSV)}")

    # --- Statistical output ---
    print(f"\n{'='*70}")
    print(f"=== Uncapped Port Distribution (N={len(overflow_results)} overflow tiles, R=860M) ===")
    print(f"{'='*70}")

    max_faces = [r["max_face_ports"] for r in overflow_results]
    max_faces_sorted = sorted(max_faces)
    n = len(max_faces)

    print(f"\nMax face ports per tile:")
    print(f"  min={min(max_faces)}  mean={statistics.mean(max_faces):.2f}  "
          f"median={statistics.median(max_faces):.1f}  "
          f"p95={max_faces_sorted[int(n*0.95)]}  "
          f"p99={max_faces_sorted[int(n*0.99)]}  "
          f"max={max(max_faces)}")

    print(f"\nPer-face breakdown:")
    for face_key, face_label in [("face_I_ports", "Face I"),
                                  ("face_O_ports", "Face O"),
                                  ("face_L_ports", "Face L"),
                                  ("face_R_ports", "Face R")]:
        vals = [r[face_key] for r in overflow_results]
        print(f"  {face_label}: min={min(vals)}  mean={statistics.mean(vals):.2f}  "
              f"median={statistics.median(vals):.1f}  max={max(vals)}")

    print(f"\nHistogram of max face ports:")
    hist = defaultdict(int)
    for v in max_faces:
        hist[v] += 1
    for k in sorted(hist.keys()):
        bar = "#" * min(hist[k], 80)
        print(f"  {k:3d}: {hist[k]:5d} tiles  {bar}")

    # Cumulative: what cap captures what fraction
    # Include ALL tiles (overflow + calibration non-overflow) for the "all tiles" view
    # But the main stat is overflow-only since non-overflow are already <=16
    print(f"\nCumulative: what PORTS_PER_FACE would capture N% of overflow tiles:")
    for cap in [16, 17, 18, 19, 20, 22, 24, 28, 32, 40, 48, 64]:
        count_under = sum(1 for v in max_faces if v <= cap)
        pct = count_under / n * 100
        label = " (current)" if cap == 16 else ""
        print(f"  {cap:3d}: {pct:6.2f}% ({count_under}/{n}){label}")

    # Calibration summary
    print(f"\n{'='*70}")
    print(f"=== Calibration (N={len(calibration_results)} non-overflow tiles) ===")
    print(f"{'='*70}")
    cal_maxes = [r["max_face_ports"] for r in calibration_results]
    cal_over16 = sum(1 for v in cal_maxes if v > 16)
    print(f"  Max face ports: min={min(cal_maxes)}  mean={statistics.mean(cal_maxes):.2f}  "
          f"median={statistics.median(cal_maxes):.1f}  max={max(cal_maxes)}")
    print(f"  Tiles with max_face > 16: {cal_over16}/{len(calibration_results)} "
          f"(should be 0 for calibration)")
    print(f"  C++ mismatches: {mismatches}/{len(calibration_results)}")

    # Extreme tiles
    print(f"\n{'='*70}")
    print(f"=== Top 10 most extreme overflow tiles ===")
    print(f"{'='*70}")
    overflow_results.sort(key=lambda r: r["max_face_ports"], reverse=True)
    for r in overflow_results[:10]:
        print(f"  ({r['a_lo']:>12d}, {r['b_lo']:>12d}): "
              f"I={r['face_I_ports']:2d} O={r['face_O_ports']:2d} "
              f"L={r['face_L_ports']:2d} R={r['face_R_ports']:2d}  "
              f"max={r['max_face_ports']:2d}  total={r['total_ports']:3d}  "
              f"groups={r['group_count']}")


if __name__ == "__main__":
    main()
