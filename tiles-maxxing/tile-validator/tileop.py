"""Shared TileOp v2 parsing, decoding, and structural validation."""

from __future__ import annotations

from dataclasses import dataclass


FACES = ("I", "O", "L", "R")
PAYLOAD_BUDGET = 125
HEADER_BYTES = 3
TILEOP_SIZE = 128
OVERFLOW_BYTE = 0xFF
EMPTY_OFFSET = 3
TILE_SIDE = 256


@dataclass(frozen=True)
class ParsedTileOp:
    status: str
    off_I: int | None = None
    off_L: int | None = None
    off_R: int | None = None
    o_cnt: int = 0
    i_cnt: int = 0
    l_cnt: int = 0
    r_cnt: int = 0
    h_start: int | None = None
    groups: dict[str, list[int]] | None = None
    h1_packed: dict[str, list[int]] | None = None


def is_overflow_tileop(tileop: bytes) -> bool:
    return len(tileop) == TILEOP_SIZE and tileop == bytes([OVERFLOW_BYTE] * TILEOP_SIZE)


def is_dead_tileop(tileop: bytes) -> bool:
    return (
        len(tileop) == TILEOP_SIZE
        and not is_overflow_tileop(tileop)
        and tileop[0] == EMPTY_OFFSET
        and tileop[1] == EMPTY_OFFSET
        and tileop[2] == EMPTY_OFFSET
        and tileop[3] == 0
    )


def face_h1_parity(face: str, tile_origin: tuple[int, int]) -> int:
    a_lo, b_lo = tile_origin
    if face not in ("L", "R"):
        return 0
    fixed_b = b_lo if face == "L" else b_lo + TILE_SIDE
    return (1 ^ (a_lo & 1) ^ (fixed_b & 1)) & 1


def decode_packed_h1(stored: int, face: str, tile_origin: tuple[int, int]) -> int:
    return 2 * stored + face_h1_parity(face, tile_origin)


def parse_tileop(tileop: bytes) -> ParsedTileOp:
    if len(tileop) != TILEOP_SIZE:
        raise ValueError(f"tileop length is {len(tileop)}, expected {TILEOP_SIZE}")
    if is_overflow_tileop(tileop):
        return ParsedTileOp(status="overflow")
    if is_dead_tileop(tileop):
        return ParsedTileOp(
            status="dead",
            off_I=EMPTY_OFFSET,
            off_L=EMPTY_OFFSET,
            off_R=EMPTY_OFFSET,
            h_start=EMPTY_OFFSET,
            groups={face: [] for face in FACES},
            h1_packed={face: [] for face in FACES},
        )

    off_I = tileop[0]
    off_L = tileop[1]
    off_R = tileop[2]
    o_cnt = off_I - HEADER_BYTES
    i_cnt = off_L - off_I
    l_cnt = off_R - off_L
    r_cnt = (PAYLOAD_BUDGET - o_cnt - i_cnt - 2 * l_cnt) >> 1
    h_start = off_R + r_cnt

    groups = {
        "O": list(tileop[HEADER_BYTES:off_I]),
        "I": list(tileop[off_I:off_L]),
        "L": list(tileop[off_L:off_R]),
        "R": list(tileop[off_R:off_R + max(r_cnt, 0)]),
    }
    h1_packed = {
        "I": [],
        "O": [],
        "L": list(tileop[h_start:h_start + max(l_cnt, 0)]),
        "R": list(tileop[h_start + max(l_cnt, 0):h_start + max(l_cnt, 0) + max(r_cnt, 0)]),
    }
    status = "dead" if is_dead_tileop(tileop) else "normal"
    return ParsedTileOp(
        status=status,
        off_I=off_I,
        off_L=off_L,
        off_R=off_R,
        o_cnt=o_cnt,
        i_cnt=i_cnt,
        l_cnt=l_cnt,
        r_cnt=r_cnt,
        h_start=h_start,
        groups=groups,
        h1_packed=h1_packed,
    )


