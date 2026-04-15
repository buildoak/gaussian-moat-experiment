"""CUDA vs C++ tile dump comparison.

Usage:
    python compare.py <cuda_dump.bin> <cpp_dump.bin> [--verbose]

Exit codes:
    0  All tiles match (exact or permutation-equivalent)
    1  One or more tiles have structural mismatches
    2  Fatal error (file I/O, incompatible tile counts, etc.)

Comparison levels (per tile pair):
    EXACT       Byte-for-byte identical TileOps
    PERM_MATCH  Headers match; group bytes differ by a consistent label bijection
    MISMATCH    Headers differ, or group structure can't be reconciled by any bijection

Note on prime_count:
    CUDA counts sieve-domain primes including the collar region.
    C++ counts tile-proper primes only.
    prime_count differences are EXPECTED and never treated as errors.
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

# ─── inline TileOp constants (mirrors tile-validator/tileop.py) ──────────────

TILEOP_SIZE = 128
HEADER_BYTES = 3
PAYLOAD_BUDGET = 125
OVERFLOW_BYTE = 0xFF
EMPTY_OFFSET = 3
FACES = ("O", "I", "L", "R")


# ─── TileOp helpers ──────────────────────────────────────────────────────────

def _is_overflow(tileop: bytes) -> bool:
    return tileop == bytes([OVERFLOW_BYTE] * TILEOP_SIZE)


def _is_dead(tileop: bytes) -> bool:
    return (
        not _is_overflow(tileop)
        and tileop[0] == EMPTY_OFFSET
        and tileop[1] == EMPTY_OFFSET
        and tileop[2] == EMPTY_OFFSET
        and tileop[3] == 0
    )


def _parse_tileop(tileop: bytes) -> dict | None:
    """Return parsed header and group lists, or None for overflow/dead.

    Returns dict with:
        off_I, off_L, off_R  : header offsets
        groups               : dict[face -> list[int]] of raw group bytes
    """
    if _is_overflow(tileop) or _is_dead(tileop):
        return None

    off_I = tileop[0]
    off_L = tileop[1]
    off_R = tileop[2]

    if not (HEADER_BYTES <= off_I <= off_L <= off_R <= 127):
        return None  # structurally invalid

    o_cnt = off_I - HEADER_BYTES
    i_cnt = off_L - off_I
    l_cnt = off_R - off_L
    residual = PAYLOAD_BUDGET - o_cnt - i_cnt - 2 * l_cnt
    if residual < 0:
        return None
    r_cnt = residual >> 1

    r_groups_raw = list(tileop[off_R : off_R + r_cnt])
    live_r = len(r_groups_raw)
    while live_r > 0 and r_groups_raw[live_r - 1] == 0:
        live_r -= 1

    return {
        "off_I": off_I,
        "off_L": off_L,
        "off_R": off_R,
        "groups": {
            "O": list(tileop[HEADER_BYTES:off_I]),
            "I": list(tileop[off_I:off_L]),
            "L": list(tileop[off_L:off_R]),
            "R": r_groups_raw[:live_r],
        },
    }


def _decode_group_id(raw_byte: int, face: str) -> int:
    """Extract the 7-bit group ID from a raw group byte."""
    if face in ("L", "R"):
        return raw_byte & 0x7F
    return raw_byte


def _decode_h1_msb(raw_byte: int) -> int:
    """Extract the h1 MSB (bit 8) packed into the high bit of an L/R group byte."""
    return (raw_byte >> 7) & 1


# ─── comparison logic ────────────────────────────────────────────────────────

def _compare_tileops(cuda_to: bytes, cpp_to: bytes, idx: int, verbose: bool) -> dict:
    """Compare a single CUDA vs C++ TileOp.

    Returns dict:
        result  : "EXACT" | "PERM_MATCH" | "MISMATCH"
        detail  : human-readable string (empty for EXACT)
    """
    # Fast path: identical bytes
    if cuda_to == cpp_to:
        return {"result": "EXACT", "detail": ""}

    # Classify both
    cuda_overflow = _is_overflow(cuda_to)
    cpp_overflow = _is_overflow(cpp_to)
    cuda_dead = _is_dead(cuda_to)
    cpp_dead = _is_dead(cpp_to)

    if cuda_overflow != cpp_overflow:
        return {
            "result": "MISMATCH",
            "detail": f"overflow mismatch: CUDA={'overflow' if cuda_overflow else 'normal'} "
                      f"CPP={'overflow' if cpp_overflow else 'normal'}",
        }
    if cuda_dead != cpp_dead:
        return {
            "result": "MISMATCH",
            "detail": f"dead mismatch: CUDA={'dead' if cuda_dead else 'normal'} "
                      f"CPP={'dead' if cpp_dead else 'normal'}",
        }
    if cuda_overflow:  # both overflow but bytes differ — shouldn't happen; treat as mismatch
        return {"result": "MISMATCH", "detail": "both overflow but bytes differ (corrupt)"}
    if cuda_dead:      # both dead but bytes differ — trailing garbage
        return {"result": "MISMATCH", "detail": "both dead but bytes differ (trailing garbage)"}

    cuda_parsed = _parse_tileop(cuda_to)
    cpp_parsed = _parse_tileop(cpp_to)

    if cuda_parsed is None or cpp_parsed is None:
        return {"result": "MISMATCH", "detail": "at least one TileOp failed structural parse"}

    # Headers must agree exactly
    for field in ("off_I", "off_L", "off_R"):
        cv, pv = cuda_parsed[field], cpp_parsed[field]
        if cv != pv:
            return {
                "result": "MISMATCH",
                "detail": f"header field {field} differs: CUDA={cv} CPP={pv}",
            }

    # Per-face: try to find a consistent group label bijection
    cuda_groups = cuda_parsed["groups"]
    cpp_groups = cpp_parsed["groups"]

    # Port counts per face must match
    for face in FACES:
        cg = cuda_groups[face]
        pg = cpp_groups[face]
        if len(cg) != len(pg):
            return {
                "result": "MISMATCH",
                "detail": f"face {face} port count differs: CUDA={len(cg)} CPP={len(pg)}",
            }

    # For L/R faces we also require h1 MSBs to match position-by-position
    # (bijection only remaps group IDs, not h1 data)
    for face in ("L", "R"):
        for pos, (cb, pb) in enumerate(zip(cuda_groups[face], cpp_groups[face])):
            c_msb = _decode_h1_msb(cb)
            p_msb = _decode_h1_msb(pb)
            if c_msb != p_msb:
                return {
                    "result": "MISMATCH",
                    "detail": f"face {face} port {pos}: h1 MSB differs (CUDA={c_msb} CPP={p_msb})",
                }

    # Build a candidate bijection: CUDA group_id -> CPP group_id
    # across all faces simultaneously
    bijection: dict[int, int] = {}   # cuda_id -> cpp_id
    reverse: dict[int, int] = {}     # cpp_id -> cuda_id

    for face in FACES:
        for cb, pb in zip(cuda_groups[face], cpp_groups[face]):
            c_id = _decode_group_id(cb, face)
            p_id = _decode_group_id(pb, face)

            if c_id in bijection:
                if bijection[c_id] != p_id:
                    return {
                        "result": "MISMATCH",
                        "detail": (
                            f"face {face}: group bijection conflict — "
                            f"CUDA {c_id} maps to both CPP {bijection[c_id]} and {p_id}"
                        ),
                    }
            else:
                if p_id in reverse and reverse[p_id] != c_id:
                    return {
                        "result": "MISMATCH",
                        "detail": (
                            f"face {face}: group bijection conflict (reverse) — "
                            f"CPP {p_id} claimed by both CUDA {reverse[p_id]} and {c_id}"
                        ),
                    }
                bijection[c_id] = p_id
                reverse[p_id] = c_id

    # Bijection is consistent — it's a permutation match
    if bijection:
        detail = f"label bijection: {bijection}" if verbose else f"{len(bijection)} group(s) relabeled"
    else:
        detail = "headers match, no groups (trivially equal)"
    return {"result": "PERM_MATCH", "detail": detail}


# ─── main ────────────────────────────────────────────────────────────────────

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare CUDA vs C++ Gaussian Moat tile dumps."
    )
    parser.add_argument("cuda_dump", help="CUDA binary dump (.bin)")
    parser.add_argument("cpp_dump", help="C++ binary dump (.bin)")
    parser.add_argument(
        "--verbose", "-v", action="store_true",
        help="Print per-tile results including EXACT matches",
    )
    parser.add_argument(
        "--max-mismatch", type=int, default=20, metavar="N",
        help="Stop after N mismatches (0 = no limit, default 20)",
    )
    args = parser.parse_args(argv)

    # Import here so the module can be imported standalone without dump_io on sys.path
    try:
        from dump_io import read_dump
    except ImportError:
        sys.path.insert(0, str(Path(__file__).parent))
        from dump_io import read_dump  # type: ignore[no-redef]

    try:
        print(f"Reading CUDA dump: {args.cuda_dump}")
        cuda_records = read_dump(args.cuda_dump)
        print(f"Reading C++ dump:  {args.cpp_dump}")
        cpp_records = read_dump(args.cpp_dump)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    if len(cuda_records) != len(cpp_records):
        print(
            f"ERROR: tile count mismatch — CUDA has {len(cuda_records)}, C++ has {len(cpp_records)}",
            file=sys.stderr,
        )
        return 2

    n = len(cuda_records)
    print(f"Comparing {n} tile pairs...\n")

    results: Counter[str] = Counter()
    mismatches_shown = 0

    for i, (cuda_rec, cpp_rec) in enumerate(zip(cuda_records, cpp_records)):
        # Coordinates must match — same input, same order
        if cuda_rec["a_lo"] != cpp_rec["a_lo"] or cuda_rec["b_lo"] != cpp_rec["b_lo"]:
            print(
                f"FATAL tile {i}: coordinate mismatch "
                f"CUDA=({cuda_rec['a_lo']}, {cuda_rec['b_lo']}) "
                f"CPP=({cpp_rec['a_lo']}, {cpp_rec['b_lo']})",
                file=sys.stderr,
            )
            return 2

        cmp = _compare_tileops(cuda_rec["tileop"], cpp_rec["tileop"], i, args.verbose)
        result = cmp["result"]
        results[result] += 1

        if result == "MISMATCH":
            mismatches_shown += 1
            print(
                f"  [{i:>6}] ({cuda_rec['a_lo']:+d}, {cuda_rec['b_lo']:+d})  "
                f"MISMATCH — {cmp['detail']}"
            )
            if args.max_mismatch and mismatches_shown >= args.max_mismatch:
                remaining = n - i - 1
                print(f"  ... stopping after {args.max_mismatch} mismatches ({remaining} tiles not checked)")
                break
        elif result == "PERM_MATCH":
            print(
                f"  [{i:>6}] ({cuda_rec['a_lo']:+d}, {cuda_rec['b_lo']:+d})  "
                f"PERM_MATCH — {cmp['detail']}"
            )
        elif args.verbose:
            print(
                f"  [{i:>6}] ({cuda_rec['a_lo']:+d}, {cuda_rec['b_lo']:+d})  EXACT"
            )

    # ─── summary ─────────────────────────────────────────────────────────────
    total_checked = sum(results.values())
    print()
    print("=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"  Total tiles      : {n}")
    print(f"  Tiles checked    : {total_checked}")
    print(f"  EXACT            : {results['EXACT']}")
    print(f"  PERM_MATCH       : {results['PERM_MATCH']}")
    print(f"  MISMATCH         : {results['MISMATCH']}")
    print()
    print("  (prime_count differences are ignored — expected between CUDA and C++)")
    print()

    if results["MISMATCH"] == 0:
        print("RESULT: OK — all tiles match (exact or permutation-equivalent)")
        return 0
    else:
        print(f"RESULT: FAIL — {results['MISMATCH']} tile(s) mismatched")
        return 1


if __name__ == "__main__":
    sys.exit(main())
