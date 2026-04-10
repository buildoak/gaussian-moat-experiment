"""Shared TileOp v2 parsing, decoding, and structural validation."""

from __future__ import annotations

from dataclasses import dataclass


FACES = ("I", "O", "L", "R")
PAYLOAD_BUDGET = 125
HEADER_BYTES = 3
TILEOP_SIZE = 128
OVERFLOW_BYTE = 0xFF
EMPTY_OFFSET = 3
GROUP_LABEL_MAX = 127


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


def encode_lr_group_byte(group_id: int, h1: int) -> int:
    if not 0 <= h1 <= 256:
        raise ValueError(f"h1={h1} out of range for TileOp v2 L/R encoding")
    if not 1 <= group_id <= GROUP_LABEL_MAX:
        raise ValueError(f"group_id={group_id} out of range for TileOp v2 L/R encoding")
    return ((h1 >> 8) << 7) | (group_id & 0x7F)


def encode_lr_h1_byte(h1: int) -> int:
    if not 0 <= h1 <= 256:
        raise ValueError(f"h1={h1} out of range for TileOp v2 L/R encoding")
    return h1 & 0xFF


def decode_packed_h1(group_byte: int, h1_byte: int) -> int:
    return ((group_byte >> 7) << 8) | h1_byte


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

    r_groups = list(tileop[off_R:off_R + max(r_cnt, 0)])
    live_r_cnt = len(r_groups)
    while live_r_cnt > 0 and r_groups[live_r_cnt - 1] == 0:
        live_r_cnt -= 1

    groups = {
        "O": list(tileop[HEADER_BYTES:off_I]),
        "I": list(tileop[off_I:off_L]),
        "L": list(tileop[off_L:off_R]),
        "R": r_groups[:live_r_cnt],
    }
    h1_packed = {
        "I": [],
        "O": [],
        "L": list(tileop[h_start:h_start + max(l_cnt, 0)]),
        "R": list(tileop[h_start + max(l_cnt, 0):h_start + max(l_cnt, 0) + live_r_cnt]),
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
    assert parsed.groups is not None
    assert parsed.h1_packed is not None
    return [
        decode_packed_h1(group_byte, h1_byte)
        for group_byte, h1_byte in zip(parsed.groups[face], parsed.h1_packed[face], strict=False)
    ]


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
            h1 = [
                decode_packed_h1(group_byte, h1_byte)
                for group_byte, h1_byte in zip(parsed.groups[face], parsed.h1_packed[face], strict=False)
            ]
        decoded[face] = {
            "groups": [
                (group_byte & 0x7F) if face in ("L", "R") else group_byte
                for group_byte in parsed.groups[face]
            ],
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

    l_groups = list(tileop[off_L:off_R])
    r_groups = list(tileop[off_R:off_R + r_cnt])
    live_r_cnt = len(r_groups)
    while live_r_cnt > 0 and r_groups[live_r_cnt - 1] == 0:
        live_r_cnt -= 1

    for face, groups in {
        "O": list(tileop[HEADER_BYTES:off_I]),
        "I": list(tileop[off_I:off_L]),
        "L": l_groups,
        "R": r_groups[:live_r_cnt],
    }.items():
        for group in groups:
            max_group = GROUP_LABEL_MAX if face in ("L", "R") else 255
            decoded_group = group & 0x7F if face in ("L", "R") else group
            if not 1 <= decoded_group <= max_group:
                problems.append(f"{face} group out of range: {group}")

    l_h1 = list(tileop[h_start:h_start + l_cnt])
    r_h1 = list(tileop[h_start + l_cnt:h_start + l_cnt + live_r_cnt])
    for face, groups, h1_bytes in (("L", l_groups, l_h1), ("R", r_groups[:live_r_cnt], r_h1)):
        if len(groups) != len(h1_bytes):
            problems.append(f"{face} groups/h1 length mismatch: {len(groups)} vs {len(h1_bytes)}")
            continue
        for group_byte, h1_byte in zip(groups, h1_bytes, strict=False):
            h1 = decode_packed_h1(group_byte, h1_byte)
            if not 0 <= h1 <= 256:
                problems.append(f"{face} h1 out of range: group_byte={group_byte} h1_byte={h1_byte} decoded={h1}")
    return problems
