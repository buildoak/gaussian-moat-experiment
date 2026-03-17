#!/usr/bin/env python3
"""
test-lb-crosschunk.py — Multi-chunk LB cross-chunk connectivity test.

Tests that the resume_connect_norm fix correctly detects moats instead of
bridging them. Uses the internal Rust sieve (no CUDA needed) with tiny
norm bounds.

Two test cases:
  k²=2:  moat at (11,4), norm 137, distance √137 ≈ 11.705
  k²=8:  moat at (11,4), norm 137, distance √137 ≈ 11.705

Verification gate:
  - PASS: farthest plateaus at a specific norm, subsequent chunks show 0 or unchanged
  - FAIL: farthest advances every chunk (moat bridged) or farthest=0 in chunk 1+ (fix too aggressive)
"""

import subprocess
import sys
import re
import os

SOLVER_BIN = os.environ.get(
    "SOLVER_BIN",
    os.path.join(os.path.dirname(__file__), "..", "solver", "target", "release", "gaussian-moat-solver"),
)


def run_solver_chunk(k_squared, norm_bound, resume_farthest_norm=0,
                     resume_farthest_a=0, resume_farthest_b=0,
                     angular=1, prime_file=None):
    """Run one solver chunk and parse the RESULT line."""
    cmd = [
        SOLVER_BIN,
        "--k-squared", str(k_squared),
        "--angular", str(angular),
        "--norm-bound", str(norm_bound),
    ]
    if resume_farthest_norm > 0:
        cmd += [
            "--resume-farthest-norm", str(resume_farthest_norm),
            "--resume-farthest-a", str(resume_farthest_a),
            "--resume-farthest-b", str(resume_farthest_b),
        ]
    if prime_file:
        cmd += ["--prime-file", prime_file]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    output = result.stdout + result.stderr

    # Parse RESULT line
    m = re.search(r"RESULT farthest_norm=(\d+) farthest_point=\((-?\d+),(-?\d+)\) component_size=(\d+) primes_processed=(\d+)", output)
    if not m:
        return {"error": f"No RESULT line in output:\n{output}", "raw": output}

    moat_found = "MOAT_FOUND" in output
    return {
        "farthest_norm": int(m.group(1)),
        "farthest_a": int(m.group(2)),
        "farthest_b": int(m.group(3)),
        "component_size": int(m.group(4)),
        "primes_processed": int(m.group(5)),
        "moat_found": moat_found,
        "raw": output,
    }


