#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["gmpy2>=2.1.5"]
# ///
"""CPU-only checks for the parameterized preflight oracle geo predicates."""

from __future__ import annotations

import importlib.util
import math
import pathlib
import sys
from typing import Any


HERE = pathlib.Path(__file__).resolve().parent
PREFLIGHT_ORACLE = HERE / "preflight-oracle.py"


def load_module(path: pathlib.Path, name: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def canonical_inner_upper(r: int, k_sq: int) -> int:
    return r * r + k_sq + math.isqrt(4 * r * r * k_sq)


def canonical_outer_lower(r: int, k_sq: int) -> int:
    return r * r + k_sq - math.isqrt(4 * r * r * k_sq)


def ceil_isqrt(n: int) -> int:
    root = math.isqrt(n)
    return root if root * root == n else root + 1


def widened_inner_upper(r: int, k_sq: int) -> int:
    width = ceil_isqrt(k_sq)
    return (r + width) * (r + width)


def widened_outer_lower(r: int, k_sq: int) -> int:
    width = ceil_isqrt(k_sq)
    return (r - width) * (r - width)


def configured_reference(k_sq: int, r_inner: int, r_outer: int) -> Any:
    oracle = load_module(PREFLIGHT_ORACLE, f"preflight_oracle_{k_sq}")
    ref = oracle.load_reference()
    oracle.configure(ref, k_sq, r_inner, r_outer)
    return ref


def assert_square_k_matches_historical_band() -> None:
    ref = configured_reference(36, 10_000, 20_000)
    inner_hi = canonical_inner_upper(ref.R_INNER, ref.K_SQ)
    outer_lo = canonical_outer_lower(ref.R_OUTER, ref.K_SQ)
    assert inner_hi == widened_inner_upper(ref.R_INNER, ref.K_SQ)
    assert outer_lo == widened_outer_lower(ref.R_OUTER, ref.K_SQ)
    assert ref.geo_inner_flag(inner_hi)
    assert not ref.geo_inner_flag(inner_hi + 1)
    assert ref.geo_outer_flag(outer_lo)
    assert not ref.geo_outer_flag(outer_lo - 1)


def assert_non_square_rejects_ceil_only_gap(k_sq: int) -> None:
    ref = configured_reference(k_sq, 10_000, 20_000)

    inner_hi = canonical_inner_upper(ref.R_INNER, ref.K_SQ)
    inner_wide_hi = widened_inner_upper(ref.R_INNER, ref.K_SQ)
    assert inner_hi < inner_wide_hi
    assert ref.geo_inner_flag(inner_hi)
    assert not ref.geo_inner_flag(inner_hi + 1)
    assert inner_hi + 1 <= inner_wide_hi

    outer_lo = canonical_outer_lower(ref.R_OUTER, ref.K_SQ)
    outer_wide_lo = widened_outer_lower(ref.R_OUTER, ref.K_SQ)
    assert outer_wide_lo < outer_lo
    assert ref.geo_outer_flag(outer_lo)
    assert not ref.geo_outer_flag(outer_lo - 1)
    assert outer_lo - 1 >= outer_wide_lo


def main() -> int:
    assert_square_k_matches_historical_band()
    assert_non_square_rejects_ceil_only_gap(38)
    assert_non_square_rejects_ceil_only_gap(40)
    print("preflight oracle geo predicates use canonical norm form")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
