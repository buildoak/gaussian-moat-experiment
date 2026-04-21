#!/usr/bin/env python3
"""Strip-based radial connectivity probe for Gaussian moat experiments.

Sweeps outward from the origin in radial shells along a near-axis strip,
tracking how the origin's connected component propagates and where it dies.

Usage:
    python3 prototype/strip_probe.py
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

K_SQUARED_LIST = [2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36]


def get_r_max(k_sq: int) -> int:
    if k_sq <= 10:
        return 200
    if k_sq <= 20:
        return 500
    if k_sq <= 30:
        return 2000
    return 5000  # k² <= 36


TIME_BUDGET = 60.0
MAX_SIEVE_NORM = 50_000_000


# ────────────────────────────────────────────────────────────────
# Gaussian prime generation — sieve + trial division fallback
# ────────────────────────────────────────────────────────────────

def _is_prime_trial(n: int) -> bool:
    """Trial division for norms beyond sieve range."""
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
    if n <= sieve_limit:
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
# Core: shell-by-shell connectivity probe
# ────────────────────────────────────────────────────────────────

def probe_strip(k_sq: int, verbose: bool = True) -> dict:
    """Run the strip connectivity probe for a given k².

    Design:
      - frontier_primes: list of (a, b) carried from the previous shell's outer edge
      - frontier_is_origin: parallel bool list — True if that frontier prime belongs
        to the origin component
      - Each shell builds a local UnionFind over [frontier + new_shell_primes],
        connects edges, propagates origin labels, extracts new frontier.
    """
    step = math.sqrt(k_sq)
    shell_width = max(math.ceil(step), 1)
    strip_half_w = max(math.ceil(3 * step), 3)
    r_max = get_r_max(k_sq)

    if verbose:
        print(f"\n{'='*65}")
        print(f"k²={k_sq}  step={step:.3f}  shell_w={shell_width}  "
              f"strip_W={2*strip_half_w+1}  R_max={r_max}")
        print(f"{'='*65}")

    # Sieve
    max_norm = r_max * r_max + strip_half_w * strip_half_w
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
    moat_distance: Optional[float] = None
    shells_data: List[dict] = []
    t_start = time.time()
    step_ceil = math.ceil(step)

    for si in range(n_shells):
        if time.time() - t_start > TIME_BUDGET:
            if verbose:
                print(f"  WARNING: time budget exceeded at shell {si}. Stopping.")
            break

        a_lo = si * shell_width
        a_hi = (si + 1) * shell_width - 1

        # Generate primes in this shell
        new_primes = gen_primes_in_rect(
            a_lo, a_hi, -strip_half_w, strip_half_w, sieve, sieve_limit
        )

        n_fr = len(frontier_primes)
        n_new = len(new_primes)
        total = n_fr + n_new

        if total == 0:
            if origin_alive:
                origin_alive = False
                if not moat_found:
                    moat_found = True
                    moat_distance = float(a_lo) if a_lo > 0 else step
            shells_data.append({
                "r_inner": a_lo, "r_outer": a_hi + 1,
                "primes": 0, "components_in": 0, "channels": 0,
                "origin_alive": False, "max_gap": float("inf"),
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
                moat_distance = 0.0
        else:
            # Propagate from frontier: merge all frontier primes that were origin
            origin_frontier_indices = [i for i in range(n_fr) if frontier_is_origin[i]]
            if origin_frontier_indices and origin_alive:
                # They may now belong to different UF components (after connecting
                # with new shell primes). Find which component(s) they're in.
                # They should all have been in one component in the previous shell,
                # but in this shell's UF they start disconnected and get merged
                # via the connection step above. We just need to union them
                # explicitly (in case some weren't connected through new primes).
                first = origin_frontier_indices[0]
                for i in origin_frontier_indices[1:]:
                    uf.union(first, i)
                origin_root = uf.find(first)
            else:
                origin_alive = False

        # ── Crossing analysis ──
        inner_comps: Set[int] = set()
        outer_comps: Set[int] = set()

        # Frontier primes touch the inner face by definition
        for idx in range(n_fr):
            inner_comps.add(uf.find(idx))

        # New shell primes: check inner/outer proximity
        for idx in range(n_fr, total):
            a, _b = coords[idx]
            if a <= a_lo + step_ceil:
                inner_comps.add(uf.find(idx))
            if a >= a_hi - step_ceil:
                outer_comps.add(uf.find(idx))

        channels = inner_comps & outer_comps
        n_channels = len(channels)

        # Origin survival: does origin component cross to outer face?
        if origin_alive and origin_root is not None:
            origin_root = uf.find(origin_root)  # re-find after all unions
            if origin_root not in channels:
                if not moat_found:
                    moat_found = True
                    moat_distance = float(a_hi + 1)
                origin_alive = False

        # ── Max angular gap among new shell primes ──
        if n_new > 0:
            angles = sorted(math.atan2(b, a) for a, b in new_primes)
            if len(angles) > 1:
                gaps = [angles[i + 1] - angles[i] for i in range(len(angles) - 1)]
                gaps.append(2 * math.pi - (angles[-1] - angles[0]))
                max_gap = max(gaps)
            else:
                max_gap = 2 * math.pi
        else:
            max_gap = float("inf")

        shells_data.append({
            "r_inner": a_lo,
            "r_outer": a_hi + 1,
            "primes": n_new,
            "components_in": len(inner_comps),
            "channels": n_channels,
            "origin_alive": origin_alive,
            "max_gap": round(max_gap, 4),
        })

        if verbose and si % max(1, n_shells // 20) == 0:
            print(f"  k²={k_sq}: shell {si} R={a_lo}-{a_hi+1}, "
                  f"primes={n_new}, ch={n_channels}, origin={origin_alive}")

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
        "strip_width": 2 * strip_half_w + 1,
        "r_max": r_max,
        "moat_found": moat_found,
        "moat_distance": moat_distance,
        "runtime_s": round(total_time, 2),
        "shells": shells_data,
    }

    if verbose:
        status = f"MOAT at R={moat_distance}" if moat_found else "NO MOAT within R_max"
        print(f"  >> {status}  [{total_time:.2f}s]")

    return result


# ────────────────────────────────────────────────────────────────
# Main
# ────────────────────────────────────────────────────────────────

def main():
    print("Gaussian Moat — Strip Connectivity Probe")
    print(f"k² values: {K_SQUARED_LIST}")
    print()

    experiments = []
    t_total = time.time()

    for k_sq in K_SQUARED_LIST:
        result = probe_strip(k_sq, verbose=True)
        experiments.append(result)

    total_time = time.time() - t_total

    # Write output
    output = {"experiments": experiments}
    out_path = os.path.join(_here, "connectivity_data.json")
    with open(out_path, "w") as f:
        json.dump(output, f, indent=2, default=str)

    print(f"\n{'='*65}")
    print(f"DONE — {len(experiments)} experiments in {total_time:.1f}s")
    print(f"Data: {out_path}")
    print(f"{'='*65}")

    # Summary table
    print(f"\n  {'k²':>4}  {'step':>6}  {'moat?':>5}  {'moat_R':>8}  {'time':>6}")
    print(f"  {'-'*38}")
    for e in experiments:
        md = e["moat_distance"]
        md_str = f"{md:.1f}" if md is not None else "---"
        print(f"  {e['k_squared']:>4}  {e['step']:>6.3f}  "
              f"{'YES' if e['moat_found'] else 'NO':>5}  {md_str:>8}  "
              f"{e['runtime_s']:>5.1f}s")


if __name__ == "__main__":
    main()
