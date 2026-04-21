#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["mpmath==1.3.0"]
# ///

"""Bad-zone reconciliation check for Gaussian-prime norms."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
from dataclasses import dataclass

from mpmath import mp

# Canonical per-deployment radii live in scripts/bz_config.json. Audit
# Codex-M2 (2026-04-21): BZ gate must bind to a single source so that
# CMakeLists.txt and this script cannot drift. Callers can still override
# via --r-inner/--r-outer for one-off audits.
_CONFIG_PATH = pathlib.Path(__file__).parent / "bz_config.json"


MR_BASES_U64 = (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37)
SMALL_PRIMES = (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37)


@dataclass(frozen=True)
class BadZone:
    name: str
    lower: mp.mpf
    upper: mp.mpf
    include_lower: bool
    include_upper: bool


def is_prime_u64(n: int) -> bool:
    if n < 2:
        return False
    for p in SMALL_PRIMES:
        if n == p:
            return True
        if n % p == 0:
            return False

    d = n - 1
    s = 0
    while d % 2 == 0:
        d //= 2
        s += 1

    for a in MR_BASES_U64:
        if a >= n:
            continue
        x = pow(a, d, n)
        if x == 1 or x == n - 1:
            continue
        for _ in range(s - 1):
            x = (x * x) % n
            if x == n - 1:
                break
        else:
            return False
    return True


def is_gaussian_prime_norm(n: int) -> bool:
    if n == 2:
        return True
    if is_prime_u64(n):
        return n % 4 == 1

    root = math.isqrt(n)
    if root * root != n:
        return False
    return root % 4 == 3 and is_prime_u64(root)


def in_zone(n: int, zone: BadZone) -> bool:
    x = mp.mpf(n)
    lower_ok = x >= zone.lower if zone.include_lower else x > zone.lower
    upper_ok = x <= zone.upper if zone.include_upper else x < zone.upper
    return bool(lower_ok and upper_ok)


def candidate_norms(zone: BadZone) -> list[int]:
    # The canonical intervals have width just over 1 at project scale. The
    # guard window prevents an endpoint rounding accident from dropping a
    # nearby integer before the high-precision predicate filters membership.
    start = int(mp.floor(zone.lower)) - 2
    stop = int(mp.ceil(zone.upper)) + 3
    return [n for n in range(start, stop) if in_zone(n, zone)]


def build_zones(r_inner: int, r_outer: int, k_sq: int) -> tuple[BadZone, BadZone]:
    r_i = mp.mpf(r_inner)
    r_o = mp.mpf(r_outer)
    k = mp.mpf(k_sq)
    sqrt_k = mp.sqrt(k)

    return (
        BadZone(
            name="BZ_I",
            lower=(mp.sqrt(r_i * r_i - 1) + sqrt_k) ** 2,
            upper=(r_i + sqrt_k) ** 2,
            include_lower=False,
            include_upper=True,
        ),
        BadZone(
            name="BZ_O",
            lower=(r_o - sqrt_k) ** 2,
            upper=(mp.sqrt(r_o * r_o + 1) - sqrt_k) ** 2,
            include_lower=True,
            include_upper=False,
        ),
    )


def load_config_radii(k_sq: int) -> tuple[int, int]:
    """Return (r_inner, r_outer) for the given K_SQ from bz_config.json."""
    if not _CONFIG_PATH.is_file():
        raise SystemExit(f"BZ config missing: {_CONFIG_PATH}")
    cfg = json.loads(_CONFIG_PATH.read_text())
    entry = cfg.get("k_sq_to_radii", {}).get(str(k_sq))
    if entry is None:
        raise SystemExit(
            f"BZ config {_CONFIG_PATH} has no k_sq={k_sq} entry. "
            "Add it before running the BZ gate for that deployment."
        )
    return int(entry["r_inner"]), int(entry["r_outer"])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Fail if a Gaussian-prime norm lies in the inner/outer bad-zone "
            "intervals where norm-form and lattice-witness geometry may diverge."
        )
    )
    parser.add_argument("--r-inner", type=int, default=None)
    parser.add_argument("--r-outer", type=int, default=None)
    parser.add_argument("--k-sq", type=int, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.k_sq <= 0:
        raise SystemExit("--k-sq must be positive")
    # Bind radii to the canonical config unless the caller overrides both.
    if args.r_inner is None or args.r_outer is None:
        cfg_inner, cfg_outer = load_config_radii(args.k_sq)
        if args.r_inner is None:
            args.r_inner = cfg_inner
        if args.r_outer is None:
            args.r_outer = cfg_outer
    if args.r_inner <= 0:
        raise SystemExit("--r-inner must be positive")
    if args.r_outer <= args.r_inner:
        raise SystemExit("--r-outer must be greater than --r-inner")

    mp.dps = 50

    bad_norms: list[tuple[str, int]] = []
    print(
        "BZ check: "
        f"R_inner={args.r_inner} R_outer={args.r_outer} K={args.k_sq} "
        f"mpmath_dps={mp.dps}"
    )

    for zone in build_zones(args.r_inner, args.r_outer, args.k_sq):
        norms = candidate_norms(zone)
        prime_norms = [n for n in norms if is_gaussian_prime_norm(n)]
        bad_norms.extend((zone.name, n) for n in prime_norms)
        print(
            f"{zone.name}: lower={mp.nstr(zone.lower, 60)} "
            f"upper={mp.nstr(zone.upper, 60)}"
        )
        print(f"{zone.name}: candidate_count={len(norms)} candidates={norms}")
        print(f"{zone.name}: gaussian_prime_norm_count={len(prime_norms)}")
        if prime_norms:
            print(f"{zone.name}: gaussian_prime_norms={prime_norms}")

    if bad_norms:
        print("FAIL: Gaussian-prime norm(s) found in bad zone:")
        for zone_name, n in bad_norms:
            print(f"  {zone_name}: {n}")
        return 1

    print("PASS: no Gaussian-prime norms found in BZ_I union BZ_O")
    return 0


if __name__ == "__main__":
    sys.exit(main())
