#!/usr/bin/env python3
"""Extract or verify 128-byte TileOps from 148-byte CUDA binary records."""

from __future__ import annotations

import argparse
import os
import struct
import sys
from dataclasses import dataclass

TILES_PER_TOWER = 32
TILE_SIDE = 256
TILEOP_SIZE = 128
TILEOP_HEADER_BYTES = 3
TILEOP_PAYLOAD_BYTES = 125
CUDA_RECORD_SIZE = 148
OVERFLOW_SENTINEL = 0xFF
EMPTY_OFFSET = 3

INPUT_HEADER_STRUCT = struct.Struct("<I")
CUDA_RECORD_STRUCT = struct.Struct("<qqI128s")
OUTPUT_HEADER_STRUCT = struct.Struct("<II")


class FormatError(ValueError):
    """Raised when the input binary or a TileOp is malformed."""


@dataclass(frozen=True)
class TileOpCounts:
    off_i: int
    off_l: int
    off_r: int
    o_cnt: int
    i_cnt: int
    l_cnt: int
    r_cnt: int
    h_start: int


def is_overflow(tile: bytes) -> bool:
    return tile[0] == OVERFLOW_SENTINEL


def is_dead(tile: bytes) -> bool:
    return tile[:4] == bytes((EMPTY_OFFSET, EMPTY_OFFSET, EMPTY_OFFSET, 0))


def parse_counts(tile: bytes, payload_budget: int = TILEOP_PAYLOAD_BYTES) -> TileOpCounts:
    if len(tile) != TILEOP_SIZE:
        raise FormatError(f"expected {TILEOP_SIZE} tile bytes, got {len(tile)}")
    if is_overflow(tile):
        raise FormatError("overflow tiles do not have parseable counts")

    off_i, off_l, off_r = tile[0], tile[1], tile[2]
    if not (TILEOP_HEADER_BYTES <= off_i <= off_l <= off_r <= TILEOP_SIZE):
        raise FormatError(
            f"invalid offsets off_I={off_i} off_L={off_l} off_R={off_r}"
        )

    o_cnt = off_i - TILEOP_HEADER_BYTES
    i_cnt = off_l - off_i
    l_cnt = off_r - off_l
    residual = payload_budget - o_cnt - i_cnt - (2 * l_cnt)
    if residual < 0:
        raise FormatError(
            "negative residual payload for counts "
            f"o={o_cnt} i={i_cnt} l={l_cnt} payload={payload_budget}"
        )

    r_cnt = residual // 2
    h_start = off_r + r_cnt
    if h_start + l_cnt + r_cnt > TILEOP_SIZE:
        raise FormatError(
            f"h1 payload overruns tile: h_start={h_start} l={l_cnt} r={r_cnt}"
        )

    return TileOpCounts(
        off_i=off_i,
        off_l=off_l,
        off_r=off_r,
        o_cnt=o_cnt,
        i_cnt=i_cnt,
        l_cnt=l_cnt,
        r_cnt=r_cnt,
        h_start=h_start,
    )


def max_group_label(tile: bytes) -> int:
    if is_overflow(tile) or is_dead(tile):
        return 0

    counts = parse_counts(tile)
    max_label = 0

    for idx in range(TILEOP_HEADER_BYTES, counts.off_i):
        max_label = max(max_label, tile[idx])

    for idx in range(counts.off_i, counts.off_l):
        max_label = max(max_label, tile[idx])

    for idx in range(counts.off_l, counts.off_r):
        label = tile[idx] & 0x7F
        if label != 0:
            max_label = max(max_label, label)

    for idx in range(counts.off_r, counts.off_r + counts.r_cnt):
        label = tile[idx] & 0x7F
        if label != 0:
            max_label = max(max_label, label)

    return max_label


def has_h1_256(tile: bytes) -> bool:
    if is_overflow(tile) or is_dead(tile):
        return False

    counts = parse_counts(tile)
    l_h1_start = counts.h_start
    r_h1_start = counts.h_start + counts.l_cnt

    for i in range(counts.l_cnt):
        group_byte = tile[counts.off_l + i]
        h1_byte = tile[l_h1_start + i]
        if (((group_byte >> 7) << 8) | h1_byte) == 256:
            return True

    for i in range(counts.r_cnt):
        group_byte = tile[counts.off_r + i]
        h1_byte = tile[r_h1_start + i]
        if (((group_byte >> 7) << 8) | h1_byte) == 256:
            return True

    return False


def read_input_header(path: str) -> tuple[int, int]:
    input_size = os.path.getsize(path)
    with open(path, "rb") as fh:
        header = fh.read(INPUT_HEADER_STRUCT.size)
    if len(header) != INPUT_HEADER_STRUCT.size:
        raise FormatError("input file is too small to contain the tile-count header")

    (n_tiles,) = INPUT_HEADER_STRUCT.unpack(header)
    expected_size = INPUT_HEADER_STRUCT.size + (n_tiles * CUDA_RECORD_SIZE)
    if input_size != expected_size:
        raise FormatError(
            f"input size mismatch: header says {n_tiles} tiles -> {expected_size} bytes, "
            f"actual file is {input_size} bytes"
        )
    if n_tiles % TILES_PER_TOWER != 0:
        raise FormatError(
            f"tile count {n_tiles} is not divisible by {TILES_PER_TOWER}"
        )

    return n_tiles, input_size


