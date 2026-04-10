#!/usr/bin/env python3
"""Cross-validate C++ tile-cpp/build/run_tile against Python tile validator.

Primary gate: byte-for-byte TileOp identity + metadata match.
Secondary check: group-bit-steal h1 round-trip.
"""

from __future__ import annotations

import math
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from tile import process_tile
from tileop import (
    TILEOP_SIZE,
    decode_packed_h1,
    is_dead_tileop,
    is_overflow_tileop,
    parse_tileop,
)

CPP_BINARY = os.path.join(os.path.dirname(__file__), "..", "tile-cpp", "build", "run_tile")
TILE_SIDE = 256


# ── Test tile sets ──────────────────────────────────────────────────────────

TILES_R860M = [
    (601040640, 601040640),   # ~45 deg, R~850M
    (850000128, 490746880),   # ~30 deg, R~981M
    (700000000, 700000000),   # ~45 deg, R~990M
    (830000128, 200000000),   # ~13.5 deg, steep angle
    (400000000, 750000000),   # ~62 deg, steep imaginary
    (860000000, 100000000),   # ~6.6 deg, near-real-axis
]

TILES_HIGH_PORT = [
    (601040384, 601040384),
    (601040896, 601040640),
    (700000256, 700000256),
]

TILES_LOW_PORT = [
    (1400000000, 1400000000), # R~1.98B, very sparse
    (1900000000, 100000000),  # R~1.9B, near-axis
]

TILES_PARITY = [
    (601040640, 601040896),   # shifted b
    (601040641, 601040640),   # odd a_lo
    (601040640, 601040641),   # odd b_lo
    (601040641, 601040641),   # both odd
]

ALL_TILES = TILES_R860M + TILES_HIGH_PORT + TILES_LOW_PORT + TILES_PARITY


def run_cpp(a_lo: int, b_lo: int) -> dict | None:
    try:
        result = subprocess.run(
            [CPP_BINARY, str(a_lo), str(b_lo)],
            capture_output=True, text=True, timeout=60,
        )
    except subprocess.TimeoutExpired:
        return None
    if result.returncode != 0:
        return None
    data: dict = {}
    for line in result.stdout.strip().split("\n"):
        if "=" in line:
            key, val = line.split("=", 1)
            data[key.strip()] = val.strip()
    full_hex = data.get("tileop_hex")
    if full_hex:
        data["tileop"] = bytes(int(chunk, 16) for chunk in full_hex.split())
    return data


def run_python(a_lo: int, b_lo: int) -> dict:
    r = process_tile(a_lo, b_lo)
    return {
        "prime_count": r["prime_count"],
        "group_count": r["group_count"],
        "ports_before": r["ports_before_pruning"],
        "ports_after": r["ports_after_pruning"],
        "overflow": r["overflow"],
        "tileop": r["tileop"],
        "tileop_status": r["tileop_status"],
        "ports": r["ports"],
    }


def byte_diff(a: bytes, b: bytes) -> list[str]:
    diffs = []
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            diffs.append(f"  byte[{i}]: C++={a[i]:02x} Py={b[i]:02x}")
    return diffs


def check_h1_round_trip(
    tileop: bytes, a_lo: int, b_lo: int, ports: dict[str, list[dict]]
) -> tuple[int, int, list[str]]:
    """Check group-bit-steal h1 round-trip. Returns (total_checked, failures, details)."""
    if is_overflow_tileop(tileop) or is_dead_tileop(tileop):
        return 0, 0, []

    parsed = parse_tileop(tileop)
    if parsed.h_start is None or parsed.h1_packed is None:
        return 0, 0, []

    total = 0
    failures = 0
    details = []

    for face in ("L", "R"):
        face_ports = ports.get(face, [])
        group_bytes = parsed.groups.get(face, []) if parsed.groups is not None else []
        all_packed = parsed.h1_packed.get(face, [])
        live_count = min(len(face_ports), len(all_packed), len(group_bytes))

        for idx in range(live_count):
            port = face_ports[idx]
            group_byte = group_bytes[idx]
            stored = all_packed[idx]
            original_h1 = port["h1"]
            original_group = port["group"]
            total += 1

            expected_group = ((original_h1 >> 8) << 7) | (original_group & 0x7F)
            expected_stored = original_h1 & 0xFF
            if group_byte != expected_group:
                failures += 1
                details.append(
                    f"{face}[{idx}]: group_byte mismatch stored={group_byte} vs expected={expected_group}"
                )
                continue
            if stored != expected_stored:
                failures += 1
                details.append(
                    f"{face}[{idx}]: h1_byte mismatch stored={stored} vs expected={expected_stored}"
                )
                continue

            decoded_group = group_byte & 0x7F
            if decoded_group != original_group:
                failures += 1
                details.append(
                    f"{face}[{idx}]: group={original_group} -> group_byte={group_byte} -> decoded={decoded_group}"
                )
                continue

            decoded = decode_packed_h1(group_byte, stored)
            if decoded != original_h1:
                failures += 1
                details.append(
                    f"{face}[{idx}]: h1={original_h1} -> group_byte={group_byte} h1_byte={stored} -> decoded={decoded}"
                )

    return total, failures, details


