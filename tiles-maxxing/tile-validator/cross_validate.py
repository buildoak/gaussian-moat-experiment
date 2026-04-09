#!/usr/bin/env python3
"""Cross-validate C++ tile-cpp/build/run_tile against Python tile validator
on N random tiles with R in [R_MIN, R_MAX]."""

import math
import os
import random
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from tile import process_tile
from tileop import decode_tileop, parse_tileop

# --- Config ---
N_TILES = 500
R_MIN = 850_000_000
R_MAX = 1_500_000_000
TILE_SIDE = 256
CPP_BINARY = os.path.join(os.path.dirname(__file__), "..", "tile-cpp", "build", "run_tile")
SEED = 42

def generate_tile_coords(n, r_min, r_max, seed):
    """Generate n random tile origins (a_lo, b_lo) with R in [r_min, r_max].
    Both must be multiples of TILE_SIDE, positive, off-axis."""
    rng = random.Random(seed)
    coords = []
    while len(coords) < n:
        # Random angle 5-85 degrees (avoid axis)
        angle = rng.uniform(math.radians(5), math.radians(85))
        # Random radius
        r = rng.uniform(r_min, r_max)
        a = r * math.cos(angle)
        b = r * math.sin(angle)
        # Snap to TILE_SIDE grid
        a_lo = int(a) // TILE_SIDE * TILE_SIDE
        b_lo = int(b) // TILE_SIDE * TILE_SIDE
        if a_lo <= 0 or b_lo <= 0:
            continue
        # Verify radius
        actual_r = math.sqrt(a_lo**2 + b_lo**2)
        if actual_r < r_min * 0.95 or actual_r > r_max * 1.05:
            continue
        coords.append((a_lo, b_lo))
    return coords

def run_cpp(a_lo, b_lo):
    """Run C++ run_tile and parse output."""
    result = subprocess.run(
        [CPP_BINARY, str(a_lo), str(b_lo)],
        capture_output=True, text=True, timeout=30
    )
    if result.returncode != 0:
        return None
    data = {}
    for line in result.stdout.strip().split("\n"):
        if "=" in line:
            key, val = line.split("=", 1)
            data[key.strip()] = val.strip()
    full_hex = data.get("tileop_hex")
    if full_hex:
        data["tileop"] = bytes(int(chunk, 16) for chunk in full_hex.split())
    return data

def run_python(a_lo, b_lo):
    """Run Python process_tile and extract key metrics."""
    r = process_tile(a_lo, b_lo)
    return {
        "prime_count": r["prime_count"],
        "group_count": r["group_count"],
        "ports_before": r["ports_before_pruning"],
        "ports_after": r["ports_after_pruning"],
        "overflow": r["overflow"],
        "tileop": r["tileop"],
        "tileop_status": r["tileop_status"],
        "tileop_decoded": r["tileop_decoded"],
        "tileop_offsets": r["tileop_offsets"],
        "tileop_counts": r["tileop_counts"],
    }