def extract_file(input_path: str, output_path: str) -> int:
    n_tiles, input_size = read_input_header(input_path)
    n_towers = n_tiles // TILES_PER_TOWER

    with open(input_path, "rb") as src, open(output_path, "wb") as dst:
        src.read(INPUT_HEADER_STRUCT.size)
        dst.write(OUTPUT_HEADER_STRUCT.pack(n_tiles, n_towers))

        for tile_idx in range(n_tiles):
            record = src.read(CUDA_RECORD_STRUCT.size)
            if len(record) != CUDA_RECORD_STRUCT.size:
                raise FormatError(
                    f"short read for tile {tile_idx}: expected {CUDA_RECORD_STRUCT.size} bytes"
                )
            _a_lo, _b_lo, _prime_count, tileop = CUDA_RECORD_STRUCT.unpack(record)
            dst.write(tileop)

    output_size = os.path.getsize(output_path)
    print(f"tiles: {n_tiles}")
    print(f"towers: {n_towers}")
    print(f"input_size: {input_size} bytes")
    print(f"output_size: {output_size} bytes")
    return 0


def verify_file(input_path: str) -> int:
    n_tiles, _input_size = read_input_header(input_path)
    n_towers = n_tiles // TILES_PER_TOWER

    overflow_count = 0
    dead_count = 0
    normal_count = 0
    oi_mismatch_count = 0
    h1_256_count = 0
    max_label_sum = 0
    max_label_min: int | None = None
    max_label_max: int | None = None
    bad_a_lo_tiles = 0
    bad_towers: set[int] = set()

    with open(input_path, "rb") as fh:
        fh.read(INPUT_HEADER_STRUCT.size)
        for tile_idx in range(n_tiles):
            record = fh.read(CUDA_RECORD_STRUCT.size)
            if len(record) != CUDA_RECORD_STRUCT.size:
                raise FormatError(
                    f"short read for tile {tile_idx}: expected {CUDA_RECORD_STRUCT.size} bytes"
                )

            a_lo, _b_lo, _prime_count, tile = CUDA_RECORD_STRUCT.unpack(record)
            tower_idx = tile_idx // TILES_PER_TOWER
            expected_a_lo = tower_idx * TILE_SIDE
            if a_lo != expected_a_lo:
                bad_a_lo_tiles += 1
                bad_towers.add(tower_idx)

            if is_overflow(tile):
                overflow_count += 1
                continue

            if is_dead(tile):
                dead_count += 1
                continue

            counts = parse_counts(tile)
            normal_count += 1

            if counts.o_cnt != counts.i_cnt:
                oi_mismatch_count += 1

            tile_max = max_group_label(tile)
            max_label_sum += tile_max
            if max_label_min is None or tile_max < max_label_min:
                max_label_min = tile_max
            if max_label_max is None or tile_max > max_label_max:
                max_label_max = tile_max

            if has_h1_256(tile):
                h1_256_count += 1

    total_from_census = overflow_count + dead_count + normal_count
    if total_from_census != n_tiles:
        raise FormatError(
            f"census mismatch: counted {total_from_census} tiles but header says {n_tiles}"
        )

    mean_text = "n/a"
    min_text = "n/a"
    max_text = "n/a"
    if normal_count:
        mean_text = f"{(max_label_sum / normal_count):.3f}"
        min_text = str(max_label_min)
        max_text = str(max_label_max)

    print(f"tiles: {n_tiles}")
    print(f"towers: {n_towers}")
    print(
        "census: "
        f"overflow={overflow_count} dead={dead_count} normal={normal_count}"
    )
    print(
        "max_group_label: "
        f"min={min_text} max={max_text} mean={mean_text}"
    )
    print(f"o_cnt!=i_cnt: {oi_mismatch_count}")
    print(f"h1=256 tiles: {h1_256_count}")
    print(
        "tower_sanity: "
        f"bad_towers={len(bad_towers)} a_lo_mismatches={bad_a_lo_tiles} "
        f"tiles_per_tower={TILES_PER_TOWER}"
    )

    verification_ok = bad_a_lo_tiles == 0 and oi_mismatch_count == 0
    print(f"verification: {'PASS' if verification_ok else 'FAIL'}")
    return 0 if verification_ok else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Extract 128-byte TileOps from 148-byte CUDA binary records."
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        help="scan the input and print verification diagnostics",
    )
    parser.add_argument("input", help="input CUDA binary (.bin)")
    parser.add_argument(
        "output",
        nargs="?",
        help="output stripped TileOp binary (required unless --verify is used)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.verify:
        if args.output is not None:
            parser.error("output path is not accepted with --verify")
        return verify_file(args.input)

    if args.output is None:
        parser.error("output path is required in extract mode")
    return extract_file(args.input, args.output)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FormatError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
