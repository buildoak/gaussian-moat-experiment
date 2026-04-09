#!/usr/bin/env python3
"""Full structural diagnostic of a single tile's face-port-group topology.

Usage: python3 diagnose_tile.py <a_lo> <b_lo>

Produces a complete breakdown of primes, components, face primes, port clustering,
group-face incidence, dead-end identification, and pruning effectiveness.
"""

from __future__ import annotations

import sys
import os
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
MAX_PORTS_PER_FACE = 16
BACKWARD_OFFSETS = make_backward_offsets(K_SQ)


def sieve_bitmap(a_lo: int, b_lo: int) -> list[int]:
    """Phase 1: Build the 271x271 prime bitmap."""
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
    """Phase 2: Prefix popcount + dense prime positions."""
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
    """Phase 3: Union-Find connected components."""
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


def pos_to_tile_coords(pos):
    """Convert bitmap position to (tile_row, tile_col)."""
    row = pos // SIDE_EXP
    col = pos % SIDE_EXP
    return row - COLLAR, col - COLLAR


def collect_face_primes(prime_pos, a_lo, b_lo):
    """Phase 4a: Collect face primes with full metadata."""
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

        # Face I (bottom): tile_row < COLLAR, h = tile_col
        if tile_row < COLLAR:
            faces["I"].append({
                "prime_index": prime_index, "row": row, "col": col,
                "a": a, "b": b, "h": tile_col, "depth": tile_row,
            })
        # Face O (top): tile_row >= TILE_SIDE - COLLAR + 1, h = tile_col
        if tile_row >= TILE_SIDE - COLLAR + 1:
            faces["O"].append({
                "prime_index": prime_index, "row": row, "col": col,
                "a": a, "b": b, "h": tile_col, "depth": TILE_SIDE - tile_row,
            })
        # Face L (left): tile_col < COLLAR, h = tile_row
        if tile_col < COLLAR:
            faces["L"].append({
                "prime_index": prime_index, "row": row, "col": col,
                "a": a, "b": b, "h": tile_row, "depth": tile_col,
            })
        # Face R (right): tile_col >= TILE_SIDE - COLLAR + 1, h = tile_row
        if tile_col >= TILE_SIDE - COLLAR + 1:
            faces["R"].append({
                "prime_index": prime_index, "row": row, "col": col,
                "a": a, "b": b, "h": tile_row, "depth": TILE_SIDE - tile_col,
            })

    # Sort each face by (h, depth, row, col, prime_index) — matches Python validator
    for f in FACES:
        faces[f].sort(key=lambda p: (p["h"], p["depth"], p["row"], p["col"], p["prime_index"]))
    return faces


def cluster_into_ports(face_primes, face, component_roots):
    """Phase 4b: Cluster consecutive face primes into ports using dist^2 <= K_SQ rule."""
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
            # Finalize previous port
            root = component_roots[current_cluster[0]["prime_index"]]
            ports.append({
                "face": face,
                "h1": min(p["h"] for p in current_cluster),
                "component_root": root,
                "primes": current_cluster,
                "size": len(current_cluster),
            })
            current_cluster = [curr]

    # Finalize last port
    root = component_roots[current_cluster[0]["prime_index"]]
    ports.append({
        "face": face,
        "h1": min(p["h"] for p in current_cluster),
        "component_root": root,
        "primes": current_cluster,
        "size": len(current_cluster),
    })
    return ports


def assign_groups(all_ports_by_face):
    """Assign 1-based group numbers in I->O->L->R scan order."""
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
        # Sort by (h1, first prime index)
        result[face].sort(key=lambda p: (p["h1"], p["primes"][0]["prime_index"]))

    return result, root_to_group, next_group - 1


def compute_group_face_incidence(ports_by_face):
    """Build the group -> {face: port_count} incidence matrix."""
    incidence = defaultdict(lambda: defaultdict(int))
    for face in FACES:
        for port in ports_by_face[face]:
            incidence[port["group"]][face] += 1
    return incidence


def identify_dead_ends(incidence):
    """A group is a dead end if it appears on exactly 1 face in exactly 1 port."""
    dead_ends = set()
    for group, face_counts in incidence.items():
        total_faces = len(face_counts)
        total_ports = sum(face_counts.values())
        if total_faces == 1 and total_ports == 1:
            dead_ends.add(group)
    return dead_ends


def prune(ports_by_face, dead_ends):
    """Remove dead-end groups, renumber survivors."""
    remap = {}
    next_group = 1
    result = {f: [] for f in FACES}
    for face in FACES:
        for port in ports_by_face[face]:
            if port["group"] in dead_ends:
                continue
            if port["group"] not in remap:
                remap[port["group"]] = next_group
                next_group += 1
            port_copy = dict(port)
            port_copy["group"] = remap[port["group"]]
            result[face].append(port_copy)
    return result, remap, next_group - 1


