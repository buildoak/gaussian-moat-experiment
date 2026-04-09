"""Composition helpers operating on canonical 128-byte TileOps."""

from __future__ import annotations

from collections.abc import Mapping

from tileop import decode_face_groups, decode_face_h1, decode_tileop, is_overflow_tileop
from uf import UnionFind

FACES = ("I", "O", "L", "R")
TileInput = bytes | bytearray | Mapping[str, object]


def _tileop_bytes(tile: TileInput) -> bytes:
    if isinstance(tile, (bytes, bytearray)):
        return bytes(tile)
    tileop = tile.get("tileop")
    if not isinstance(tileop, (bytes, bytearray)):
        raise TypeError("tile input must be TileOp bytes or a process_tile() result dict")
    return bytes(tileop)


def _face_ports(tile: TileInput, face: str) -> list[dict]:
    if not isinstance(tile, Mapping):
        return []
    ports = tile.get("ports")
    if not isinstance(ports, Mapping):
        return []
    face_ports = ports.get(face)
    if not isinstance(face_ports, list):
        return []
    return [port for port in face_ports if isinstance(port, Mapping)]


def _decoded_face(tile: TileInput, face: str) -> dict[str, list[int]] | None:
    if not isinstance(tile, Mapping):
        return None
    decoded = tile.get("tileop_decoded")
    if not isinstance(decoded, Mapping):
        return None
    face_decoded = decoded.get(face)
    if not isinstance(face_decoded, Mapping):
        return None
    return {
        "groups": list(face_decoded.get("groups", [])),
        "h1": list(face_decoded.get("h1", [])),
        "h1_packed": list(face_decoded.get("h1_packed", [])),
    }


def _tile_origin(tile: TileInput) -> tuple[int, int] | None:
    if not isinstance(tile, Mapping):
        return None
    a_lo = tile.get("a_lo")
    b_lo = tile.get("b_lo")
    if isinstance(a_lo, int) and isinstance(b_lo, int):
        return (a_lo, b_lo)
    return None


def _shared_prime_slot_pairs(
    tile_a: TileInput,
    face_a: str,
    tile_b: TileInput,
    face_b: str,
) -> list[tuple[int, int]]:
    ports_a = _face_ports(tile_a, face_a)
    ports_b = _face_ports(tile_b, face_b)
    if not ports_a or not ports_b:
        return []

    shared: list[tuple[int, int]] = []
    primes_b: dict[tuple[int, int], set[int]] = {}
    for slot_b, port_b in enumerate(ports_b):
        for prime in port_b.get("primes", []):
            if isinstance(prime, (tuple, list)) and len(prime) == 2:
                primes_b.setdefault((int(prime[0]), int(prime[1])), set()).add(slot_b)

    seen: set[tuple[int, int]] = set()
    for slot_a, port_a in enumerate(ports_a):
        for prime in port_a.get("primes", []):
            if not isinstance(prime, (tuple, list)) or len(prime) != 2:
                continue
            coord = (int(prime[0]), int(prime[1]))
            for slot_b in primes_b.get(coord, ()):
                pair = (slot_a, slot_b)
                if pair not in seen:
                    seen.add(pair)
                    shared.append(pair)
    return shared


def _slots_for_tile(tileop: bytes, tile_tag: str) -> tuple[list[tuple[str, int]], dict[tuple[str, int], int]]:
    slots: list[tuple[str, int]] = []
    groups: dict[tuple[str, int], int] = {}
    decoded = decode_tileop(tileop)
    for face in FACES:
        for slot, group in enumerate(decoded[face]["groups"]):
            key = (tile_tag, len(slots))
            slots.append(key)
            groups[key] = group
    return slots, groups


def _wire_internal_groups(tileop: bytes, tile_tag: str, slot_index: dict[tuple[str, int], int], uf: UnionFind):
    decoded = decode_tileop(tileop)
    leaders: dict[int, int] = {}
    cursor = 0
    for face in FACES:
        for group in decoded[face]["groups"]:
            idx = slot_index[(tile_tag, cursor)]
            cursor += 1
            if group in leaders:
                uf.union(leaders[group], idx)
            else:
                leaders[group] = idx


