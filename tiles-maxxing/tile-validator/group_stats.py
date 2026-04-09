#!/usr/bin/env python3
"""Deep structural statistics on Gaussian prime tile groups.

Processes a stratified sample of 1000 tiles (500 overflow, 500 non-overflow)
from the census at R=860M. For each tile, runs the full Python pipeline and
collects per-group structural data.

Output:
  - Inline statistical report (sections A-G)
  - CSV: census_output/group_stats_R860000000.csv
"""

from __future__ import annotations

import csv
import os
import sys
import time
import statistics
from collections import defaultdict, Counter

# Ensure our package is importable
sys.path.insert(0, os.path.dirname(__file__))

from primes import is_gaussian_prime, BACKEND as PRIME_BACKEND
from uf import UnionFind, make_backward_offsets

# --- Constants ---
TILE_SIDE = 256
COLLAR = 7
TILE_POINTS = TILE_SIDE + 1       # 257
SIDE_EXP = TILE_POINTS + 2 * COLLAR  # 271
K_SQ = 40
FACES = ("I", "O", "L", "R")
MAX_PORTS_PER_FACE = 16
BACKWARD_OFFSETS = make_backward_offsets(K_SQ)

CENSUS_CSV = os.path.join(
    os.path.dirname(__file__),
    "..", "tile-cpp", "census_output", "census_R860000000_T3125.csv",
)
OUTPUT_CSV = os.path.join(
    os.path.dirname(__file__),
    "..", "tile-cpp", "census_output", "group_stats_R860000000.csv",
)


# ---------------------------------------------------------------------------
# Core pipeline (from diagnose_tile.py / measure_uncapped.py)
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


# ---------------------------------------------------------------------------
# Group-level analysis for a single tile
# ---------------------------------------------------------------------------

def analyze_tile_groups(a_lo: int, b_lo: int, is_overflow: bool):
    """Run full pipeline, return (tile_summary, list_of_group_records)."""
    bitmap = sieve_bitmap(a_lo, b_lo)
    count, prefix, prime_pos = compact_bitmap(bitmap)
    roots = build_components(bitmap, prefix, prime_pos, count)
    face_primes = collect_face_primes(prime_pos, a_lo, b_lo)

    raw_ports = {f: cluster_into_ports(face_primes[f], f, roots) for f in FACES}
    ports_by_face, root_to_group, total_groups = assign_groups(raw_ports)
    incidence = compute_group_face_incidence(ports_by_face)
    dead_ends = identify_dead_ends(incidence)
    pruned = prune(ports_by_face, dead_ends)

    # Count primes in tile proper
    tile_prime_count = 0
    for pos in prime_pos:
        tr = pos // SIDE_EXP - COLLAR
        tc = pos % SIDE_EXP - COLLAR
        if 0 <= tr <= TILE_SIDE and 0 <= tc <= TILE_SIDE:
            tile_prime_count += 1

    ports_before = sum(len(ports_by_face[f]) for f in FACES)
    ports_after = sum(len(pruned[f]) for f in FACES)
    face_ports_after = {f: len(pruned[f]) for f in FACES}
    max_face_after = max(face_ports_after.values())

    # Build group-level records
    # First, count primes per component root (in tile proper)
    root_prime_counts = Counter()
    for idx, pos in enumerate(prime_pos):
        tr = pos // SIDE_EXP - COLLAR
        tc = pos % SIDE_EXP - COLLAR
        if 0 <= tr <= TILE_SIDE and 0 <= tc <= TILE_SIDE:
            root_prime_counts[roots[idx]] += 1

    # Map group -> component root (reverse of root_to_group)
    group_to_root = {}
    for root, grp in root_to_group.items():
        group_to_root[grp] = root

    group_records = []
    for group in sorted(incidence.keys()):
        face_counts = incidence[group]
        num_faces = len(face_counts)
        total_ports = sum(face_counts.values())
        face_I_ports = face_counts.get("I", 0)
        face_O_ports = face_counts.get("O", 0)
        face_L_ports = face_counts.get("L", 0)
        face_R_ports = face_counts.get("R", 0)
        max_ports_on_any_face = max(face_counts.values())

        root = group_to_root.get(group, -1)
        primes_in_group = root_prime_counts.get(root, 0)

        is_dead_end = (num_faces == 1 and total_ports == 1)
        if is_dead_end:
            category = "dead_end"
        elif num_faces == 1:
            category = "single_face_multi_port"
        else:
            category = "multi_face"

        # Face combination string for multi-face groups
        face_combo = "+".join(f for f in FACES if f in face_counts) if num_faces >= 2 else ""

        group_records.append({
            "tile_a_lo": a_lo,
            "tile_b_lo": b_lo,
            "tile_overflow": 1 if is_overflow else 0,
            "group_id": group,
            "num_faces": num_faces,
            "total_ports": total_ports,
            "face_I_ports": face_I_ports,
            "face_O_ports": face_O_ports,
            "face_L_ports": face_L_ports,
            "face_R_ports": face_R_ports,
            "max_ports_on_any_face": max_ports_on_any_face,
            "primes_in_group": primes_in_group,
            "is_dead_end": is_dead_end,
            "category": category,
            "face_combination": face_combo,
        })

    tile_summary = {
        "a_lo": a_lo,
        "b_lo": b_lo,
        "is_overflow": is_overflow,
        "tile_primes": tile_prime_count,
        "total_groups": total_groups,
        "ports_before": ports_before,
        "ports_after": ports_after,
        "dead_end_count": sum(1 for g in group_records if g["category"] == "dead_end"),
        "bridge_count": sum(1 for g in group_records if g["category"] == "single_face_multi_port"),
        "multi_face_count": sum(1 for g in group_records if g["category"] == "multi_face"),
        "max_face_after": max_face_after,
        "face_I_after": face_ports_after["I"],
        "face_O_after": face_ports_after["O"],
        "face_L_after": face_ports_after["L"],
        "face_R_after": face_ports_after["R"],
    }

    return tile_summary, group_records