def diagnose(a_lo: int, b_lo: int):
    """Run full diagnostic on a single tile."""
    print(f"\n{'='*70}")
    print(f"=== Tile (a_lo={a_lo}, b_lo={b_lo}) ===")
    print(f"{'='*70}")
    print(f"Primality backend: {PRIME_BACKEND}")

    # Phase 1: Sieve
    print("\n--- Phase 1: Sieve (271x271 domain) ---")
    bitmap = sieve_bitmap(a_lo, b_lo)
    total_primes_in_domain, prefix, prime_pos = compact_bitmap(bitmap)
    print(f"Primes in sieve domain (271x271): {total_primes_in_domain}")

    # Count primes in tile proper
    tile_proper_count = 0
    for pos in prime_pos:
        tr, tc = pos_to_tile_coords(pos)
        if 0 <= tr <= TILE_SIDE and 0 <= tc <= TILE_SIDE:
            tile_proper_count += 1
    print(f"Primes in tile proper (257x257):  {tile_proper_count}")

    # Phase 3: Connected components
    print("\n--- Phase 3: Connected components ---")
    roots = build_components(bitmap, prefix, prime_pos, total_primes_in_domain)
    unique_roots = set(roots)
    print(f"Connected components (full domain): {len(unique_roots)}")

    # Phase 4a: Face primes
    print("\n--- Phase 4a: Face primes ---")
    face_primes = collect_face_primes(prime_pos, a_lo, b_lo)
    for f in FACES:
        face_name = {"I": "I (bottom, b=b_lo)", "O": "O (top, b=b_lo+256)",
                     "L": "L (left, a=a_lo)", "R": "R (right, a=a_lo+256)"}[f]
        print(f"  Face {face_name}: {len(face_primes[f]):3d} primes")

    # Phase 4b: Port clustering
    print("\n--- Phase 4b: Port clustering (before pruning) ---")
    raw_ports = {f: [] for f in FACES}
    for f in FACES:
        raw_ports[f] = cluster_into_ports(face_primes[f], f, roots)

    ports_by_face, root_to_group, total_groups = assign_groups(raw_ports)
    total_ports_before = sum(len(ports_by_face[f]) for f in FACES)

    for f in FACES:
        ports = ports_by_face[f]
        sizes = [p["size"] for p in ports]
        if len(ports) >= 2:
            gaps = []
            for i in range(1, len(ports)):
                prev_last = ports[i-1]["primes"][-1]
                curr_first = ports[i]["primes"][0]
                dx = curr_first["col"] - prev_last["col"]
                dy = curr_first["row"] - prev_last["row"]
                gaps.append(dx * dx + dy * dy)
            print(f"  Face {f}: {len(ports):2d} ports  sizes={sizes}  gaps={gaps}")
        else:
            print(f"  Face {f}: {len(ports):2d} ports  sizes={sizes}")
    print(f"  Total: {total_ports_before} ports across {total_groups} groups")

    # Group-face incidence matrix
    print("\n--- Group-face incidence ---")
    incidence = compute_group_face_incidence(ports_by_face)
    dead_ends = identify_dead_ends(incidence)

    multi_face_count = 0
    single_face_multi_port_count = 0
    dead_end_count = 0

    for group in sorted(incidence.keys()):
        face_counts = incidence[group]
        faces_touched = len(face_counts)
        total_ports_for_group = sum(face_counts.values())

        parts = []
        for f in FACES:
            if f in face_counts:
                parts.append(f"{f}({face_counts[f]} port{'s' if face_counts[f]>1 else ''})")

        if group in dead_ends:
            label = "DEAD END"
            dead_end_count += 1
        elif faces_touched >= 2:
            label = f"spans {faces_touched} faces"
            multi_face_count += 1
        else:
            label = "single-face, multi-port"
            single_face_multi_port_count += 1

        print(f"  Group {group:2d}: {', '.join(parts):40s} -> {label}")

    print(f"\nSummary: {total_groups} groups total")
    print(f"  Multi-face:              {multi_face_count} ({multi_face_count/max(total_groups,1)*100:.1f}%)")
    print(f"  Single-face, multi-port: {single_face_multi_port_count} ({single_face_multi_port_count/max(total_groups,1)*100:.1f}%)")
    print(f"  Dead ends (prunable):    {dead_end_count} ({dead_end_count/max(total_groups,1)*100:.1f}%)")

    # Pruning
    print("\n--- Pruning ---")
    ports_removed = 0
    for f in FACES:
        for p in ports_by_face[f]:
            if p["group"] in dead_ends:
                ports_removed += 1

    pruned_ports, remap, new_group_count = prune(ports_by_face, dead_ends)
    total_ports_after = sum(len(pruned_ports[f]) for f in FACES)

    print(f"Dead-end groups: {sorted(dead_ends)} ({dead_end_count} out of {total_groups})")
    print(f"Pruning removes: {ports_removed} ports ({ports_removed/max(total_ports_before,1)*100:.1f}%)")

    print("\n--- After pruning ---")
    max_face_ports = 0
    for f in FACES:
        count = len(pruned_ports[f])
        max_face_ports = max(max_face_ports, count)
        print(f"  Face {f}: {count:2d} ports")
    print(f"  Total: {total_ports_after} ports")
    print(f"  Groups: {new_group_count}")
    print(f"  Max face ports: {max_face_ports}  -> {'OVERFLOW' if max_face_ports > MAX_PORTS_PER_FACE else 'OK'}")

    return {
        "a_lo": a_lo,
        "b_lo": b_lo,
        "tile_primes": tile_proper_count,
        "domain_primes": total_primes_in_domain,
        "components": len(unique_roots),
        "groups": total_groups,
        "ports_before": total_ports_before,
        "ports_after": total_ports_after,
        "dead_ends": dead_end_count,
        "pruning_pct": ports_removed / max(total_ports_before, 1) * 100,
        "max_face_after": max_face_ports,
        "overflow": max_face_ports > MAX_PORTS_PER_FACE,
    }


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <a_lo> <b_lo>")
        sys.exit(1)

    a_lo = int(sys.argv[1])
    b_lo = int(sys.argv[2])
    diagnose(a_lo, b_lo)


if __name__ == "__main__":
    main()