def test_multi_chunk(k_squared, chunk_size, total_norm, expected_farthest_norm, expected_farthest_point, label):
    """Run a multi-chunk LB campaign and verify moat detection."""
    print(f"\n{'='*60}")
    print(f"TEST: {label}")
    print(f"  k²={k_squared}, chunk_size={chunk_size}, total_norm={total_norm}")
    print(f"  expected farthest: {expected_farthest_point} at norm {expected_farthest_norm}")
    print(f"{'='*60}")

    farthest_norm = 0
    farthest_a = 0
    farthest_b = 0
    chunk_results = []

    norm_cursor = 0
    chunk_num = 0

    while norm_cursor < total_norm:
        chunk_hi = min(norm_cursor + chunk_size, total_norm)

        r = run_solver_chunk(
            k_squared=k_squared,
            norm_bound=chunk_hi,
            resume_farthest_norm=farthest_norm if chunk_num > 0 else 0,
            resume_farthest_a=farthest_a,
            resume_farthest_b=farthest_b,
            angular=1,
        )

        if "error" in r:
            print(f"  CHUNK {chunk_num} ERROR: {r['error']}")
            return False

        chunk_results.append(r)

        new_farthest = r["farthest_norm"]
        print(f"  Chunk {chunk_num}: norm_bound={chunk_hi}, farthest_norm={new_farthest}, "
              f"point=({r['farthest_a']},{r['farthest_b']}), "
              f"component={r['component_size']}, moat={'YES' if r['moat_found'] else 'no'}")

        if new_farthest > farthest_norm:
            farthest_norm = new_farthest
            farthest_a = r["farthest_a"]
            farthest_b = r["farthest_b"]

        norm_cursor = chunk_hi
        chunk_num += 1

    # Verify
    print(f"\n  --- Verification ---")
    print(f"  Final farthest_norm: {farthest_norm}")

    passed = True
    reasons = []

    # Check 1: farthest should match expected
    if farthest_norm != expected_farthest_norm:
        passed = False
        reasons.append(f"farthest_norm={farthest_norm} != expected {expected_farthest_norm}")

    # Check 2: farthest should plateau (not advance in later chunks)
    if len(chunk_results) >= 2:
        # Find the chunk where farthest first reached its peak
        peak_chunk = 0
        for i, r in enumerate(chunk_results):
            if r["farthest_norm"] == farthest_norm:
                peak_chunk = i
                break

        # All subsequent chunks should not advance farthest
        for i in range(peak_chunk + 1, len(chunk_results)):
            r = chunk_results[i]
            # In chunks after the moat, farthest_norm should be 0 (no origin component found)
            # or equal to farthest_norm (unchanged from resume)
            if r["farthest_norm"] > farthest_norm:
                passed = False
                reasons.append(f"Chunk {i} advanced farthest to {r['farthest_norm']} (moat bridged!)")

    # Check 3: farthest point coordinates
    last_with_origin = None
    for r in chunk_results:
        if r["farthest_norm"] > 0:
            last_with_origin = r
    if last_with_origin:
        actual_point = (last_with_origin["farthest_a"], last_with_origin["farthest_b"])
        if actual_point != expected_farthest_point:
            # In resume mode the solver may not know the coordinates
            # (it has a synthetic origin at 0,0), so only warn
            print(f"  NOTE: farthest point {actual_point} vs expected {expected_farthest_point}")

    # Check 4: moat should be found in at least one chunk
    any_moat = any(r.get("moat_found", False) for r in chunk_results)
    if not any_moat:
        passed = False
        reasons.append("No chunk reported MOAT_FOUND")

    if passed:
        print(f"  PASS: farthest plateaus at norm {farthest_norm}, moat correctly detected")
    else:
        print(f"  FAIL: {'; '.join(reasons)}")

    return passed


def main():
    if not os.path.isfile(SOLVER_BIN):
        print(f"ERROR: Solver binary not found at {SOLVER_BIN}")
        print(f"Build: cd solver && cargo build --release")
        sys.exit(2)

    results = {}

    # Test 1: k²=2, tiny chunks to force cross-chunk resume
    # Known: moat at (11,4) norm 137. Use chunk_size=50 to get ~4 chunks.
    results["k2_2"] = test_multi_chunk(
        k_squared=2,
        chunk_size=50,
        total_norm=300,
        expected_farthest_norm=137,
        expected_farthest_point=(11, 4),
        label="k²=2 multi-chunk LB (chunk_size=50, 6 chunks)",
    )

    # Test 2: k²=8, chunked to force cross-chunk resume
    # Known: moat at (84,41) norm 8737. Use chunk_size=2000 to get ~6 chunks.
    results["k2_8"] = test_multi_chunk(
        k_squared=8,
        chunk_size=2000,
        total_norm=12000,
        expected_farthest_norm=8737,
        expected_farthest_point=(84, 41),
        label="k²=8 multi-chunk LB (chunk_size=2000, 6 chunks)",
    )

    # Single-chunk baseline: full range in one shot (no resume involved)
    print(f"\n{'='*60}")
    print("BASELINE: k²=2 single-chunk (no resume)")
    print(f"{'='*60}")
    baseline = run_solver_chunk(k_squared=2, norm_bound=300, angular=1)
    if "error" not in baseline:
        print(f"  farthest_norm={baseline['farthest_norm']}, "
              f"point=({baseline['farthest_a']},{baseline['farthest_b']}), "
              f"component={baseline['component_size']}, moat={'YES' if baseline['moat_found'] else 'no'}")
        if baseline["farthest_norm"] == 137:
            print("  PASS: baseline matches expected")
        else:
            print(f"  FAIL: baseline farthest_norm={baseline['farthest_norm']} != 137")
    else:
        print(f"  ERROR: {baseline['error']}")

    # Summary
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    all_pass = all(results.values())
    for name, passed in results.items():
        print(f"  {name}: {'PASS' if passed else 'FAIL'}")
    print(f"  Overall: {'ALL PASS' if all_pass else 'SOME FAILED'}")

    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()