# ---------------------------------------------------------------------------
# Statistics helpers
# ---------------------------------------------------------------------------

def pct(n, total):
    return f"{n/max(total,1)*100:.1f}%"


def stats_line(vals, label=""):
    if not vals:
        return f"{label}(no data)"
    s = sorted(vals)
    n = len(s)
    return (f"{label}min={s[0]}, mean={statistics.mean(s):.1f}, "
            f"median={statistics.median(s):.1f}, max={s[-1]}, "
            f"p5={s[max(0,int(n*0.05))]}, p95={s[min(n-1,int(n*0.95))]}")


def mean_std(vals):
    if not vals:
        return "N/A"
    m = statistics.mean(vals)
    if len(vals) >= 2:
        sd = statistics.stdev(vals)
        return f"{m:.1f} +/- {sd:.1f}"
    return f"{m:.1f}"


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
                "overflow": int(row["overflow"]),
                "tower_j": int(row["tower_j"]),
            }
            if rec["overflow"] == 1:
                overflow_tiles.append(rec)
            else:
                non_overflow_tiles.append(rec)

    total_census = len(overflow_tiles) + len(non_overflow_tiles)
    print(f"Total tiles in census: {total_census}")
    print(f"  Overflow:     {len(overflow_tiles)}")
    print(f"  Non-overflow: {len(non_overflow_tiles)}")

    # --- Stratified sampling ---
    N_OVERFLOW = 500
    N_NON_OVERFLOW = 500

    # Uniform sample across tower range for overflow tiles
    overflow_tiles.sort(key=lambda r: (r["tower_j"], r["a_lo"], r["b_lo"]))
    if len(overflow_tiles) <= N_OVERFLOW:
        overflow_sample = overflow_tiles
    else:
        step = len(overflow_tiles) / N_OVERFLOW
        overflow_sample = [overflow_tiles[int(i * step)] for i in range(N_OVERFLOW)]

    # Uniform sample across tower range for non-overflow tiles
    non_overflow_tiles.sort(key=lambda r: (r["tower_j"], r["a_lo"], r["b_lo"]))
    if len(non_overflow_tiles) <= N_NON_OVERFLOW:
        non_overflow_sample = non_overflow_tiles
    else:
        step = len(non_overflow_tiles) / N_NON_OVERFLOW
        non_overflow_sample = [non_overflow_tiles[int(i * step)] for i in range(N_NON_OVERFLOW)]

    print(f"\nSampled: {len(overflow_sample)} overflow + {len(non_overflow_sample)} non-overflow")

    # --- Process all tiles ---
    all_sample = [(t, True) for t in overflow_sample] + [(t, False) for t in non_overflow_sample]
    total_tiles = len(all_sample)

    all_tile_summaries = []
    all_group_records = []

    t0 = time.time()
    for idx, (tile, is_ovf) in enumerate(all_sample):
        summary, groups = analyze_tile_groups(tile["a_lo"], tile["b_lo"], is_ovf)
        all_tile_summaries.append(summary)
        all_group_records.extend(groups)

        if (idx + 1) % 50 == 0 or idx == 0:
            elapsed = time.time() - t0
            rate = (idx + 1) / elapsed
            eta = (total_tiles - idx - 1) / rate
            print(f"  [{idx+1}/{total_tiles}] "
                  f"({elapsed:.1f}s, {rate:.1f} tiles/s, ETA {eta:.0f}s) "
                  f"tile ({tile['a_lo']}, {tile['b_lo']}): "
                  f"{len(groups)} groups, max_face={summary['max_face_after']}")

    t_total = time.time() - t0
    print(f"\nProcessing complete: {t_total:.1f}s ({total_tiles/t_total:.1f} tiles/s)")

    # --- Save CSV ---
    csv_fields = [
        "tile_a_lo", "tile_b_lo", "tile_overflow", "group_id",
        "num_faces", "total_ports", "face_I_ports", "face_O_ports",
        "face_L_ports", "face_R_ports", "primes_in_group",
        "category", "face_combination",
    ]
    with open(OUTPUT_CSV, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=csv_fields, extrasaction="ignore")
        writer.writeheader()
        for rec in all_group_records:
            writer.writerow(rec)
    print(f"Group-level CSV saved: {os.path.abspath(OUTPUT_CSV)}")

    # =====================================================================
    # STATISTICAL REPORT
    # =====================================================================

    ovf_summaries = [s for s in all_tile_summaries if s["is_overflow"]]
    novf_summaries = [s for s in all_tile_summaries if not s["is_overflow"]]
    ovf_groups = [g for g in all_group_records if g["tile_overflow"] == 1]
    novf_groups = [g for g in all_group_records if g["tile_overflow"] == 0]

    print(f"\n{'='*70}")
    print(f"=== GROUP STRUCTURE REPORT — R=860M, N={total_tiles} tiles ===")
    print(f"{'='*70}")

    # --- Section A: Tile-level stats ---
    print(f"\n--- A. Tile-level stats ---")
    print(f"Tiles analyzed: {total_tiles} ({len(ovf_summaries)} overflow, {len(novf_summaries)} non-overflow)")
    print(f"Groups per tile:     {stats_line([s['total_groups'] for s in all_tile_summaries])}")
    print(f"Primes per tile:     {stats_line([s['tile_primes'] for s in all_tile_summaries])}")
    print(f"Total ports before:  {stats_line([s['ports_before'] for s in all_tile_summaries])}")
    print(f"Total ports after:   {stats_line([s['ports_after'] for s in all_tile_summaries])}")

    # --- Section B: Group category breakdown ---
    print(f"\n--- B. Group category breakdown ---")
    total_groups = len(all_group_records)
    print(f"Total groups analyzed: {total_groups}")
    print()

    cats = ["dead_end", "single_face_multi_port", "multi_face"]
    cat_labels = {
        "dead_end": "dead_end",
        "single_face_multi_port": "single_face_multi_port",
        "multi_face": "multi_face",
    }

    print(f"{'Category':<30s} {'Count':>6s}  {'%':>6s}  {'Avg primes':>10s}  {'Avg ports':>10s}  {'Avg max-face-ports':>18s}")
    for cat in cats:
        gs = [g for g in all_group_records if g["category"] == cat]
        cnt = len(gs)
        if cnt == 0:
            print(f"{cat_labels[cat]:<30s} {cnt:>6d}  {pct(cnt, total_groups):>6s}  {'N/A':>10s}  {'N/A':>10s}  {'N/A':>18s}")
            continue
        avg_primes = statistics.mean([g["primes_in_group"] for g in gs])
        avg_ports = statistics.mean([g["total_ports"] for g in gs])
        avg_max_face = statistics.mean([g["max_ports_on_any_face"] for g in gs])
        print(f"{cat_labels[cat]:<30s} {cnt:>6d}  {pct(cnt, total_groups):>6s}  {avg_primes:>10.1f}  {avg_ports:>10.1f}  {avg_max_face:>18.1f}")

    # --- Section C: Multi-face group analysis ---
    print(f"\n--- C. Multi-face group analysis ---")
    mf_groups = [g for g in all_group_records if g["category"] == "multi_face"]
    print(f"Total multi-face groups: {len(mf_groups)}")

    print(f"\nFaces spanned distribution:")
    for nf in [2, 3, 4]:
        gs = [g for g in mf_groups if g["num_faces"] == nf]
        if not gs:
            print(f"  {nf} faces: 0 groups")
            continue
        avg_ports = statistics.mean([g["total_ports"] for g in gs])
        avg_primes = statistics.mean([g["primes_in_group"] for g in gs])
        print(f"  {nf} faces: {len(gs)} groups ({pct(len(gs), len(mf_groups))})  "
              f"-- avg ports {avg_ports:.1f}, avg primes {avg_primes:.1f}")

    print(f"\nMost common face combinations:")
    combo_counts = Counter(g["face_combination"] for g in mf_groups)
    for combo, cnt in combo_counts.most_common(15):
        print(f"  {combo:<12s}: {cnt:5d}  ({pct(cnt, len(mf_groups))})")

    # --- Section D: Single-face multi-port groups (bridge groups) ---
    print(f"\n--- D. Single-face multi-port group analysis (bridge groups) ---")
    bridge_groups = [g for g in all_group_records if g["category"] == "single_face_multi_port"]
    print(f"Total bridge groups: {len(bridge_groups)}")

    if bridge_groups:
        bp = [g["total_ports"] for g in bridge_groups]
        bpr = [g["primes_in_group"] for g in bridge_groups]
        print(f"Ports per group:     {stats_line(bp)}")
        print(f"Primes per group:    {stats_line(bpr)}")

        print(f"\nDistribution by face:")
        for face in FACES:
            face_bridge = [g for g in bridge_groups
                           if g[f"face_{face}_ports"] > 0]
            if face_bridge:
                avg_p = statistics.mean([g["total_ports"] for g in face_bridge])
                print(f"  Face {face}: {len(face_bridge)} groups, avg ports {avg_p:.1f}")
            else:
                print(f"  Face {face}: 0 groups")

        print(f"\nPorts-per-group histogram:")
        port_hist = Counter(g["total_ports"] for g in bridge_groups)
        for p in sorted(port_hist.keys()):
            label = f"{p}" if p < 5 else f"{p}"
            print(f"  {label:>3s} ports: {port_hist[p]:5d}")

    # --- Section E: Port budget analysis ---
    print(f"\n--- E. Port budget analysis -- who's eating the 16 slots ---")

    # Per-face port contributions by category
    # We need per-face data, so we look at the per-face port counts in each group
    face_contrib = {cat: {f: [] for f in FACES} for cat in cats}

    for s in all_tile_summaries:
        a_lo = s["a_lo"]
        b_lo = s["b_lo"]
        tile_groups = [g for g in all_group_records
                       if g["tile_a_lo"] == a_lo and g["tile_b_lo"] == b_lo]
        for f in FACES:
            for cat in cats:
                cat_port_sum = sum(g[f"face_{f}_ports"] for g in tile_groups
                                   if g["category"] == cat)
                face_contrib[cat][f].append(cat_port_sum)

    print(f"\nPer face, average port contribution by group category:")
    for cat in cats:
        avg_per_face = statistics.mean(
            [statistics.mean(face_contrib[cat][f]) for f in FACES]
        ) if any(face_contrib[cat][f] for f in FACES) else 0
        print(f"  {cat:<30s}: {avg_per_face:.1f}")

    # After pruning breakdown
    print(f"\nAfter pruning, average ports per face:")
    all_face_after = []
    from_bridge = []
    from_multi = []
    for s in all_tile_summaries:
        a_lo = s["a_lo"]
        b_lo = s["b_lo"]
        tile_groups = [g for g in all_group_records
                       if g["tile_a_lo"] == a_lo and g["tile_b_lo"] == b_lo]
        for f in FACES:
            surviving_ports = sum(g[f"face_{f}_ports"] for g in tile_groups
                                  if g["category"] != "dead_end")
            bridge_ports = sum(g[f"face_{f}_ports"] for g in tile_groups
                               if g["category"] == "single_face_multi_port")
            multi_ports = sum(g[f"face_{f}_ports"] for g in tile_groups
                              if g["category"] == "multi_face")
            all_face_after.append(surviving_ports)
            from_bridge.append(bridge_ports)
            from_multi.append(multi_ports)

    avg_total = statistics.mean(all_face_after)
    avg_bridge = statistics.mean(from_bridge)
    avg_multi = statistics.mean(from_multi)
    print(f"  Average ports per face after pruning: {avg_total:.1f}")
    print(f"    from single_face_multi_port: {avg_bridge:.1f} ({avg_bridge/max(avg_total,0.01)*100:.1f}%)")
    print(f"    from multi_face:             {avg_multi:.1f} ({avg_multi/max(avg_total,0.01)*100:.1f}%)")

    # --- Section F: The mega-group phenomenon ---
    print(f"\n--- F. The mega-group phenomenon ---")

    largest_primes = []
    largest_faces = []
    largest_ports = []
    tiles_largest_gt50pct = 0
    tiles_with_4face_group = 0

    for s in all_tile_summaries:
        a_lo = s["a_lo"]
        b_lo = s["b_lo"]
        tile_groups = [g for g in all_group_records
                       if g["tile_a_lo"] == a_lo and g["tile_b_lo"] == b_lo]
        if not tile_groups:
            continue

        # Largest by prime count
        largest = max(tile_groups, key=lambda g: g["primes_in_group"])
        largest_primes.append(largest["primes_in_group"])
        largest_faces.append(largest["num_faces"])
        largest_ports.append(largest["total_ports"])

        # Does largest group have >50% of surviving ports?
        surviving_total = sum(g["total_ports"] for g in tile_groups
                              if g["category"] != "dead_end")
        largest_surviving_ports = largest["total_ports"] if largest["category"] != "dead_end" else 0
        if surviving_total > 0 and largest_surviving_ports > surviving_total * 0.5:
            tiles_largest_gt50pct += 1

        # Any group spans all 4 faces?
        if any(g["num_faces"] == 4 for g in tile_groups):
            tiles_with_4face_group += 1

    print(f"Largest group per tile (by prime count):")
    print(f"  min primes: {min(largest_primes)}, mean: {statistics.mean(largest_primes):.1f}, max: {max(largest_primes)}")
    print(f"  avg faces spanned: {statistics.mean(largest_faces):.1f}")
    print(f"  avg total ports: {statistics.mean(largest_ports):.1f}")
    print()
    print(f"Tiles where largest group has >50% of surviving ports: "
          f"{tiles_largest_gt50pct}/{total_tiles} ({pct(tiles_largest_gt50pct, total_tiles)})")
    print(f"Tiles where one group spans all 4 faces: "
          f"{tiles_with_4face_group}/{total_tiles} ({pct(tiles_with_4face_group, total_tiles)})")

    # --- Section G: Overflow vs non-overflow comparison ---
    print(f"\n--- G. Overflow vs non-overflow comparison ---")
    print(f"{'':>35s} {'Overflow tiles':>18s}    {'Non-overflow tiles':>18s}")

    metrics = [
        ("Groups per tile",
         [s["total_groups"] for s in ovf_summaries],
         [s["total_groups"] for s in novf_summaries]),
        ("Dead ends per tile",
         [s["dead_end_count"] for s in ovf_summaries],
         [s["dead_end_count"] for s in novf_summaries]),
        ("Bridge groups per tile",
         [s["bridge_count"] for s in ovf_summaries],
         [s["bridge_count"] for s in novf_summaries]),
        ("Multi-face per tile",
         [s["multi_face_count"] for s in ovf_summaries],
         [s["multi_face_count"] for s in novf_summaries]),
        ("Avg max face ports",
         [s["max_face_after"] for s in ovf_summaries],
         [s["max_face_after"] for s in novf_summaries]),
        ("Ports before (total)",
         [s["ports_before"] for s in ovf_summaries],
         [s["ports_before"] for s in novf_summaries]),
        ("Ports after (total)",
         [s["ports_after"] for s in ovf_summaries],
         [s["ports_after"] for s in novf_summaries]),
    ]

    for label, ovf_vals, novf_vals in metrics:
        print(f"{label:>35s} {mean_std(ovf_vals):>18s}    {mean_std(novf_vals):>18s}")

    # Per-face average surviving ports, overflow vs not
    print()
    print(f"{'Per-face avg surviving ports':>35s}")
    for f in FACES:
        ovf_face = [s[f"face_{f}_after"] for s in ovf_summaries]
        novf_face = [s[f"face_{f}_after"] for s in novf_summaries]
        print(f"{'  Face ' + f:>35s} {mean_std(ovf_face):>18s}    {mean_std(novf_face):>18s}")

    # Bridge port contribution comparison
    ovf_bridge_ports_per_tile = []
    novf_bridge_ports_per_tile = []
    for s in all_tile_summaries:
        a_lo = s["a_lo"]
        b_lo = s["b_lo"]
        tile_groups = [g for g in all_group_records
                       if g["tile_a_lo"] == a_lo and g["tile_b_lo"] == b_lo]
        bridge_total = sum(g["total_ports"] for g in tile_groups
                           if g["category"] == "single_face_multi_port")
        if s["is_overflow"]:
            ovf_bridge_ports_per_tile.append(bridge_total)
        else:
            novf_bridge_ports_per_tile.append(bridge_total)

    print()
    print(f"{'Avg ports from bridges (total)':>35s} {mean_std(ovf_bridge_ports_per_tile):>18s}    {mean_std(novf_bridge_ports_per_tile):>18s}")

    print(f"\n{'='*70}")
    print(f"Done.")


if __name__ == "__main__":
    main()
