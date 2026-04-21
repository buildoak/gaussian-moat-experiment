#!/usr/bin/env python3
"""Multi-strip radial connectivity probe for Gaussian moat experiments.

Instead of a single narrow strip, uses N adjacent strips forming a wide band.
Cross-strip connections are naturally captured, so prime chains that escape
sideways are retained. This dramatically reduces false positives compared
to the single-strip probe.

Validates against known Tsuchimura moat distances.

Usage:
    python3 prototype/multi_strip_probe.py
"""

from __future__ import annotations

import json
import math
import os
import sys
import time
from typing import Dict, List, Optional, Set, Tuple

import numpy as np

# Handle both standalone and package execution
_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_here))

from prototype.primes import sieve_of_eratosthenes
from prototype.unionfind import UnionFind

# ────────────────────────────────────────────────────────────────
# Configuration
# ────────────────────────────────────────────────────────────────

# Known moat distances (Tsuchimura 2004, exact lower bounds)
TSUCHIMURA = {
    2: 11.7,
    4: 45.3,
    8: 93.5,
    10: 1024,
    16: 4313,
    18: 10749,
    20: 133679,
    26: 1015639,
    32: 2823055,
    36: 80015782,  # upper bound
}

TIME_BUDGET_PER_RUN = 120.0
MAX_SIEVE_NORM = 200_000_000  # 200M — can handle R=12000

# ────────────────────────────────────────────────────────────────
# Prime generation (sieve + trial division fallback)
# ────────────────────────────────────────────────────────────────

def _is_prime_trial(n: int) -> bool:
    if n < 2:
        return False
    if n < 4:
        return True
    if n % 2 == 0 or n % 3 == 0:
        return False
    i = 5
    while i * i <= n:
        if n % i == 0 or n % (i + 2) == 0:
            return False
        i += 6
    return True


def _check_prime(n: int, sieve: Optional[np.ndarray], sieve_limit: int) -> bool:
    if n <= sieve_limit and sieve is not None:
        return bool(sieve[n])
    return _is_prime_trial(n)


def gen_primes_in_rect(
    a_lo: int, a_hi: int, b_lo: int, b_hi: int,
    sieve: Optional[np.ndarray], sieve_limit: int,
) -> List[Tuple[int, int]]:
    """Generate Gaussian primes in [a_lo..a_hi] x [b_lo..b_hi]."""
    primes: List[Tuple[int, int]] = []
    for a in range(a_lo, a_hi + 1):
        for b in range(b_lo, b_hi + 1):
            if a == 0 and b == 0:
                continue
            if a == 0:
                bb = abs(b)
                if bb >= 2 and bb % 4 == 3 and _check_prime(bb, sieve, sieve_limit):
                    primes.append((0, b))
            elif b == 0:
                aa = abs(a)
                if aa >= 2 and aa % 4 == 3 and _check_prime(aa, sieve, sieve_limit):
                    primes.append((a, 0))
            else:
                norm = a * a + b * b
                if _check_prime(norm, sieve, sieve_limit):
                    primes.append((a, b))
    return primes


# ────────────────────────────────────────────────────────────────
# Core: multi-strip radial connectivity probe
# ────────────────────────────────────────────────────────────────