def main():
    print(f"Generating {N_TILES} random tiles with R in [{R_MIN/1e9:.2f}B, {R_MAX/1e9:.2f}B]...")
    coords = generate_tile_coords(N_TILES, R_MIN, R_MAX, SEED)
    print(f"Generated {len(coords)} tiles. Seed={SEED}")

    # Verify C++ binary exists
    if not os.path.isfile(CPP_BINARY):
        print(f"ERROR: C++ binary not found at {CPP_BINARY}")
        sys.exit(1)

    mismatches = []
    total_cpp_time = 0
    total_py_time = 0

    for i, (a_lo, b_lo) in enumerate(coords):
        r = math.sqrt(a_lo**2 + b_lo**2)
        angle = math.degrees(math.atan2(b_lo, a_lo))

        # Run C++
        t0 = time.time()
        cpp = run_cpp(a_lo, b_lo)
        cpp_time = time.time() - t0
        total_cpp_time += cpp_time

        if cpp is None:
            print(f"[{i+1}/{N_TILES}] FAIL C++ crashed: ({a_lo}, {b_lo})")
            mismatches.append((a_lo, b_lo, "C++ crash", None, None))
            continue

        # Run Python
        t0 = time.time()
        py = run_python(a_lo, b_lo)
        py_time = time.time() - t0
        total_py_time += py_time

        # Compare
        cpp_prime = int(cpp.get("prime_count", -1))
        cpp_group = int(cpp.get("group_count", -1))
        cpp_ports_before = int(cpp.get("ports_before_pruning", -1))
        cpp_ports_after = int(cpp.get("ports_after_pruning", -1))

        py_prime = py["prime_count"]
        py_group = py["group_count"]
        py_ports_before = py["ports_before"]
        py_ports_after = py["ports_after"]

        # Check overflow via tileop
        cpp_tileop = cpp.get("tileop")
        if cpp_tileop is None:
            mismatches.append((a_lo, b_lo, ["C++ output missing full tileop_hex"], cpp, py))
            print(f"[{i+1}/{N_TILES}] MISMATCH ({a_lo},{b_lo}) missing full tileop_hex in C++ output")
            continue
        cpp_overflow = cpp_tileop[0] == 0xFF
        py_overflow = py["overflow"]
        cpp_parsed = parse_tileop(cpp_tileop)
        py_parsed = parse_tileop(py["tileop"])

        errors = []
        if cpp_prime != py_prime:
            errors.append(f"prime_count: C++={cpp_prime} Py={py_prime}")
        if cpp_group != py_group:
            errors.append(f"group_count: C++={cpp_group} Py={py_group}")
        if cpp_ports_before != py_ports_before:
            errors.append(f"ports_before: C++={cpp_ports_before} Py={py_ports_before}")
        if cpp_ports_after != py_ports_after:
            errors.append(f"ports_after: C++={cpp_ports_after} Py={py_ports_after}")
        if cpp_overflow != py_overflow:
            errors.append(f"overflow: C++={cpp_overflow} Py={py_overflow}")
        if cpp_tileop != py["tileop"]:
            errors.append("tileop_full MISMATCH")
        if cpp_parsed.status != py["tileop_status"]:
            errors.append(f"status: C++={cpp_parsed.status} Py={py['tileop_status']}")
        if cpp_parsed.off_I != py_parsed.off_I or cpp_parsed.off_L != py_parsed.off_L or cpp_parsed.off_R != py_parsed.off_R:
            errors.append("header offsets mismatch")

        if errors:
            mismatches.append((a_lo, b_lo, errors, cpp, py))
            print(f"[{i+1}/{N_TILES}] MISMATCH ({a_lo},{b_lo}) R={r/1e9:.3f}B {angle:.1f}deg: {'; '.join(errors)}")
        elif (i + 1) % 50 == 0 or i == 0:
            print(f"[{i+1}/{N_TILES}] OK  R={r/1e9:.3f}B {angle:.1f}deg  primes={cpp_prime} groups={cpp_group} ports={cpp_ports_after} cpp={cpp_time*1000:.0f}ms py={py_time*1000:.0f}ms")

    # Summary
    print(f"\n{'='*60}")
    print(f"CROSS-VALIDATION SUMMARY")
    print(f"{'='*60}")
    print(f"Tiles tested:     {N_TILES}")
    print(f"Radius range:     [{R_MIN/1e9:.2f}B, {R_MAX/1e9:.2f}B]")
    print(f"Seed:             {SEED}")
    print(f"C++ total time:   {total_cpp_time:.1f}s ({total_cpp_time/N_TILES*1000:.1f}ms/tile)")
    print(f"Python total time:{total_py_time:.1f}s ({total_py_time/N_TILES*1000:.1f}ms/tile)")
    print(f"Mismatches:       {len(mismatches)}/{N_TILES}")

    if mismatches:
        print(f"\nMISMATCH DETAILS:")
        for a_lo, b_lo, errs, cpp, py in mismatches:
            print(f"  ({a_lo}, {b_lo}): {errs}")
        print(f"\nVERDICT: FAIL — {len(mismatches)} mismatches found")
        sys.exit(1)
    else:
        print(f"\nVERDICT: PASS — all {N_TILES} tiles match exactly")
        sys.exit(0)

if __name__ == "__main__":
    main()