def decode_face_groups(tileop: bytes, face: str) -> list[int]:
    parsed = parse_tileop(tileop)
    if parsed.status == "overflow":
        return []
    assert parsed.groups is not None
    return list(parsed.groups[face])


def decode_face_h1(tileop: bytes, face: str, *, tile_origin: tuple[int, int]) -> list[int]:
    if face not in ("L", "R"):
        return []
    parsed = parse_tileop(tileop)
    if parsed.status == "overflow":
        return []
    assert parsed.h1_packed is not None
    return [decode_packed_h1(stored, face, tile_origin) for stored in parsed.h1_packed[face]]


def decode_tileop(
    tileop: bytes,
    *,
    tile_origin: tuple[int, int] | None = None,
) -> dict[str, dict[str, list[int]]]:
    parsed = parse_tileop(tileop)
    if parsed.status == "overflow":
        return {face: {"groups": [], "h1": [], "h1_packed": []} for face in FACES}

    assert parsed.groups is not None
    assert parsed.h1_packed is not None
    decoded: dict[str, dict[str, list[int]]] = {}
    for face in FACES:
        h1 = []
        if face in ("L", "R") and tile_origin is not None:
            h1 = [decode_packed_h1(stored, face, tile_origin) for stored in parsed.h1_packed[face]]
        decoded[face] = {
            "groups": list(parsed.groups[face]),
            "h1_packed": list(parsed.h1_packed[face]),
            "h1": h1,
        }
    return decoded


def validate_tileop_structure(tileop: bytes) -> list[str]:
    problems: list[str] = []
    if len(tileop) != TILEOP_SIZE:
        return [f"tileop length is {len(tileop)}, expected {TILEOP_SIZE}"]

    if tileop[0] == OVERFLOW_BYTE and tileop != bytes([OVERFLOW_BYTE] * TILEOP_SIZE):
        problems.append("overflow sentinel requires all 128 bytes to be 0xFF")
        return problems
    if is_overflow_tileop(tileop):
        return problems
    if is_dead_tileop(tileop):
        return problems

    off_I = tileop[0]
    off_L = tileop[1]
    off_R = tileop[2]
    if not (HEADER_BYTES <= off_I <= off_L <= off_R <= 127):
        problems.append(
            f"offset monotonicity violated: off_I={off_I} off_L={off_L} off_R={off_R}"
        )
        return problems

    o_cnt = off_I - HEADER_BYTES
    i_cnt = off_L - off_I
    l_cnt = off_R - off_L
    residual = PAYLOAD_BUDGET - o_cnt - i_cnt - 2 * l_cnt
    if residual < 0:
        problems.append(
            f"negative residual budget: o={o_cnt} i={i_cnt} l={l_cnt} residual={residual}"
        )
        return problems

    r_cnt = residual >> 1
    packed_used = o_cnt + i_cnt + 2 * l_cnt + 2 * r_cnt
    if packed_used > PAYLOAD_BUDGET:
        problems.append(f"packed payload uses {packed_used} bytes, exceeds {PAYLOAD_BUDGET}")

    h_start = off_R + r_cnt
    l_h1_end = h_start + l_cnt
    r_h1_end = l_h1_end + r_cnt
    if h_start > TILEOP_SIZE or l_h1_end > TILEOP_SIZE or r_h1_end > TILEOP_SIZE:
        problems.append(
            f"payload slices overflow record: h_start={h_start} l_end={l_h1_end} r_end={r_h1_end}"
        )
        return problems

    for face, groups in {
        "O": list(tileop[HEADER_BYTES:off_I]),
        "I": list(tileop[off_I:off_L]),
        "L": list(tileop[off_L:off_R]),
    }.items():
        for group in groups:
            if not 1 <= group <= 255:
                problems.append(f"{face} group out of range: {group}")
    return problems