def probe_multi_strip(
    k_sq: int,
    n_strips: int = 10,
    strip_width: Optional[int] = None,
    r_max: int = 1500,
    verbose: bool = True,
) -> dict:
    """Run multi-strip connectivity probe.

    The band is a rectangle: a in [0, r_max], b in [-band_width/2, band_width/2].
    Shell-by-shell processing with frontier propagation.
    Tracks per-strip presence of origin component as diagnostic.

    Args:
        k_sq: squared step threshold.
        n_strips: number of adjacent strips.
        strip_width: width of each strip. Default: ceil(2*sqrt(k_sq)).
        r_max: maximum radial distance.
        verbose: print progress.

    Returns:
        dict with experiment results.
    """
    step = math.sqrt(k_sq)
    step_ceil = max(math.ceil(step), 1)
    shell_width = step_ceil

    if strip_width is None:
        strip_width = max(math.ceil(2 * step), 3)

    band_width = n_strips * strip_width
    band_half = band_width // 2

    if verbose:
        print(f"\n{'='*70}")
        print(f"k²={k_sq}  step={step:.3f}  n_strips={n_strips}  "
              f"strip_w={strip_width}  band_w={band_width}  R_max={r_max}")
        print(f"{'='*70}")

    # Sieve — need primes up to max norm in the band
    max_norm = r_max * r_max + band_half * band_half
    sieve_limit = min(max_norm, MAX_SIEVE_NORM)
    if verbose:
        print(f"  Sieve up to {sieve_limit:,} ...", end=" ", flush=True)
    t0 = time.time()
    sieve = sieve_of_eratosthenes(sieve_limit)
    if verbose:
        print(f"done ({time.time()-t0:.2f}s)")

    n_shells = math.ceil(r_max / shell_width)

    # State carried between shells
    frontier_primes: List[Tuple[int, int]] = []
    frontier_is_origin: List[bool] = []

    origin_alive = True
    moat_found = False
    moat_r: Optional[float] = None
    shells_data: List[dict] = []
    t_start = time.time()

    # Strip boundaries for diagnostic tracking
    # Strip i covers b in [strip_lo_i, strip_hi_i)
    strip_bounds = []
    for i in range(n_strips):
        lo = -band_half + i * strip_width
        hi = lo + strip_width
        strip_bounds.append((lo, hi))

    def _get_strip(b: int) -> int:
        """Return which strip index a b-coordinate belongs to, or -1 if outside."""
        idx = (b + band_half) // strip_width
        if 0 <= idx < n_strips:
            return idx
        return -1

    for si in range(n_shells):
        elapsed = time.time() - t_start
        if elapsed > TIME_BUDGET_PER_RUN:
            if verbose:
                print(f"  WARNING: time budget ({TIME_BUDGET_PER_RUN}s) exceeded "
                      f"at shell {si}/{n_shells}. Stopping.")
            break

        a_lo = si * shell_width
        a_hi = (si + 1) * shell_width - 1

        # Generate primes in this shell across the full band width
        new_primes = gen_primes_in_rect(
            a_lo, a_hi, -band_half, band_half, sieve, sieve_limit
        )

        n_fr = len(frontier_primes)
        n_new = len(new_primes)
        total = n_fr + n_new

        if total == 0:
            if origin_alive:
                origin_alive = False
                if not moat_found:
                    moat_found = True
                    moat_r = float(a_lo) if a_lo > 0 else step
            shells_data.append({
                "r": a_lo,
                "active_strips": 0,
                "total_primes": 0,
                "channels": 0,
                "origin_alive": False,
            })
            frontier_primes = []
            frontier_is_origin = []
            continue

        # Build UF: indices [0..n_fr) = frontier, [n_fr..total) = new primes
        uf = UnionFind(total)
        coords: List[Tuple[int, int]] = list(frontier_primes) + new_primes

        # Spatial hash for neighbor search
        cell_side = max(step_ceil, 1)
        cells: Dict[Tuple[int, int], List[int]] = {}
        for idx, (a, b) in enumerate(coords):
            cx, cy = a // cell_side, b // cell_side
            cells.setdefault((cx, cy), []).append(idx)

        # Connect all pairs within step distance
        for idx in range(total):
            a, b = coords[idx]
            cx, cy = a // cell_side, b // cell_side
            for dcx in range(-2, 3):
                for dcy in range(-2, 3):
                    nbr_cell = (cx + dcx, cy + dcy)
                    if nbr_cell not in cells:
                        continue
                    for jdx in cells[nbr_cell]:
                        if jdx <= idx:
                            continue
                        da = coords[jdx][0] - a
                        db = coords[jdx][1] - b
                        if da * da + db * db <= k_sq:
                            uf.union(idx, jdx)

        # ── Origin propagation ──
        origin_root: Optional[int] = None

        if si == 0:
            # First shell: origin connects to primes within step of (0,0)
            origin_nbrs = [idx for idx, (a, b) in enumerate(coords)
                           if a * a + b * b <= k_sq]
            if origin_nbrs:
                for i in range(1, len(origin_nbrs)):
                    uf.union(origin_nbrs[0], origin_nbrs[i])
                origin_root = uf.find(origin_nbrs[0])
                origin_alive = True
            else:
                origin_alive = False
                moat_found = True
                moat_r = 0.0
        else:
            # Propagate from frontier
            origin_frontier_indices = [i for i in range(n_fr) if frontier_is_origin[i]]
            if origin_frontier_indices and origin_alive:
                first = origin_frontier_indices[0]
                for i in origin_frontier_indices[1:]:
                    uf.union(first, i)
                origin_root = uf.find(first)
            else:
                origin_alive = False

        # ── Crossing analysis ──
        outer_comps: Set[int] = set()
        for idx in range(total):
            a, _b = coords[idx]
            if a >= a_hi - step_ceil:
                outer_comps.add(uf.find(idx))

        channels = 0
        if origin_alive and origin_root is not None:
            origin_root = uf.find(origin_root)
            if origin_root not in outer_comps:
                if not moat_found:
                    moat_found = True
                    moat_r = float(a_hi + 1)
                origin_alive = False
            else:
                # Count distinct crossing components
                inner_comps: Set[int] = set()
                for idx in range(n_fr):
                    inner_comps.add(uf.find(idx))
                for idx in range(n_fr, total):
                    a, _b = coords[idx]
                    if a <= a_lo + step_ceil:
                        inner_comps.add(uf.find(idx))
                channels = len(inner_comps & outer_comps)

        # ── Per-strip diagnostic: which strips have origin component primes? ──
        active_strips = 0
        if origin_alive and origin_root is not None:
            strip_has_origin = [False] * n_strips
            origin_root_final = uf.find(origin_root)
            for idx in range(n_fr, total):  # only new primes (current shell)
                if uf.find(idx) == origin_root_final:
                    _, b = coords[idx]
                    s = _get_strip(b)
                    if 0 <= s < n_strips:
                        strip_has_origin[s] = True
            active_strips = sum(strip_has_origin)

        shells_data.append({
            "r": a_lo,
            "active_strips": active_strips,
            "total_primes": n_new,
            "channels": channels,
            "origin_alive": origin_alive,
        })

        if verbose and (si % max(1, n_shells // 20) == 0 or not origin_alive):
            print(f"  k²={k_sq}, n_strips={n_strips}: shell {si} R={a_lo}-{a_hi+1}, "
                  f"primes={n_new}, active={active_strips}, origin={origin_alive}")
            if not origin_alive and moat_found:
                # Print once and stop verbose spam
                pass

        # Early exit: origin is dead and we've recorded it
        if not origin_alive and moat_found:
            # Continue a few more shells to confirm no revival (shouldn't happen)
            # Actually in this design, once dead = dead. We can break.
            if verbose:
                print(f"  >> Origin component dead at R={moat_r}. Stopping.")
            break

        # ── Build next frontier ──
        next_frontier: List[Tuple[int, int]] = []
        next_is_origin: List[bool] = []

        for idx in range(total):
            a, b = coords[idx]
            if a >= a_hi - step_ceil:
                next_frontier.append((a, b))
                if origin_alive and origin_root is not None:
                    next_is_origin.append(uf.find(idx) == uf.find(origin_root))
                else:
                    next_is_origin.append(False)

        frontier_primes = next_frontier
        frontier_is_origin = next_is_origin

    total_time = time.time() - t_start

    result = {
        "k_squared": k_sq,
        "step": round(step, 4),
        "n_strips": n_strips,
        "strip_width": strip_width,
        "total_band_width": band_width,
        "r_max": r_max,
        "moat_found": moat_found,
        "moat_r": moat_r,
        "runtime_s": round(total_time, 2),
        "shells": shells_data,
    }

    if verbose:
        status = f"MOAT at R={moat_r}" if moat_found else "NO MOAT within R_max"
        known = TSUCHIMURA.get(k_sq)
        ratio_str = ""
        if known and moat_r:
            ratio_str = f"  (ratio={moat_r/known:.4f})"
        print(f"  >> {status}{ratio_str}  [{total_time:.2f}s]")

    return result


# ────────────────────────────────────────────────────────────────
# Experiment runner
# ────────────────────────────────────────────────────────────────

def run_calibration_suite() -> dict:
    """Run calibration experiments against Tsuchimura moats."""

    all_results = {}

    # ── Experiment A: k²=10 (moat at 1024) ──
    print("\n" + "=" * 70)
    print("EXPERIMENT A: k²=10  (Tsuchimura moat at R=1,024)")
    print("=" * 70)

    k10_results = []
    strip_w_10 = max(math.ceil(2 * math.sqrt(10)), 3)  # = 7
    for n_s in [1, 5, 10, 20, 50, 100, 200, 350]:
        # 350 strips * 7 width = 2450 band width, ~2.4x moat distance
        r = probe_multi_strip(k_sq=10, n_strips=n_s, strip_width=strip_w_10,
                              r_max=1500, verbose=True)
        k10_results.append(r)
    all_results["k10"] = k10_results

    # ── Experiment A2: k²=16 (moat at 4313) ──
    print("\n" + "=" * 70)
    print("EXPERIMENT A2: k²=16  (Tsuchimura moat at R=4,313)")
    print("=" * 70)

    k16_results = []
    strip_w_16 = max(math.ceil(2 * math.sqrt(16)), 3)  # = 8
    for n_s in [1, 5, 10, 20, 50, 100, 200, 500]:
        # 500 strips * 8 width = 4000, ~0.93x moat distance
        r_max_16 = 5000
        r = probe_multi_strip(k_sq=16, n_strips=n_s, strip_width=strip_w_16,
                              r_max=r_max_16, verbose=True)
        k16_results.append(r)
    all_results["k16"] = k16_results

    # ── Experiment A3: k²=18 (moat at 10749) ──
    print("\n" + "=" * 70)
    print("EXPERIMENT A3: k²=18  (Tsuchimura moat at R=10,749)")
    print("=" * 70)

    k18_results = []
    strip_w_18 = max(math.ceil(2 * math.sqrt(18)), 3)  # = 9
    for n_s in [1, 5, 10, 20, 50, 100, 200]:
        # 200 strips * 9 width = 1800, still only 0.17x moat distance
        # but tests the scaling trend
        r_max_18 = 12000 if n_s <= 50 else 8000
        r = probe_multi_strip(k_sq=18, n_strips=n_s, strip_width=strip_w_18,
                              r_max=r_max_18, verbose=True)
        k18_results.append(r)
    all_results["k18"] = k18_results

    return all_results


def print_summary_table(all_results: dict):
    """Print summary comparison table."""
    print("\n" + "=" * 70)
    print("SUMMARY: Multi-Strip Probe vs Tsuchimura Moats")
    print("=" * 70)

    for key in sorted(all_results.keys()):
        results = all_results[key]
        if not results:
            continue
        k_sq = results[0]["k_squared"]
        known = TSUCHIMURA.get(k_sq, None)
        known_str = f"{known:,.0f}" if known else "???"

        print(f"\nk²={k_sq}  (Tsuchimura moat: R={known_str})")
        print(f"  {'n_strips':>8}  {'band_w':>7}  {'moat_R':>10}  "
              f"{'ratio':>8}  {'time':>7}")
        print(f"  {'-'*50}")

        for r in results:
            mr = r["moat_r"]
            if mr is not None and known:
                ratio = mr / known
                ratio_str = f"{ratio:.4f}"
            else:
                ratio_str = "---"
            mr_str = f"{mr:,.0f}" if mr is not None else "alive"
            print(f"  {r['n_strips']:>8}  {r['total_band_width']:>7}  "
                  f"{mr_str:>10}  {ratio_str:>8}  {r['runtime_s']:>6.1f}s")


# ────────────────────────────────────────────────────────────────
# Plotting
# ────────────────────────────────────────────────────────────────

def generate_plots(all_results: dict, out_dir: str):
    """Generate calibration and diagnostic plots."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    os.makedirs(out_dir, exist_ok=True)

    # ── Plot 1: Disconnect R vs Number of Strips ──
    fig, ax = plt.subplots(1, 1, figsize=(14, 8))

    colors = {"k10": "#2196F3", "k16": "#FF9800", "k18": "#4CAF50"}
    labels = {"k10": "k²=10", "k16": "k²=16", "k18": "k²=18"}
    known_moats = {"k10": 1024, "k16": 4313, "k18": 10749}

    for key in ["k10", "k16", "k18"]:
        if key not in all_results:
            continue
        results = all_results[key]
        ns = [r["n_strips"] for r in results]
        moat_rs = [r["moat_r"] if r["moat_r"] is not None else r["r_max"] for r in results]
        alive = [r["moat_r"] is None for r in results]

        color = colors[key]
        ax.plot(ns, moat_rs, "o-", color=color, label=labels[key], linewidth=2,
                markersize=8, zorder=5)

        # Mark points where origin survived to r_max with open circles
        for i, is_alive in enumerate(alive):
            if is_alive:
                ax.plot(ns[i], moat_rs[i], "o", color=color, markersize=12,
                        markerfacecolor="white", markeredgewidth=2, zorder=6)

        # Horizontal line at known moat
        km = known_moats[key]
        ax.axhline(y=km, color=color, linestyle="--", alpha=0.5, linewidth=1.5)
        ax.text(max(ns) * 1.05, km, f"Tsuchimura: {km:,}",
                color=color, fontsize=9, va="center", alpha=0.7)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Number of Strips (N)", fontsize=13)
    ax.set_ylabel("Disconnect R (probe)", fontsize=13)
    ax.set_title("Multi-Strip Probe: Disconnect Distance vs Strip Count\n"
                 "(open circles = origin survived to r_max)", fontsize=14)
    ax.legend(fontsize=12, loc="lower right")
    ax.grid(True, alpha=0.3, which="both")

    fig.tight_layout()
    path1 = os.path.join(out_dir, "multistrip_calibration.png")
    fig.savefig(path1, dpi=150)
    plt.close(fig)
    print(f"  Plot saved: {path1}")

    # ── Plot 2: Active Strips vs R (for k²=10 with 50 strips) ──
    fig, ax = plt.subplots(1, 1, figsize=(14, 8))

    # Find k²=10 result with n_strips closest to 50
    target_results = {}
    if "k10" in all_results:
        for r in all_results["k10"]:
            if r["n_strips"] in [10, 20, 50, 100]:
                target_results[r["n_strips"]] = r

    strip_colors = {10: "#90CAF9", 20: "#42A5F5", 50: "#1E88E5", 100: "#0D47A1"}

    for ns_val, result in sorted(target_results.items()):
        shells = result["shells"]
        rs = [s["r"] for s in shells]
        active = [s["active_strips"] for s in shells]
        c = strip_colors.get(ns_val, "#999")
        ax.plot(rs, active, "-", color=c, label=f"N={ns_val} strips",
                linewidth=1.5, alpha=0.8)

    ax.axvline(x=1024, color="red", linestyle="--", linewidth=2, alpha=0.7,
               label="Tsuchimura moat (R=1024)")
    ax.set_xlabel("Radial Distance R", fontsize=13)
    ax.set_ylabel("Active Strips (origin component present)", fontsize=13)
    ax.set_title("k²=10: Lateral Spread of Origin Component Across Strips", fontsize=14)
    ax.legend(fontsize=11)
    ax.grid(True, alpha=0.3)

    fig.tight_layout()
    path2 = os.path.join(out_dir, "multistrip_active_strips.png")
    fig.savefig(path2, dpi=150)
    plt.close(fig)
    print(f"  Plot saved: {path2}")

    # ── Plot 3: Accuracy Ratio vs N strips ──
    fig, ax = plt.subplots(1, 1, figsize=(10, 6))

    for key in ["k10", "k16", "k18"]:
        if key not in all_results:
            continue
        results = all_results[key]
        k_sq = results[0]["k_squared"]
        known = TSUCHIMURA.get(k_sq)
        if not known:
            continue

        ns = []
        ratios = []
        for r in results:
            mr = r["moat_r"]
            if mr is not None:
                ns.append(r["n_strips"])
                ratios.append(mr / known)
            else:
                # Origin survived — ratio > 1 effectively (probe didn't find moat)
                # Mark as r_max / known to show it's at least that good
                ns.append(r["n_strips"])
                ratios.append(r["r_max"] / known)

        color = colors[key]
        ax.plot(ns, ratios, "o-", color=color, label=labels[key],
                linewidth=2, markersize=8)

    ax.axhline(y=1.0, color="red", linestyle="--", linewidth=2, alpha=0.5,
               label="Perfect (ratio=1.0)")
    ax.set_xscale("log")
    ax.set_xlabel("Number of Strips (N)", fontsize=13)
    ax.set_ylabel("Probe R / Tsuchimura R  (accuracy ratio)", fontsize=13)
    ax.set_title("Multi-Strip Probe Accuracy: Ratio to Known Moat Distance", fontsize=14)
    ax.legend(fontsize=11)
    ax.grid(True, alpha=0.3, which="both")
    ax.set_ylim(bottom=0)

    fig.tight_layout()
    path3 = os.path.join(out_dir, "multistrip_accuracy.png")
    fig.savefig(path3, dpi=150)
    plt.close(fig)
    print(f"  Plot saved: {path3}")


# ────────────────────────────────────────────────────────────────
# Main
# ────────────────────────────────────────────────────────────────

def main():
    print("Gaussian Moat — Multi-Strip Radial Connectivity Probe")
    print("Calibrating against Tsuchimura (2004) known moat distances")
    print()

    t0 = time.time()

    # Run calibration suite
    all_results = run_calibration_suite()

    # Print summary
    print_summary_table(all_results)

    # Save data
    data_path = os.path.join(_here, "multistrip_data.json")
    # Flatten for JSON
    flat_results = {}
    for key, results in all_results.items():
        flat_results[key] = results
    with open(data_path, "w") as f:
        json.dump(flat_results, f, indent=2, default=str)
    print(f"\nData saved: {data_path}")

    # Generate plots
    out_dir = os.path.join(_here, "output")
    print("\nGenerating plots...")
    generate_plots(all_results, out_dir)

    total = time.time() - t0
    print(f"\nTotal time: {total:.1f}s")


if __name__ == "__main__":
    main()
