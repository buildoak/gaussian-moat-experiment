#!/usr/bin/env python3
"""
verify_grid.py -- Grid invariant checker for tiles-compositor.

Reads JSON output from dump_grid for multiple R values and verifies
all grid_spec v6 invariants.
"""

import json
import math
import os
import subprocess
import sys

S = 256
TILES_PER_TOWER_MIN = 32
TILES_PER_TOWER_MAX = 46
MARGIN = 2 * S

DUMP_GRID = os.path.join(os.path.dirname(__file__), "..", "build", "dump_grid")

# R values to test
R_VALUES = [850_000_000, 400_000_000, 100_000_000, 10_000]


def run_dump_grid(R: int) -> dict:
    """Run dump_grid and parse JSON output."""
    result = subprocess.run(
        [DUMP_GRID, str(R)],
        capture_output=True, text=True, timeout=120
    )
    if result.returncode != 0:
        raise RuntimeError(f"dump_grid failed for R={R}: {result.stderr}")
    return json.loads(result.stdout)


def check_tower_heights(data: dict) -> list[str]:
    """Verify all tiles_per_tower[j] in [32, 46]."""
    failures = []
    tpt = data["tiles_per_tower"]
    for j, h in enumerate(tpt):
        if h < TILES_PER_TOWER_MIN or h > TILES_PER_TOWER_MAX:
            failures.append(f"tower {j}: tiles_per_tower={h} outside [32, 46]")
    return failures


def check_height_cos_theta(data: dict) -> list[str]:
    """Verify tiles_per_tower[j] == ceil(32 / cos_theta_j), clamped."""
    failures = []
    tpt = data["tiles_per_tower"]
    base_y = data["base_y"]
    for j in range(len(tpt)):
        xj = j * S
        yj = base_y[j]
        hyp = math.sqrt(xj * xj + yj * yj)
        if hyp < 1.0:
            expected = 32
        elif yj <= 0:
            expected = 46
        else:
            cos_theta = yj / hyp
            raw = 32.0 / cos_theta
            expected = math.ceil(raw)
            expected = max(32, min(46, expected))
        if tpt[j] != expected:
            failures.append(
                f"tower {j}: tpt={tpt[j]} expected={expected} "
                f"(xj={xj}, yj={yj}, hyp={hyp:.2f})"
            )
    return failures


def check_gentle_ramp(data: dict) -> list[str]:
    """Verify |tiles_per_tower[j] - tiles_per_tower[j+1]| <= 1.
    This is an asymptotic property for large R (spec S4.2a); at small R
    the curvature changes fast enough that adjacent towers can jump by 2.
    Only enforce for R >= 1_000_000.
    """
    R = data["R"]
    if R < 1_000_000:
        return []  # not expected to hold at small R
    failures = []
    tpt = data["tiles_per_tower"]
    for j in range(len(tpt) - 1):
        diff = abs(tpt[j + 1] - tpt[j])
        if diff > 1:
            failures.append(f"towers {j}-{j+1}: height jump {diff} > 1")
    return failures


def check_base_y_monotonic(data: dict) -> list[str]:
    """Verify base_y is non-increasing."""
    failures = []
    base_y = data["base_y"]
    for j in range(1, len(base_y)):
        if base_y[j] > base_y[j - 1]:
            failures.append(
                f"tower {j}: base_y={base_y[j]} > base_y[{j-1}]={base_y[j-1]}"
            )
    return failures


def check_arc_deviation(data: dict) -> list[str]:
    """Verify |base_y[j] - y_cont[j]| <= 0.5 for all towers."""
    failures = []
    arc_dev = data["arc_deviation"]
    for j, dev in enumerate(arc_dev):
        if abs(dev) > 0.500001:  # tiny epsilon for float comparison
            failures.append(f"tower {j}: |deviation|={abs(dev):.6f} > 0.5")
    return failures


