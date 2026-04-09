"""Sample runner for operating-point tile validation."""

from __future__ import annotations

from ports import FACES, FacePrime, Port, prune_ports
from tile import (
    BACKWARD_OFFSETS,
    COLLAR,
    DEFAULT_K,
    SIDE_EXP,
    TILE_SIDE,
    compact_bitmap,
    process_tile,
    sieve_tile_bitmap,
    tile_interior_prime_positions,
)
from uf import build_components, make_backward_offsets
from validate import validate_pair, validate_tile


TARGETS = [
    ("45deg", 601040640, 601040640),
    ("30deg", 736121088, 424999936),
    ("15deg", 820888320, 220000000),
]

EXPECTED = {
    "45deg": {
        "prime_count": 2040,
        "group_count": 11,
        "ports_before_pruning": 70,
        "ports_after_pruning": 54,
        "per_face": {"I": 12, "O": 15, "L": 12, "R": 15},
    },
    "30deg": {
        "prime_count": 2055,
        "group_count": 16,
        "ports_before_pruning": 75,
        "ports_after_pruning": 64,
        "per_face": {"I": 18, "O": 19, "L": 16, "R": 11},
    },
    "15deg": {
        "prime_count": 2013,
        "group_count": 6,
        "ports_before_pruning": 59,
        "ports_after_pruning": 51,
        "per_face": {"I": 14, "O": 12, "L": 14, "R": 11},
    },
}


def _distance_sq(a: FacePrime, b: FacePrime) -> int:
    dr = a.row - b.row
    dc = a.col - b.col
    return dr * dr + dc * dc


def _legacy_connected_to_cluster(prime: FacePrime, cluster: list[FacePrime], k_sq: int) -> bool:
    max_delta_h = int(k_sq**0.5)
    for other in reversed(cluster):
        if prime.h - other.h > max_delta_h:
            break
        if _distance_sq(prime, other) <= k_sq:
            return True
    return False


def _legacy_cluster_face_primes(face_primes: list[FacePrime], k_sq: int) -> list[list[FacePrime]]:
    if not face_primes:
        return []

    clusters: list[list[FacePrime]] = []
    current: list[FacePrime] = []
    for face_prime in face_primes:
        if not current or _legacy_connected_to_cluster(face_prime, current, k_sq):
            current.append(face_prime)
            continue
        clusters.append(current)
        current = [face_prime]
    if current:
        clusters.append(current)
    return clusters


def _legacy_collect_face_primes(
    a_lo: int,
    b_lo: int,
    prime_pos: list[int],
    side_exp: int,
    tile_side: int,
    collar: int,
) -> dict[str, list[FacePrime]]:
    faces: dict[str, list[FacePrime]] = {face: [] for face in FACES}
    base_a = a_lo - collar
    base_b = b_lo - collar

    for prime_index, pos in enumerate(prime_pos):
        row = pos // side_exp
        col = pos % side_exp
        tile_row = row - collar
        tile_col = col - collar
        if not (0 <= tile_row <= tile_side and 0 <= tile_col <= tile_side):
            continue

        a = base_a + row
        b = base_b + col

        if tile_col < collar:
            faces["I"].append(
                FacePrime(prime_index, row, col, a, b, tile_row, tile_col)
            )
        if tile_col >= tile_side - collar + 1:
            faces["O"].append(
                FacePrime(prime_index, row, col, a, b, tile_row, tile_side - tile_col)
            )
        if tile_row < collar:
            faces["L"].append(
                FacePrime(prime_index, row, col, a, b, tile_col, tile_row)
            )
        if tile_row >= tile_side - collar + 1:
            faces["R"].append(
                FacePrime(prime_index, row, col, a, b, tile_col, tile_side - tile_row)
            )

    for primes in faces.values():
        primes.sort(key=lambda p: (p.h, p.depth, p.row, p.col, p.prime_index))
    return faces


