"""Spec-faithful tile validator pipeline.

This validator uses the authoritative tile specs' phase boundaries and emits
the canonical 128-byte TileOp. Primality testing remains delegated to
`primes.py`; the alignment work here is about phase structure and encoding.
"""

from __future__ import annotations

from primes import is_gaussian_prime
from uf import build_components, make_backward_offsets
from analysis import packed_budget_counts
from tileop import decode_tileop, parse_tileop

import math as _math

TILE_SIDE = 256
S = TILE_SIDE
DEFAULT_K = 40
COLLAR = _math.ceil(_math.sqrt(DEFAULT_K))
SIDE_EXP = TILE_SIDE + 2 * COLLAR + 1
HALO = SIDE_EXP
BIT_WORD_BITS = 32
BIT_WORD_COUNT = (SIDE_EXP * SIDE_EXP + BIT_WORD_BITS - 1) // BIT_WORD_BITS
BACKWARD_OFFSETS = make_backward_offsets(DEFAULT_K)


def coords_to_pos(a: int, b: int, a_lo: int, b_lo: int) -> int:
    row = a - (a_lo - COLLAR)
    col = b - (b_lo - COLLAR)
    return row * SIDE_EXP + col


def pos_to_coords(pos: int, a_lo: int, b_lo: int) -> tuple[int, int]:
    row = pos // SIDE_EXP
    col = pos % SIDE_EXP
    return a_lo - COLLAR + row, b_lo - COLLAR + col


def bitmap_test(bitmap: list[int], pos: int) -> bool:
    return bool(bitmap[pos >> 5] & (1 << (pos & 31)))


def sieve_tile_bitmap(a_lo: int, b_lo: int) -> list[int]:
    """Phase 1: emit the raw 1=prime bitmap over the expanded 271x271 domain."""

    bitmap = [0] * BIT_WORD_COUNT
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
    """Phase 2: prefix popcount and dense compacted prime positions."""

    counts = [word.bit_count() for word in bitmap]
    prefix = [0] * len(bitmap)
    running = 0
    for i, count in enumerate(counts):
        prefix[i] = running
        running += count

    prime_pos: list[int] = [0] * running
    for word_index, word in enumerate(bitmap):
        idx = prefix[word_index]
        cursor = word
        while cursor:
            bit = (cursor & -cursor).bit_length() - 1
            prime_pos[idx] = word_index * BIT_WORD_BITS + bit
            idx += 1
            cursor &= cursor - 1

    return running, prefix, prime_pos


def tile_interior_prime_positions(prime_pos: list[int]) -> list[int]:
    """Return compact positions that lie inside the tile proper."""

    interior: list[int] = []
    for pos in prime_pos:
        row = pos // SIDE_EXP
        col = pos % SIDE_EXP
        tile_row = row - COLLAR
        tile_col = col - COLLAR
        if 0 <= tile_row <= TILE_SIDE and 0 <= tile_col <= TILE_SIDE:
            interior.append(pos)
    return interior


def adjacent(p1: tuple[int, int], p2: tuple[int, int], k_sq: int = DEFAULT_K) -> bool:
    da = p1[0] - p2[0]
    db = p1[1] - p2[1]
    return da * da + db * db <= k_sq


def _finalize_prime_groups(
    prime_count: int,
    component_roots: list[int],
    pre_prune_root_to_group: dict[int, int],
    final_group_remap: dict[int, int],
) -> list[int]:
    groups = [0] * prime_count
    for prime_index, root in enumerate(component_roots):
        initial_group = pre_prune_root_to_group.get(root)
        if initial_group is None:
            continue
        groups[prime_index] = final_group_remap.get(initial_group, 0)
    return groups


def process_tile(a_lo: int, b_lo: int, k_sq: int = DEFAULT_K) -> dict:
    """Run the full 5-phase spec pipeline for one tile."""

    from ports import assign_groups, collect_face_primes, encode_tileop, ports_to_public_dict, prune_ports

    bitmap = sieve_tile_bitmap(a_lo, b_lo)
    bitmap_prime_count, prefix, prime_pos = compact_bitmap(bitmap)
    tile_prime_pos = tile_interior_prime_positions(prime_pos)
    component_roots = build_components(
        bitmap=bitmap,
        prefix=prefix,
        prime_pos=prime_pos,
        side_exp=SIDE_EXP,
        k_sq=k_sq,
        backward_offsets=BACKWARD_OFFSETS if k_sq == DEFAULT_K else make_backward_offsets(k_sq),
    )

    face_primes = collect_face_primes(
        a_lo=a_lo,
        b_lo=b_lo,
        prime_pos=prime_pos,
        side_exp=SIDE_EXP,
        tile_side=TILE_SIDE,
        collar=COLLAR,
    )
    ports_before_pruning, root_to_group = assign_groups(
        face_primes=face_primes,
        component_roots=component_roots,
        k_sq=k_sq,
    )
    raw_port_count = sum(len(ports) for ports in ports_before_pruning.values())

    ports_after_pruning, final_group_remap = prune_ports(ports_before_pruning)
    final_port_count = sum(len(ports) for ports in ports_after_pruning.values())
    group_count = max(final_group_remap.values(), default=0)

    tileop, overflow = encode_tileop(ports_after_pruning, group_count)
    budget = packed_budget_counts(ports_to_public_dict(ports_after_pruning))
    if overflow:
        tileop_status = "overflow"
        tileop_offsets = None
        tileop_counts = None
        tileop_decoded = {face: {"groups": [], "h1_packed": [], "h1": []} for face in ("I", "O", "L", "R")}
    else:
        parsed = parse_tileop(tileop)
        tileop_status = parsed.status
        tileop_offsets = {
            "off_I": parsed.off_I,
            "off_L": parsed.off_L,
            "off_R": parsed.off_R,
        }
        tileop_counts = {
            "o_cnt": parsed.o_cnt,
            "i_cnt": parsed.i_cnt,
            "l_cnt": parsed.l_cnt,
            "r_cnt": parsed.r_cnt,
            "h_start": parsed.h_start,
        }
        tileop_decoded = decode_tileop(tileop, tile_origin=(a_lo, b_lo))
    final_groups = _finalize_prime_groups(
        prime_count=bitmap_prime_count,
        component_roots=component_roots,
        pre_prune_root_to_group=root_to_group,
        final_group_remap=final_group_remap,
    )
    if overflow:
        final_groups = [0] * bitmap_prime_count

    return {
        "bitmap": bitmap,
        "prime_count": len(tile_prime_pos),
        "bitmap_prime_count": bitmap_prime_count,
        "prefix": prefix,
        "prime_pos": [pos_to_coords(pos, a_lo, b_lo) for pos in prime_pos],
        "tile_prime_pos": [pos_to_coords(pos, a_lo, b_lo) for pos in tile_prime_pos],
        "prime_pos_bitmap": prime_pos,
        "tile_prime_pos_bitmap": tile_prime_pos,
        "groups": final_groups,
        "group_count": group_count,
        "face_primes": {
            face: [(prime.a, prime.b) for prime in primes]
            for face, primes in face_primes.items()
        },
        "ports": ports_to_public_dict(ports_after_pruning),
        "ports_before_pruning": raw_port_count,
        "ports_after_pruning": final_port_count,
        "tileop": tileop,
        "overflow": overflow,
        "a_lo": a_lo,
        "b_lo": b_lo,
        "tileop_status": tileop_status,
        "tileop_offsets": tileop_offsets,
        "tileop_counts": tileop_counts,
        "tileop_decoded": tileop_decoded,
    }
