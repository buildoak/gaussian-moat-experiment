"""Binary dump format I/O for Gaussian Moat tile dumps.

Dump file layout (little-endian throughout):
    uint32_t num_tiles
    Repeated num_tiles times:
        int64_t  a_lo
        int64_t  b_lo
        uint32_t prime_count
        uint8_t  tileop[128]

Each record is exactly 3*4 + 8 + 8 + 4 + 128 = 152 bytes:
    a_lo        : 8 bytes
    b_lo        : 8 bytes
    prime_count : 4 bytes
    tileop      : 128 bytes
Total per record = 148 bytes (+ 4 byte header for num_tiles at file start).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

# ─── constants ──────────────────────────────────────────────────────────────

HEADER_FMT = "<I"          # uint32_t num_tiles
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 4 bytes

RECORD_FMT = "<qqI128s"   # int64 a_lo, int64 b_lo, uint32 prime_count, 128-byte tileop
RECORD_SIZE = struct.calcsize(RECORD_FMT)  # 8+8+4+128 = 148 bytes

TILEOP_SIZE = 128


# ─── metadata ────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class DumpHeader:
    path: Path
    num_tiles: int
    file_size: int

    @property
    def expected_size(self) -> int:
        return HEADER_SIZE + self.num_tiles * RECORD_SIZE

    @property
    def size_ok(self) -> bool:
        return self.file_size == self.expected_size


def read_header(path: str | Path) -> DumpHeader:
    """Read and validate the dump file header without loading all tiles."""
    p = Path(path)
    file_size = p.stat().st_size
    with p.open("rb") as f:
        raw = f.read(HEADER_SIZE)
    if len(raw) < HEADER_SIZE:
        raise ValueError(f"File too short for header: {p}")
    (num_tiles,) = struct.unpack(HEADER_FMT, raw)
    return DumpHeader(path=p, num_tiles=num_tiles, file_size=file_size)


# ─── readers ─────────────────────────────────────────────────────────────────

def read_dump(path: str | Path) -> list[dict]:
    """Load all tile records from a dump file.

    Returns a list of dicts with keys:
        a_lo        : int   (signed 64-bit tile coordinate)
        b_lo        : int   (signed 64-bit tile coordinate)
        prime_count : int   (unsigned 32-bit, semantics differ between CUDA/C++)
        tileop      : bytes (128 bytes)
    """
    p = Path(path)
    with p.open("rb") as f:
        raw_header = f.read(HEADER_SIZE)
        if len(raw_header) < HEADER_SIZE:
            raise ValueError(f"Truncated header in {p}")
        (num_tiles,) = struct.unpack(HEADER_FMT, raw_header)

        records: list[dict] = []
        for i in range(num_tiles):
            raw = f.read(RECORD_SIZE)
            if len(raw) < RECORD_SIZE:
                raise ValueError(
                    f"Truncated record {i} (expected {RECORD_SIZE} bytes, got {len(raw)}) in {p}"
                )
            a_lo, b_lo, prime_count, tileop = struct.unpack(RECORD_FMT, raw)
            records.append(
                {
                    "a_lo": a_lo,
                    "b_lo": b_lo,
                    "prime_count": prime_count,
                    "tileop": tileop,
                }
            )
    return records


def read_dump_iter(path: str | Path) -> Iterator[dict]:
    """Streaming version of read_dump for large files.

    Yields one dict per tile without loading all records into memory.
    Dict keys: a_lo, b_lo, prime_count, tileop (same as read_dump).
    """
    p = Path(path)
    with p.open("rb") as f:
        raw_header = f.read(HEADER_SIZE)
        if len(raw_header) < HEADER_SIZE:
            raise ValueError(f"Truncated header in {p}")
        (num_tiles,) = struct.unpack(HEADER_FMT, raw_header)

        for i in range(num_tiles):
            raw = f.read(RECORD_SIZE)
            if len(raw) < RECORD_SIZE:
                raise ValueError(
                    f"Truncated record {i} (expected {RECORD_SIZE} bytes, got {len(raw)}) in {p}"
                )
            a_lo, b_lo, prime_count, tileop = struct.unpack(RECORD_FMT, raw)
            yield {
                "a_lo": a_lo,
                "b_lo": b_lo,
                "prime_count": prime_count,
                "tileop": tileop,
            }


# ─── writer ──────────────────────────────────────────────────────────────────

def write_dump(path: str | Path, records: list[dict]) -> None:
    """Write a list of tile records to a dump file.

    Each record must have: a_lo (int), b_lo (int), prime_count (int), tileop (bytes, 128 bytes).
    """
    p = Path(path)
    num_tiles = len(records)
    with p.open("wb") as f:
        f.write(struct.pack(HEADER_FMT, num_tiles))
        for i, rec in enumerate(records):
            tileop = rec["tileop"]
            if len(tileop) != TILEOP_SIZE:
                raise ValueError(
                    f"Record {i}: tileop must be {TILEOP_SIZE} bytes, got {len(tileop)}"
                )
            f.write(
                struct.pack(
                    RECORD_FMT,
                    rec["a_lo"],
                    rec["b_lo"],
                    rec["prime_count"],
                    tileop,
                )
            )