def compose_vertical(tileop_a: TileInput, tileop_b: TileInput) -> bool:
    """Compose O(face lower tile) to I(face upper tile)."""

    bytes_a = _tileop_bytes(tileop_a)
    bytes_b = _tileop_bytes(tileop_b)

    if is_overflow_tileop(bytes_a) or is_overflow_tileop(bytes_b):
        return bool(decode_face_groups(bytes_a, "I") and decode_face_groups(bytes_b, "O"))

    slots_a, _ = _slots_for_tile(bytes_a, "A")
    slots_b, _ = _slots_for_tile(bytes_b, "B")
    all_slots = slots_a + slots_b
    uf = UnionFind(len(all_slots))
    slot_index = {slot: i for i, slot in enumerate(all_slots)}

    _wire_internal_groups(bytes_a, "A", slot_index, uf)
    _wire_internal_groups(bytes_b, "B", slot_index, uf)

    shared_pairs = _shared_prime_slot_pairs(tileop_a, "O", tileop_b, "I")
    if shared_pairs:
        a_cursor = _face_slot_start(bytes_a, "O")
        b_cursor = _face_slot_start(bytes_b, "I")
        for slot_a, slot_b in shared_pairs:
            uf.union(slot_index[("A", a_cursor + slot_a)], slot_index[("B", b_cursor + slot_b)])
    else:
        a_groups = _decoded_face(tileop_a, "O")["groups"] if _decoded_face(tileop_a, "O") else decode_face_groups(bytes_a, "O")
        b_groups = _decoded_face(tileop_b, "I")["groups"] if _decoded_face(tileop_b, "I") else decode_face_groups(bytes_b, "I")
        if len(a_groups) != len(b_groups):
            raise ValueError("aligned I/O face count mismatch")
        a_cursor = _face_slot_start(bytes_a, "O")
        b_cursor = _face_slot_start(bytes_b, "I")
        for slot in range(len(a_groups)):
            uf.union(slot_index[("A", a_cursor + slot)], slot_index[("B", b_cursor + slot)])

    inner = _face_global_indices(bytes_a, "A", "I", slot_index)
    outer = _face_global_indices(bytes_b, "B", "O", slot_index)
    return any(uf.find(i) == uf.find(o) for i in inner for o in outer)


def compose_horizontal(tileop_a: TileInput, tileop_b: TileInput, delta_h: int = 0) -> bool:
    """Compose R(face left tile) to L(face right tile) by h1 + delta_h."""

    bytes_a = _tileop_bytes(tileop_a)
    bytes_b = _tileop_bytes(tileop_b)

    if is_overflow_tileop(bytes_a) or is_overflow_tileop(bytes_b):
        return bool(decode_face_groups(bytes_a, "I") and decode_face_groups(bytes_b, "O"))

    slots_a, _ = _slots_for_tile(bytes_a, "A")
    slots_b, _ = _slots_for_tile(bytes_b, "B")
    all_slots = slots_a + slots_b
    uf = UnionFind(len(all_slots))
    slot_index = {slot: i for i, slot in enumerate(all_slots)}

    _wire_internal_groups(bytes_a, "A", slot_index, uf)
    _wire_internal_groups(bytes_b, "B", slot_index, uf)

    shared_pairs = _shared_prime_slot_pairs(tileop_a, "R", tileop_b, "L") if delta_h == 0 else []
    a_cursor = _face_slot_start(bytes_a, "R")
    b_cursor = _face_slot_start(bytes_b, "L")
    if shared_pairs:
        for slot_a, slot_b in shared_pairs:
            uf.union(slot_index[("A", a_cursor + slot_a)], slot_index[("B", b_cursor + slot_b)])
    else:
        origin_a = _tile_origin(tileop_a)
        origin_b = _tile_origin(tileop_b)
        if origin_a is None or origin_b is None:
            raise ValueError("horizontal composition requires tile origins for TileOp v2 h1 decode")
        decoded_a = _decoded_face(tileop_a, "R")
        decoded_b = _decoded_face(tileop_b, "L")
        a_h1 = decoded_a["h1"] if decoded_a is not None and decoded_a["h1"] else decode_face_h1(bytes_a, "R", tile_origin=origin_a)
        b_h1 = decoded_b["h1"] if decoded_b is not None and decoded_b["h1"] else decode_face_h1(bytes_b, "L", tile_origin=origin_b)
        for sa, ah in enumerate(a_h1):
            for sb, bh in enumerate(b_h1):
                if ah == bh + delta_h:
                    uf.union(slot_index[("A", a_cursor + sa)], slot_index[("B", b_cursor + sb)])

    inner = _face_global_indices(bytes_a, "A", "I", slot_index) + _face_global_indices(bytes_b, "B", "I", slot_index)
    outer = _face_global_indices(bytes_a, "A", "O", slot_index) + _face_global_indices(bytes_b, "B", "O", slot_index)
    return any(uf.find(i) == uf.find(o) for i in inner for o in outer)


def _face_slot_start(tileop: bytes, face: str) -> int:
    offset = 0
    for candidate in FACES:
        if candidate == face:
            return offset
        offset += len(decode_face_groups(tileop, candidate))
    return offset


def _face_global_indices(tileop: bytes, tile_tag: str, face: str, slot_index: dict[tuple[str, int], int]) -> list[int]:
    start = _face_slot_start(tileop, face)
    return [slot_index[(tile_tag, start + i)] for i in range(len(decode_face_groups(tileop, face)))]
