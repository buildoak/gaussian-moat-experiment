"""Phase 4 and Phase 5 helpers: face extraction, port clustering, pruning, encode."""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass

from analysis import compute_group_face_incidence, identify_dead_ends, overflow_reason, packed_budget_counts
from tileop import encode_lr_group_byte, encode_lr_h1_byte

FACES = ("I", "O", "L", "R")
GROUP_EMPTY = 0


@dataclass
class FacePrime:
    prime_index: int
    row: int
    col: int
    a: int
    b: int
    h: int
    depth: int


@dataclass
class Port:
    face: str
    h1: int
    group: int
    prime_indices: list[int]
    primes: list[tuple[int, int]]
    component_root: int

    @property
    def prime_count(self) -> int:
        return len(self.prime_indices)

    def as_dict(self) -> dict:
        return {
            "face": self.face,
            "h1": self.h1,
            "group": self.group,
            "prime_indices": list(self.prime_indices),
            "primes": list(self.primes),
        }


def _distance_sq(a: FacePrime, b: FacePrime) -> int:
    dr = a.row - b.row
    dc = a.col - b.col
    return dr * dr + dc * dc


def _cluster_face_primes(face_primes: list[FacePrime], k_sq: int) -> list[list[FacePrime]]:
    if not face_primes:
        return []

    clusters: list[list[FacePrime]] = [[face_primes[0]]]
    for i in range(1, len(face_primes)):
        face_prime = face_primes[i]
        if _distance_sq(face_prime, face_primes[i - 1]) <= k_sq:
            clusters[-1].append(face_prime)
            continue
        clusters.append([face_prime])
    return clusters


def collect_face_primes(
    a_lo: int,
    b_lo: int,
    prime_pos: list[int],
    side_exp: int,
    tile_side: int,
    collar: int,
) -> dict[str, list[FacePrime]]:
    """Collect face primes that are inside the tile proper and within the collar."""

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

        if tile_row < collar:
            faces["I"].append(
                FacePrime(prime_index, row, col, a, b, tile_col, tile_row)
            )
        if tile_row >= tile_side - collar + 1:
            faces["O"].append(
                FacePrime(prime_index, row, col, a, b, tile_col, tile_side - tile_row)
            )
        if tile_col < collar:
            faces["L"].append(
                FacePrime(prime_index, row, col, a, b, tile_row, tile_col)
            )
        if tile_col >= tile_side - collar + 1:
            faces["R"].append(
                FacePrime(prime_index, row, col, a, b, tile_row, tile_side - tile_col)
            )

    for primes in faces.values():
        primes.sort(key=lambda p: (p.h, p.depth, p.row, p.col, p.prime_index))
    return faces


def assign_groups(
    face_primes: dict[str, list[FacePrime]],
    component_roots: list[int],
    k_sq: int,
) -> tuple[dict[str, list[Port]], dict[int, int]]:
    """Assign 1-based groups in spec scan order before pruning."""

    ports_by_face: dict[str, list[Port]] = {face: [] for face in FACES}
    root_to_group: dict[int, int] = {}
    next_group = 1

    for face in FACES:
        for cluster in _cluster_face_primes(face_primes[face], k_sq):
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

    return ports_by_face, root_to_group


def prune_ports(ports_by_face: dict[str, list[Port]]) -> tuple[dict[str, list[Port]], dict[int, int]]:
    """Prune dead-end groups and renumber surviving groups contiguously from 1."""

    incidence = compute_group_face_incidence(ports_to_public_dict(ports_by_face))
    pruned_groups = identify_dead_ends(incidence)

    remap: dict[int, int] = {}
    next_group = 1
    result: dict[str, list[Port]] = {face: [] for face in FACES}

    for face in FACES:
        for port in ports_by_face[face]:
            if port.group in pruned_groups:
                continue
            if port.group not in remap:
                remap[port.group] = next_group
                next_group += 1
            result[face].append(
                Port(
                    face=port.face,
                    h1=port.h1,
                    group=remap[port.group],
                    prime_indices=list(port.prime_indices),
                    primes=list(port.primes),
                    component_root=port.component_root,
                )
            )
    return result, remap


def encode_tileop(
    ports_by_face: dict[str, list[Port]],
    group_count: int,
) -> tuple[bytes, bool]:
    public_ports = ports_to_public_dict(ports_by_face)
    budget = packed_budget_counts(public_ports)
    if overflow_reason(public_ports, group_count) is not None:
        return bytes([0xFF] * 128), True

    tileop = bytearray(128)
    off_R = 3 + budget["o_cnt"] + budget["i_cnt"] + budget["l_cnt"]
    tileop[0] = 3 + budget["o_cnt"]
    tileop[1] = tileop[0] + budget["i_cnt"]
    tileop[2] = off_R

    # Write group labels for O, I, L, R
    cursor = 3
    for face in ("O", "I"):
        for port in ports_by_face[face]:
            tileop[cursor] = port.group
            cursor += 1
    for face in ("L", "R"):
        for port in ports_by_face[face]:
            tileop[cursor] = encode_lr_group_byte(port.group, port.h1)
            cursor += 1

    derived_r_cnt = (128 - off_R - budget["l_cnt"]) // 2
    cursor = off_R + derived_r_cnt
    for face in ("L", "R"):
        for port in ports_by_face[face]:
            tileop[cursor] = encode_lr_h1_byte(port.h1)
            cursor += 1

    return bytes(tileop), False


def ports_to_public_dict(ports_by_face: dict[str, list[Port]]) -> dict[str, list[dict]]:
    return {face: [port.as_dict() for port in ports_by_face[face]] for face in FACES}
