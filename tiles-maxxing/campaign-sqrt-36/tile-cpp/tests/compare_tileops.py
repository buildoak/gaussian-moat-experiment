#!/usr/bin/env python3
"""Compare C++ and CUDA TileOp binary dumps byte-for-byte.

Both files use the same record layout (from CUDA dump mode):
  Header:  uint32_t num_tiles
  Records: int64_t a_lo, int64_t b_lo, uint32_t prime_count, uint8_t tileop[256]
           (= 276 bytes per record)

Usage:
  python3 compare_tileops.py <cpp_tileops.bin> <cuda_tileops.bin> [--max-mismatches N]
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass

TILEOP_SIZE = 256
RECORD_STRUCT = struct.Struct("<qqI256s")  # a_lo(i64) + b_lo(i64) + prime_count(u32) + tileop(256B)
HEADER_STRUCT = struct.Struct("<I")
OVERFLOW_SENTINEL = 0xFF
EMPTY_OFFSET = 3


@dataclass(frozen=True)
class TileRecord:
    a_lo: int
    b_lo: int
    prime_count: int
    tileop: bytes


def read_dump(path: str) -> list[TileRecord]:
    """Read a binary dump file and return list of TileRecords."""
    with open(path, "rb") as fh:
        header = fh.read(HEADER_STRUCT.size)
        if len(header) != HEADER_STRUCT.size:
            raise ValueError(f"{path}: too small for header")
        (num_tiles,) = HEADER_STRUCT.unpack(header)

        records = []
        for i in range(num_tiles):
            data = fh.read(RECORD_STRUCT.size)
            if len(data) != RECORD_STRUCT.size:
                raise ValueError(f"{path}: short read at tile {i}, got {len(data)} bytes")
            a_lo, b_lo, prime_count, tileop = RECORD_STRUCT.unpack(data)
            records.append(TileRecord(a_lo, b_lo, prime_count, tileop))

        remaining = fh.read(1)
        if remaining:
            print(f"warning: {path} has trailing bytes after {num_tiles} records", file=sys.stderr)

    return records


def tile_status(tileop: bytes) -> str:
    """Classify a TileOp as overflow/empty/normal."""
    if tileop[0] == OVERFLOW_SENTINEL:
        return "overflow"
    if tileop[0] == EMPTY_OFFSET and tileop[1] == EMPTY_OFFSET and tileop[2] == EMPTY_OFFSET:
        return "empty"
    return "normal"


def hex_dump(data: bytes, width: int = 32) -> str:
    """Format bytes as hex dump with offset markers."""
    lines = []
    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        lines.append(f"  {off:3d}: {hex_part}")
    return "\n".join(lines)


def diff_bytes(a: bytes, b: bytes) -> list[int]:
    """Return list of byte offsets where a and b differ."""
    return [i for i in range(min(len(a), len(b))) if a[i] != b[i]]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare C++ and CUDA TileOp binary dumps byte-for-byte."
    )
    parser.add_argument("cpp_file", help="C++ reference dump (cpp_tileops.bin)")
    parser.add_argument("cuda_file", help="CUDA dump (cuda_tileops.bin)")
    parser.add_argument(
        "--max-mismatches", type=int, default=20,
        help="max number of mismatch details to print (default: 20)"
    )
    parser.add_argument(
        "--hex", action="store_true",
        help="print full hex dumps for mismatched tiles"
    )
    args = parser.parse_args(argv)

    print(f"Reading C++ dump:  {args.cpp_file}")
    cpp_records = read_dump(args.cpp_file)
    print(f"  {len(cpp_records)} tiles")

    print(f"Reading CUDA dump: {args.cuda_file}")
    cuda_records = read_dump(args.cuda_file)
    print(f"  {len(cuda_records)} tiles")

    # Build lookup by (a_lo, b_lo) for CUDA records
    cuda_by_coord: dict[tuple[int, int], TileRecord] = {}
    for rec in cuda_records:
        key = (rec.a_lo, rec.b_lo)
        if key in cuda_by_coord:
            print(f"warning: duplicate CUDA coord ({rec.a_lo}, {rec.b_lo})", file=sys.stderr)
        cuda_by_coord[key] = rec

    # Compare
    total = 0
    matched = 0
    mismatched = 0
    missing_in_cuda = 0
    prime_count_mismatches = 0
    mismatch_details: list[str] = []

    # Per-status counters
    status_counts = {"overflow": 0, "empty": 0, "normal": 0}
    status_match = {"overflow": 0, "empty": 0, "normal": 0}

    for cpp_rec in cpp_records:
        total += 1
        key = (cpp_rec.a_lo, cpp_rec.b_lo)
        cuda_rec = cuda_by_coord.get(key)

        if cuda_rec is None:
            missing_in_cuda += 1
            continue

        cpp_status = tile_status(cpp_rec.tileop)
        cuda_status = tile_status(cuda_rec.tileop)
        status_counts[cpp_status] = status_counts.get(cpp_status, 0) + 1

        if cpp_rec.tileop == cuda_rec.tileop:
            matched += 1
            status_match[cpp_status] = status_match.get(cpp_status, 0) + 1
        else:
            mismatched += 1
            diffpos = diff_bytes(cpp_rec.tileop, cuda_rec.tileop)

            if len(mismatch_details) < args.max_mismatches:
                detail = (
                    f"  MISMATCH #{mismatched}: coord=({cpp_rec.a_lo}, {cpp_rec.b_lo})\n"
                    f"    cpp_status={cpp_status}  cuda_status={cuda_status}\n"
                    f"    cpp_prime_count={cpp_rec.prime_count}  cuda_prime_count={cuda_rec.prime_count}\n"
                    f"    differing_bytes={len(diffpos)} at offsets: {diffpos[:32]}"
                    f"{'...' if len(diffpos) > 32 else ''}\n"
                    f"    cpp_header:  {cpp_rec.tileop[0]:02x} {cpp_rec.tileop[1]:02x} {cpp_rec.tileop[2]:02x}\n"
                    f"    cuda_header: {cuda_rec.tileop[0]:02x} {cuda_rec.tileop[1]:02x} {cuda_rec.tileop[2]:02x}"
                )
                if args.hex:
                    detail += f"\n    --- C++ TileOp ---\n{hex_dump(cpp_rec.tileop)}"
                    detail += f"\n    --- CUDA TileOp ---\n{hex_dump(cuda_rec.tileop)}"
                mismatch_details.append(detail)

        if cpp_rec.prime_count != cuda_rec.prime_count:
            prime_count_mismatches += 1

    # Report
    print(f"\n{'='*60}")
    print(f"COMPARISON RESULTS")
    print(f"{'='*60}")
    print(f"C++ tiles:          {len(cpp_records)}")
    print(f"CUDA tiles:         {len(cuda_records)}")
    print(f"Compared:           {total - missing_in_cuda}")
    print(f"Missing in CUDA:    {missing_in_cuda}")
    print()
    print(f"TileOp MATCH:       {matched}")
    print(f"TileOp MISMATCH:    {mismatched}")
    print(f"Prime count diff:   {prime_count_mismatches}")
    print()

    print(f"By C++ status:")
    for status in ["normal", "overflow", "empty"]:
        cnt = status_counts.get(status, 0)
        mat = status_match.get(status, 0)
        mis = cnt - mat
        print(f"  {status:10s}: {cnt:5d} total, {mat:5d} match, {mis:5d} mismatch")

    if mismatched > 0:
        print(f"\nMismatch details (showing {len(mismatch_details)}/{mismatched}):")
        for detail in mismatch_details:
            print(detail)

    print()
    if mismatched == 0 and missing_in_cuda == 0:
        print("RESULT: PASS -- all TileOps match byte-for-byte")
        return 0
    elif mismatched == 0:
        print(f"RESULT: PARTIAL -- all compared tiles match, but {missing_in_cuda} missing from CUDA")
        return 0
    else:
        match_pct = 100.0 * matched / (matched + mismatched) if (matched + mismatched) > 0 else 0
        print(f"RESULT: FAIL -- {mismatched} mismatches out of {matched + mismatched} compared ({match_pct:.1f}% match)")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