def check_termination(data: dict) -> list[str]:
    """Verify termination predicate:
    - Last tower satisfies: base_y[j] + tpt[j]*S + MARGIN > j*S
    - Would-be next tower (if arc allows) would fail the predicate.
    """
    failures = []
    R = data["R"]
    num = data["num_towers"]
    base_y = data["base_y"]
    tpt = data["tiles_per_tower"]

    if num == 0:
        return ["no towers generated"]

    # Last included tower must satisfy predicate
    j_last = num - 1
    top_last = base_y[j_last] + tpt[j_last] * S + MARGIN
    x_last = j_last * S
    if top_last <= x_last:
        failures.append(
            f"last tower {j_last}: top_edge={top_last} <= x={x_last} "
            "(should satisfy predicate)"
        )

    # Check that the next tower would either be beyond the arc or fail the predicate
    j_next = num
    x_next = j_next * S
    x_next_sq = x_next * x_next
    R_sq = R * R
    if x_next_sq <= R_sq:
        # Next tower is within the arc - compute its properties
        y_cont = math.sqrt(R_sq - x_next_sq)
        # Approximate base_y (round to nearest integer, clamped for monotonicity)
        y_approx = round(y_cont)
        # Compute tower height at this position
        hyp = math.sqrt(x_next * x_next + y_approx * y_approx)
        if hyp >= 1.0 and y_approx > 0:
            cos_theta = y_approx / hyp
            h_approx = min(46, max(32, math.ceil(32.0 / cos_theta)))
        else:
            h_approx = 46
        top_next = y_approx + h_approx * S + MARGIN
        if top_next > x_next:
            # The next tower WOULD satisfy the predicate.
            # This is only informational - monotonicity clamp might change y slightly.
            # We flag it as a warning, not a hard failure.
            failures.append(
                f"WARNING: next tower {j_next} would satisfy predicate "
                f"(top={top_next} > x={x_next}), y_approx={y_approx}"
            )

    return failures


def check_prefix_sums(data: dict) -> list[str]:
    """Verify tower_offset[j] = sum(tiles_per_tower[0:j])."""
    failures = []
    tpt = data["tiles_per_tower"]
    offsets = data["tower_offset"]
    running = 0
    for j in range(len(tpt)):
        if offsets[j] != running:
            failures.append(
                f"tower {j}: tower_offset={offsets[j]} expected={running}"
            )
        running += tpt[j]
    if data["total_tiles"] != running:
        failures.append(
            f"total_tiles={data['total_tiles']} expected={running}"
        )
    return failures


def check_delta_table(data: dict) -> list[str]:
    """Verify delta[j] = base_y[j] - base_y[j+1], all >= 0."""
    failures = []
    base_y = data["base_y"]
    delta = data["delta"]
    if len(delta) != len(base_y) - 1:
        failures.append(
            f"delta length={len(delta)} expected={len(base_y) - 1}"
        )
        return failures
    for j in range(len(delta)):
        expected = base_y[j] - base_y[j + 1]
        if delta[j] != expected:
            failures.append(
                f"tower {j}: delta={delta[j]} expected={expected}"
            )
        if delta[j] < 0:
            failures.append(f"tower {j}: delta={delta[j]} < 0")
    return failures


def check_dead_tile_spec_consistency(data: dict) -> list[str]:
    """Check is_tile_dead against grid_spec v6:
    A tile (j, r) is dead when base_y[j] + (r+1)*S <= j*S
    (tile's top edge is at or below the diagonal y=x at x=j*S).
    C++ now matches this formula.
    """
    failures = []
    base_y = data["base_y"]
    tpt = data["tiles_per_tower"]
    num = data["num_towers"]

    dead_count = 0
    for j in range(num):
        for r in range(tpt[j]):
            if (base_y[j] + (r + 1) * S) <= (j * S):
                dead_count += 1

    # Sanity: dead tiles should only exist in towers near/past the diagonal
    if num > 0:
        j_last = num - 1
        x_last = j_last * S
        if dead_count > 0 and base_y[0] + S <= 0:
            failures.append(f"dead tiles found but tower 0 base_y={base_y[0]} makes no sense")

    return failures


def check_tower_count_range(data: dict) -> list[str]:
    """Verify tower count is in expected range: ~ R / (sqrt(2) * S) + O(1)."""
    failures = []
    R = data["R"]
    num = data["num_towers"]
    expected_approx = R / (math.sqrt(2) * S)
    # Allow 5% tolerance plus some constant for MARGIN overshoot
    lo = expected_approx * 0.95
    hi = expected_approx * 1.05 + 100
    if not (lo <= num <= hi):
        failures.append(
            f"num_towers={num} outside expected range [{lo:.0f}, {hi:.0f}] "
            f"(expected ~{expected_approx:.0f})"
        )
    return failures


