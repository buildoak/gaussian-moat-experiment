#!/usr/bin/env python3
"""TileOp v2 structural diagnostic for a single operating-point tile."""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

from analysis import category_histogram, classify_surviving_groups, packed_budget_counts
from tile import DEFAULT_K, process_tile
from tileop import face_h1_parity


def diagnose(a_lo: int, b_lo: int, k_sq: int = DEFAULT_K) -> None:
    result = process_tile(a_lo, b_lo, k_sq)
    classifications = classify_surviving_groups(result["ports"])
    budget = packed_budget_counts(result["ports"])

    print(f"tile ({a_lo}, {b_lo}) status={result['tileop_status']} overflow={result['overflow']}")
    print(
        f"  primes={result['prime_count']} groups={result['group_count']} "
        f"ports_before={result['ports_before_pruning']} ports_after={result['ports_after_pruning']}"
    )
    print(
        "  offsets="
        f"{result['tileop_offsets']} counts={result['tileop_counts']} "
        f"packed_bytes_used={budget['packed_bytes_used']} slack={budget['packed_bytes_slack']}"
    )
    print(
        "  face_counts="
        + ", ".join(f"{face}={len(result['ports'][face])}" for face in ("I", "O", "L", "R"))
    )
    print(
        "  face_parity="
        f"L:{face_h1_parity('L', (a_lo, b_lo))} "
        f"R:{face_h1_parity('R', (a_lo, b_lo))}"
    )
    for face in ("I", "O", "L", "R"):
        decoded = result["tileop_decoded"][face]
        print(
            f"  {face}: groups={decoded['groups']}"
            + (f" h1_packed={decoded['h1_packed']} h1={decoded['h1']}" if face in ('L', 'R') else "")
        )
    print(f"  classifications={category_histogram(classifications)}")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: python3 diagnose_tile.py <a_lo> <b_lo>")
        return 1
    diagnose(int(argv[1]), int(argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