def main():
    if not os.path.isfile(CPP_BINARY):
        print(f"ERROR: C++ binary not found at {CPP_BINARY}")
        sys.exit(1)

    results = []
    encoding_pass = 0
    encoding_fail = 0
    total_h1_checked = 0
    total_h1_failures = 0

    print(f"Cross-validating {len(ALL_TILES)} tiles (C++ vs Python)")
    print(f"{'='*80}")

    for tile_idx, (a_lo, b_lo) in enumerate(ALL_TILES):
        r = math.sqrt(a_lo**2 + b_lo**2)
        angle = math.degrees(math.atan2(b_lo, a_lo))
        tag = f"[{tile_idx+1}/{len(ALL_TILES)}]"

        t0 = time.time()
        cpp = run_cpp(a_lo, b_lo)
        cpp_ms = (time.time() - t0) * 1000

        if cpp is None:
            print(f"{tag} ({a_lo},{b_lo}) -- C++ CRASH")
            results.append({"status": "FAIL", "tile": (a_lo, b_lo)})
            encoding_fail += 1
            continue

        t0 = time.time()
        py = run_python(a_lo, b_lo)
        py_ms = (time.time() - t0) * 1000

        errors = []

        # Metadata
        for key, cpp_key in [("prime_count", "prime_count"), ("group_count", "group_count"),
                              ("ports_before", "ports_before_pruning"), ("ports_after", "ports_after_pruning")]:
            cpp_val = int(cpp.get(cpp_key, -1))
            py_val = py[key]
            if cpp_val != py_val:
                errors.append(f"{key}: C++={cpp_val} Py={py_val}")

        # TileOp bytes
        cpp_tileop = cpp.get("tileop")
        py_tileop = py["tileop"]
        if cpp_tileop is None:
            errors.append("C++ missing tileop_hex")
        elif cpp_tileop != py_tileop:
            diffs = byte_diff(cpp_tileop, py_tileop)
            errors.append(f"TILEOP BYTE MISMATCH ({len(diffs)} bytes differ)")
            for d in diffs[:5]:
                errors.append(d)

        # Status
        cpp_status = cpp.get("tileop_status", "?")
        if cpp_status == "empty":
            cpp_status = "dead"
        if cpp_status != py["tileop_status"]:
            errors.append(f"status: C++={cpp.get('tileop_status','?')} Py={py['tileop_status']}")

        # h1 round-trip (secondary check)
        h1_total, h1_fail, h1_details = check_h1_round_trip(py_tileop, a_lo, b_lo, py["ports"])
        total_h1_checked += h1_total
        total_h1_failures += h1_fail

        # Face counts
        face_summary = {}
        for face in ("O", "I", "L", "R"):
            fp = py["ports"].get(face, [])
            face_summary[face] = len(fp)

        tile_result = {
            "tile": (a_lo, b_lo), "r": r, "angle": angle,
            "cpp_ms": cpp_ms, "py_ms": py_ms,
            "faces": face_summary,
            "h1_total": h1_total, "h1_fail": h1_fail,
            "h1_details": h1_details,
        }

        if errors:
            tile_result["status"] = "FAIL"
            tile_result["errors"] = errors
            encoding_fail += 1
            print(f"{tag} ({a_lo},{b_lo}) R={r/1e9:.3f}B {angle:.1f}deg -- ENCODING FAIL")
            for e in errors:
                print(f"  {e}")
        else:
            tile_result["status"] = "PASS"
            encoding_pass += 1
            f = face_summary
            h1_note = f" h1-roundtrip-failures={h1_fail}/{h1_total}" if h1_fail else ""
            print(
                f"{tag} ({a_lo},{b_lo}) R={r/1e9:.3f}B {angle:.1f}deg -- PASS  "
                f"primes={int(cpp.get('prime_count',0))} groups={int(cpp.get('group_count',0))} "
                f"ports={int(cpp.get('ports_after_pruning',0))} "
                f"O:{f['O']} I:{f['I']} L:{f['L']} R:{f['R']}{h1_note}"
            )

        results.append(tile_result)

    # ── Summary ──
    print(f"\n{'='*80}")
    print("CROSS-VALIDATION RESULTS")
    print(f"{'='*80}")
    print(f"\nPrimary gate (encoding identity):")
    print(f"  Tiles tested:  {len(ALL_TILES)}")
    print(f"  Passed:        {encoding_pass}")
    print(f"  Failed:        {encoding_fail}")

    if encoding_fail > 0:
        print(f"\n  Failed tiles:")
        for r in results:
            if r["status"] == "FAIL":
                print(f"    {r['tile']}: {r.get('errors', [])}")
        verdict = "FAIL"
    else:
        verdict = "PASS"

    print(f"\nSecondary check (group-bit-steal h1 round-trip):")
    print(f"  Total h1 values checked:  {total_h1_checked}")
    print(f"  Round-trip failures:      {total_h1_failures}")
    if total_h1_checked > 0:
        pct = total_h1_failures / total_h1_checked * 100
        print(f"  Failure rate:             {pct:.1f}%")
    print(f"\n  Per-tile h1 breakdown:")
    for r in results:
        if r.get("h1_total", 0) > 0:
            tile = r["tile"]
            print(f"    {tile}: {r['h1_fail']}/{r['h1_total']} failures")

    print(f"\nVERDICT: {verdict}")
    sys.exit(0 if verdict == "PASS" else 1)


if __name__ == "__main__":
    main()