def _assign_groups_with_cluster(
    face_primes: dict[str, list[FacePrime]],
    component_roots: list[int],
    k_sq: int,
    cluster_face_primes,
) -> dict[str, list[Port]]:
    ports_by_face: dict[str, list[Port]] = {face: [] for face in FACES}
    root_to_group: dict[int, int] = {}
    next_group = 1

    for face in FACES:
        for cluster in cluster_face_primes(face_primes[face], k_sq):
            root = component_roots[cluster[0].prime_index]
            if root not in root_to_group:
                root_to_group[root] = next_group
                next_group += 1
            ports_by_face[face].append(
                Port(
                    face=face,
                    h1=min(p.h for p in cluster),
                    group=root_to_group[root],
                    prime_indices=[p.prime_index for p in cluster],
                    primes=[(p.a, p.b) for p in cluster],
                    component_root=root,
                )
            )
        ports_by_face[face].sort(key=lambda port: (port.h1, port.prime_indices[0]))

    return ports_by_face


def _run_pipeline(
    a_lo: int,
    b_lo: int,
    k_sq: int,
    collect_fn,
    cluster_fn,
) -> dict[str, object]:
    bitmap = sieve_tile_bitmap(a_lo, b_lo)
    _, prefix, prime_pos = compact_bitmap(bitmap)
    tile_prime_pos = tile_interior_prime_positions(prime_pos)
    component_roots = build_components(
        bitmap=bitmap,
        prefix=prefix,
        prime_pos=prime_pos,
        side_exp=SIDE_EXP,
        k_sq=k_sq,
        backward_offsets=BACKWARD_OFFSETS if k_sq == DEFAULT_K else make_backward_offsets(k_sq),
    )

    face_primes = collect_fn(
        a_lo=a_lo,
        b_lo=b_lo,
        prime_pos=prime_pos,
        side_exp=SIDE_EXP,
        tile_side=TILE_SIDE,
        collar=COLLAR,
    )
    ports_before = _assign_groups_with_cluster(face_primes, component_roots, k_sq, cluster_fn)
    ports_after, _ = prune_ports(ports_before)

    return {
        "prime_count": len(tile_prime_pos),
        "ports_before_pruning": sum(len(ports) for ports in ports_before.values()),
        "ports_after_pruning": sum(len(ports) for ports in ports_after.values()),
        "per_face": {face: len(ports_after[face]) for face in FACES},
    }


def _face_counts(result: dict) -> dict[str, int]:
    return {face: len(result["ports"][face]) for face in FACES}


def _format_face_counts(counts: dict[str, int]) -> str:
    return ", ".join(f"{face}={counts[face]}" for face in FACES)


def main():
    for label, a_lo, b_lo in TARGETS:
        expected = EXPECTED[label]
        fixed = process_tile(a_lo, b_lo, DEFAULT_K)
        legacy = _run_pipeline(
            a_lo=a_lo,
            b_lo=b_lo,
            k_sq=DEFAULT_K,
            collect_fn=_legacy_collect_face_primes,
            cluster_fn=_legacy_cluster_face_primes,
        )
        fixed_face_counts = _face_counts(fixed)

        print(f"{label} ({a_lo},{b_lo})")
        print(f"  prime_count={fixed['prime_count']} expected={expected['prime_count']}")
        print(f"  group_count={fixed['group_count']} expected={expected['group_count']}")
        print(
            "  ports_before: "
            f"legacy={legacy['ports_before_pruning']} "
            f"fixed={fixed['ports_before_pruning']} "
            f"expected={expected['ports_before_pruning']}"
        )
        print(
            "  ports_after:  "
            f"legacy={legacy['ports_after_pruning']} "
            f"fixed={fixed['ports_after_pruning']} "
            f"expected={expected['ports_after_pruning']}"
        )
        print(
            "  per_face_fixed: "
            f"{_format_face_counts(fixed_face_counts)} "
            f"expected={_format_face_counts(expected['per_face'])}"
        )
        print(
            f"  status={fixed['tileop_status']} overflow={fixed['overflow']} "
            f"offsets={fixed['tileop_offsets']} counts={fixed['tileop_counts']}"
        )
        for check in validate_tile(a_lo, b_lo, DEFAULT_K):
            print(f"  {check}")
        for check in validate_pair(a_lo, b_lo, a_lo + TILE_SIDE, b_lo, DEFAULT_K):
            print(f"  {check}")
        print()


if __name__ == "__main__":
    main()
