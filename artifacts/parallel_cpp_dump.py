#!/usr/bin/env python3
"""
parallel_cpp_dump.py — Split a coord file into 12 chunks, run cpp_dump in
parallel, merge outputs.

Usage:
    python3 parallel_cpp_dump.py coords_85deg.bin cpp_85deg.bin [--workers 12]

Coord format: uint32 num_tiles, then num_tiles * (int64 a_lo, int64 b_lo) = 16 bytes each.
Output format: uint32 num_tiles, then num_tiles * (int64 a_lo, int64 b_lo, uint32 prime_count, uint8 tileop[128]) = 148 bytes each.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time

COORD_RECORD_SIZE = 16   # int64 a_lo + int64 b_lo
OUTPUT_RECORD_SIZE = 148  # int64 a_lo + int64 b_lo + uint32 prime_count + uint8[128]
HEADER_SIZE = 4           # uint32 num_tiles

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CPP_DUMP_BIN = os.path.join(SCRIPT_DIR, "tile-cuda", "cpp_dump")


def split_coords(coord_path: str, num_workers: int, tmpdir: str) -> list[tuple[str, int]]:
    """Split coord file into num_workers chunk files. Returns list of (chunk_path, num_tiles)."""
    with open(coord_path, "rb") as f:
        header = f.read(HEADER_SIZE)
        total_tiles = struct.unpack("<I", header)[0]
        all_records = f.read()

    assert len(all_records) == total_tiles * COORD_RECORD_SIZE, (
        f"Expected {total_tiles * COORD_RECORD_SIZE} bytes, got {len(all_records)}"
    )

    base_chunk = total_tiles // num_workers
    remainder = total_tiles % num_workers

    chunks = []
    offset = 0
    for i in range(num_workers):
        chunk_size = base_chunk + (1 if i < remainder else 0)
        chunk_path = os.path.join(tmpdir, f"chunk_{i:02d}_coords.bin")
        out_path = os.path.join(tmpdir, f"chunk_{i:02d}_output.bin")

        with open(chunk_path, "wb") as cf:
            cf.write(struct.pack("<I", chunk_size))
            cf.write(all_records[offset : offset + chunk_size * COORD_RECORD_SIZE])

        offset += chunk_size * COORD_RECORD_SIZE
        chunks.append((chunk_path, out_path, chunk_size))

    assert offset == len(all_records), "Didn't consume all records"
    print(f"  Split {total_tiles} tiles into {num_workers} chunks: "
          f"{[c[2] for c in chunks]}")
    return chunks, total_tiles


def run_parallel(chunks: list, total_tiles: int) -> list[subprocess.Popen]:
    """Launch all cpp_dump processes in parallel."""
    procs = []
    for coord_path, out_path, _ in chunks:
        proc = subprocess.Popen(
            [CPP_DUMP_BIN, "-c", coord_path, "-o", out_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        procs.append(proc)
    return procs


def wait_and_check(procs: list, chunks: list):
    """Wait for all processes and check for errors."""
    for i, proc in enumerate(procs):
        stdout, stderr = proc.communicate()
        if proc.returncode != 0:
            print(f"  ERROR: Worker {i} failed (rc={proc.returncode})")
            print(f"  stderr: {stderr.decode()}")
            sys.exit(1)
        # Print progress from stderr (cpp_dump writes progress there)
        lines = stderr.decode().strip().split("\n")
        # Just print the summary line (last one)
        for line in lines:
            if "wrote" in line:
                print(f"  Worker {i:2d}: {line.strip()}")


def merge_outputs(chunks: list, total_tiles: int, output_path: str):
    """Merge chunk output files into single output file with correct header."""
    with open(output_path, "wb") as out:
        # Write total header
        out.write(struct.pack("<I", total_tiles))

        for _, out_path, expected_tiles in chunks:
            with open(out_path, "rb") as inp:
                # Read and verify chunk header
                chunk_header = inp.read(HEADER_SIZE)
                chunk_tiles = struct.unpack("<I", chunk_header)[0]
                assert chunk_tiles == expected_tiles, (
                    f"Chunk header says {chunk_tiles}, expected {expected_tiles}"
                )
                # Copy all records
                data = inp.read()
                expected_size = expected_tiles * OUTPUT_RECORD_SIZE
                assert len(data) == expected_size, (
                    f"Chunk data {len(data)} bytes, expected {expected_size}"
                )
                out.write(data)

    actual_size = os.path.getsize(output_path)
    expected_size = HEADER_SIZE + total_tiles * OUTPUT_RECORD_SIZE
    assert actual_size == expected_size, (
        f"Output size {actual_size}, expected {expected_size}"
    )
    print(f"  Merged: {output_path} ({actual_size:,} bytes)")


def spot_check(output_path: str, num_samples: int = 5):
    """Read a few records and print prime_counts as sanity check."""
    with open(output_path, "rb") as f:
        header = f.read(HEADER_SIZE)
        total = struct.unpack("<I", header)[0]

        # Check tiles at evenly spaced positions
        positions = [int(i * total / num_samples) for i in range(num_samples)]
        for pos in positions:
            f.seek(HEADER_SIZE + pos * OUTPUT_RECORD_SIZE)
            record = f.read(OUTPUT_RECORD_SIZE)
            a_lo, b_lo = struct.unpack_from("<qq", record, 0)
            prime_count = struct.unpack_from("<I", record, 16)[0]
            print(f"  Tile {pos:6d}: a_lo={a_lo}, b_lo={b_lo}, "
                  f"prime_count={prime_count}")


def main():
    parser = argparse.ArgumentParser(description="Parallel cpp_dump runner")
    parser.add_argument("coords", help="Input coord .bin file")
    parser.add_argument("output", help="Output .bin file")
    parser.add_argument("--workers", type=int, default=12, help="Number of parallel workers")
    args = parser.parse_args()

    if not os.path.isfile(CPP_DUMP_BIN):
        print(f"ERROR: cpp_dump not found at {CPP_DUMP_BIN}")
        sys.exit(1)

    coord_path = os.path.join(SCRIPT_DIR, args.coords) if not os.path.isabs(args.coords) else args.coords
    output_path = os.path.join(SCRIPT_DIR, args.output) if not os.path.isabs(args.output) else args.output

    print(f"=== Parallel cpp_dump: {args.coords} -> {args.output} ({args.workers} workers) ===")

    with tempfile.TemporaryDirectory(prefix="cpp_dump_") as tmpdir:
        # Split
        t0 = time.time()
        chunks, total_tiles = split_coords(coord_path, args.workers, tmpdir)
        t_split = time.time() - t0
        print(f"  Split time: {t_split:.2f}s")

        # Run parallel
        t1 = time.time()
        procs = run_parallel(chunks, total_tiles)
        print(f"  Launched {len(procs)} workers...")
        wait_and_check(procs, chunks)
        t_compute = time.time() - t1
        print(f"  Compute time: {t_compute:.1f}s ({total_tiles / t_compute:.0f} tiles/s)")

        # Merge
        t2 = time.time()
        merge_outputs(chunks, total_tiles, output_path)
        t_merge = time.time() - t2
        print(f"  Merge time: {t_merge:.2f}s")

    # Spot check
    print(f"  Spot check:")
    spot_check(output_path)

    total_time = time.time() - t0
    print(f"  Total: {total_time:.1f}s")
    print()


if __name__ == "__main__":
    main()
