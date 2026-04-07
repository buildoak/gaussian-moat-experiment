#!/usr/bin/env python3
"""Validate the hierarchical tiling moat finder against known results."""

import math
import os
import sys
import time

# Ensure parent dir is on path so 'prototype' is importable as a package
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from prototype.moat_finder import find_moat, incremental_search
from prototype.visualize import plot_primes_with_tiles, plot_frontier, plot_composition_tree


def run_test(test_name, k_sq, radii, tile_size, expect_moat):
    """Run a single moat test and report results.

    Args:
        test_name: descriptive name.
        k_sq: squared step threshold.
        radii: list of radii to try.
        tile_size: tile dimension.
        expect_moat: True if we expect a moat, False if we expect no moat,
                     None if unknown.
    """
    step = math.sqrt(k_sq)
    print(f"\n{'#'*70}")
    print(f"# {test_name}")
    print(f"# k²={k_sq}, step={step:.3f}, radii={radii}, tile={tile_size}")
    print(f"# Expected: {'moat' if expect_moat else 'no moat' if expect_moat is False else 'unknown'}")
    print(f"{'#'*70}")

    results = incremental_search(k_sq, radii, tile_size, verbose=True)

    # Final result
    final = results[-1]

    print(f"\n{'='*60}")
    print(f"FINAL RESULT for {test_name}:")
    print(final.summary())

    if expect_moat is not None:
        if final.moat_found == expect_moat:
            print(f"  PASS — matches expectation")
        else:
            print(f"  NOTE — does not match expectation (expected moat={expect_moat}, got moat={final.moat_found})")
    print(f"{'='*60}")

    # Generate plots for the final (or most interesting) result
    # Use a smaller radius for plotting if R is large
    plot_R = min(final.radius, 100)

    tag = f"k{k_sq}"
    p1 = plot_primes_with_tiles(final, f"{tag}_primes_tiles.png", max_plot_radius=plot_R)
    print(f"  Plot saved: {p1}")

    p2 = plot_frontier(final, f"{tag}_frontier.png", max_plot_radius=plot_R)
    print(f"  Plot saved: {p2}")

    # Composition tree (schematic)
    n_rows = (2 * final.radius) // final.tile_size
    n_cols = n_rows
    n_levels = max(1, math.ceil(math.log2(max(n_rows, n_cols))))
    p3 = plot_composition_tree(n_rows, n_cols, n_levels, f"{tag}_tree.png")
    print(f"  Plot saved: {p3}")

    return final


def main():
    t_start = time.time()
    all_passed = True

    # ─────────────────────────────────────────────────
    # Test 1: k²=2 (step √2 ≈ 1.414)
    # Known: moat exists at small radius
    # ─────────────────────────────────────────────────
    r1 = run_test(
        "Test 1: k²=2 (step √2)",
        k_sq=2,
        radii=[10, 30],
        tile_size=16,
        expect_moat=True,
    )

    # ─────────────────────────────────────────────────
    # Test 2: k²=4 (step 2.0)
    # Known: moat exists at moderate radius
    # ─────────────────────────────────────────────────
    r2 = run_test(
        "Test 2: k²=4 (step 2.0)",
        k_sq=4,
        radii=[10, 50],
        tile_size=16,
        expect_moat=True,
    )

    # ─────────────────────────────────────────────────
    # Test 3: k²=6 (step √6 ≈ 2.449)
    # Explore: does moat exist?
    # ─────────────────────────────────────────────────
    r3 = run_test(
        "Test 3: k²=6 (step √6)",
        k_sq=6,
        radii=[50, 100],
        tile_size=32,
        expect_moat=None,  # exploratory
    )

    # ─────────────────────────────────────────────────
    # Test 4: k²=40 (step √40 ≈ 6.324) — our target
    # Expected: no moat (component reaches boundary)
    # ─────────────────────────────────────────────────
    r4 = run_test(
        "Test 4: k²=40 (step √40) — target",
        k_sq=40,
        radii=[100, 500],
        tile_size=64,
        expect_moat=False,
    )

    total_time = time.time() - t_start

    # ─────────────────────────────────────────────────
    # Summary
    # ─────────────────────────────────────────────────
    print(f"\n\n{'='*70}")
    print("SUMMARY")
    print(f"{'='*70}")
    for label, r in [("k²=2", r1), ("k²=4", r2), ("k²=6", r3), ("k²=40", r4)]:
        status = "MOAT" if r.moat_found else f"NO MOAT (reaches {r.faces_reached})"
        print(f"  {label:8s}: {status:40s} [{r.runtime_s:.2f}s]")
    print(f"\n  Total time: {total_time:.1f}s")
    print(f"{'='*70}")


if __name__ == "__main__":
    main()