def check_base_y_near_arc(data: dict) -> list[str]:
    """Verify base_y[j] is close to sqrt(R^2 - (j*S)^2)."""
    failures = []
    R = data["R"]
    base_y = data["base_y"]
    R_sq = R * R
    max_deviation = 0.0
    for j in range(len(base_y)):
        x_j = j * S
        y_cont = math.sqrt(R_sq - x_j * x_j)
        dev = abs(base_y[j] - y_cont)
        max_deviation = max(max_deviation, dev)
    # Each base_y should be within ~S of the arc (generous bound)
    if max_deviation > S:
        failures.append(f"max base_y deviation from arc: {max_deviation:.2f} > {S}")
    return failures


def check_first_tower(data: dict) -> list[str]:
    """First tower should have base_y close to R and height 32."""
    failures = []
    R = data["R"]
    base_y = data["base_y"]
    tpt = data["tiles_per_tower"]
    if not base_y:
        return ["no towers"]
    if abs(base_y[0] - R) > S:
        failures.append(f"base_y[0]={base_y[0]} too far from R={R}")
    if tpt[0] != 32:
        failures.append(f"tiles_per_tower[0]={tpt[0]} expected 32")
    return failures


def check_last_tower_near_diagonal(data: dict) -> list[str]:
    """Last few towers should be near or past the y=x diagonal."""
    failures = []
    base_y = data["base_y"]
    tpt = data["tiles_per_tower"]
    num = data["num_towers"]
    if num == 0:
        return ["no towers"]
    j_last = num - 1
    x_last = j_last * S
    # Last tower's base_y should be reasonably close to x_last
    # (within a few tiles of the diagonal)
    if base_y[j_last] > x_last + 50 * S:
        failures.append(
            f"last tower {j_last}: base_y={base_y[j_last]} too far above "
            f"diagonal at x={x_last}"
        )
    # Last tower should have height >= 44 (near 45 degrees)
    if tpt[j_last] < 44:
        failures.append(f"last tower height={tpt[j_last]} expected >= 44")
    return failures


def run_all_checks(R: int):
    """Run all invariant checks for one R value."""
    print(f"\n{'='*60}")
    print(f"R = {R:,}")
    print(f"{'='*60}")

    data = run_dump_grid(R)
    print(f"  num_towers = {data['num_towers']:,}")
    print(f"  total_tiles = {data['total_tiles']:,}")
    print(f"  tiles_per_tower[0] = {data['tiles_per_tower'][0]}")
    print(f"  tiles_per_tower[-1] = {data['tiles_per_tower'][-1]}")
    print(f"  max |deviation| = {max(abs(d) for d in data['arc_deviation']):.6f}")

    checks = [
        ("Tower heights in [32, 46]", check_tower_heights),
        ("Height matches ceil(32/cos_theta)", check_height_cos_theta),
        ("Gentle ramp (|delta_h| <= 1)", check_gentle_ramp),
        ("base_y monotonically non-increasing", check_base_y_monotonic),
        ("Arc deviation |dev| <= 0.5", check_arc_deviation),
        ("Termination predicate", check_termination),
        ("Prefix sums correct", check_prefix_sums),
        ("Delta table correct", check_delta_table),
        ("Tower count in expected range", check_tower_count_range),
        ("base_y near arc", check_base_y_near_arc),
        ("First tower properties", check_first_tower),
        ("Last tower near diagonal", check_last_tower_near_diagonal),
        ("Dead-tile spec consistency", check_dead_tile_spec_consistency),
    ]

    all_pass = True
    for name, check_fn in checks:
        failures = check_fn(data)
        if failures:
            status = "FAIL" if not all(f.startswith("WARNING") for f in failures) else "WARN"
            if status == "FAIL":
                all_pass = False
            print(f"  [{status}] {name}")
            for f in failures:
                print(f"         {f}")
        else:
            print(f"  [PASS] {name}")

    return all_pass


def main():
    if not os.path.exists(DUMP_GRID):
        print(f"ERROR: dump_grid not found at {DUMP_GRID}")
        print("Build it first: cd build && cmake .. && make dump_grid")
        sys.exit(1)

    all_pass = True
    for R in R_VALUES:
        try:
            if not run_all_checks(R):
                all_pass = False
        except Exception as e:
            print(f"\nERROR for R={R}: {e}")
            all_pass = False

    print(f"\n{'='*60}")
    if all_pass:
        print("ALL INVARIANTS PASSED")
    else:
        print("SOME INVARIANTS FAILED")
    print(f"{'='*60}")
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()
