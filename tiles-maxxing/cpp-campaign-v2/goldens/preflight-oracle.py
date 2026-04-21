#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["gmpy2>=2.1.5"]
# ///
"""Parameterized Python oracle for cpp-campaign-v2 preflight runs."""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys
from typing import Any


HERE = pathlib.Path(__file__).resolve().parent
REFERENCE = HERE / "5tile-k36.reference.py"


def load_reference() -> Any:
    spec = importlib.util.spec_from_file_location("five_tile_reference", REFERENCE)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {REFERENCE}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def configure(ref: Any, k_sq: int, r_inner: int, r_outer: int) -> None:
    ref.K_SQ = k_sq
    ref.R_INNER = r_inner
    ref.R_OUTER = r_outer
    ref.C = ref.floor_isqrt(k_sq)
    ref.CEIL_SQRT_K = ref.ceil_isqrt(k_sq)
    ref.R_INNER_SQ = r_inner * r_inner
    ref.R_OUTER_SQ = r_outer * r_outer
    ref.GEO_INNER_UPPER_SQ = (r_inner + ref.CEIL_SQRT_K) ** 2
    ref.GEO_OUTER_LOWER_SQ = (r_outer - ref.CEIL_SQRT_K) ** 2


def active_column_bounds(ref: Any, i: int) -> tuple[int, int] | None:
    a_lo, _ = ref.tile_origin(i, 0)
    if a_lo > ref.R_OUTER:
        return None

    y_max = ref.floor_isqrt(max(0, ref.R_OUTER_SQ - max(0, a_lo) ** 2))
    j_hi_guess = (y_max - ref.OFFSET_Y) // ref.S + 1
    j_lo_guess = max(0, i - 1)
    while j_lo_guess <= j_hi_guess and not ref.is_tile_active(i, j_lo_guess):
        j_lo_guess += 1
    if j_lo_guess > j_hi_guess:
        return None

    j_hi = j_lo_guess
    while j_hi + 1 <= j_hi_guess and ref.is_tile_active(i, j_hi + 1):
        j_hi += 1
    return j_lo_guess, j_hi


def first_n_tiles(ref: Any, n_tiles: int) -> list[tuple[int, int]]:
    tiles: list[tuple[int, int]] = []
    i = 0
    while len(tiles) < n_tiles:
        bounds = active_column_bounds(ref, i)
        if bounds is not None:
            j_lo, j_hi = bounds
            for j in range(j_lo, j_hi + 1):
                tiles.append((i, j))
                if len(tiles) == n_tiles:
                    return tiles
        i += 1
        if i > (ref.R_OUTER - ref.OFFSET_X) // ref.S + 2:
            raise RuntimeError(f"ran out of active tiles before N={n_tiles}")
    return tiles


def load_tiles(path: pathlib.Path) -> list[tuple[int, int]]:
    doc = json.loads(path.read_text())
    return sorted((int(t["i"]), int(t["j"])) for t in doc["tiles"])


def manifest_path_for(snapshot_path: pathlib.Path) -> pathlib.Path:
    name = snapshot_path.name
    if name.endswith(".snapshot.bin"):
        stem = name[: -len(".snapshot.bin")]
    elif name.endswith(".bin"):
        stem = name[: -len(".bin")]
    else:
        stem = snapshot_path.stem
    return snapshot_path.with_name(stem + ".manifest.json")


def write_region(path: pathlib.Path, tiles: list[tuple[int, int]]) -> None:
    doc = {"tiles": [{"i": i, "j": j} for i, j in tiles]}
    path.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--k-sq", type=int, required=True)
    parser.add_argument("--r-inner", type=int, required=True)
    parser.add_argument("--r-outer", type=int, required=True)
    parser.add_argument("--n-tiles", type=int, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--tiles-spec", type=pathlib.Path)
    parser.add_argument("--region-output", type=pathlib.Path)
    args = parser.parse_args()

    ref = load_reference()
    configure(ref, args.k_sq, args.r_inner, args.r_outer)
    tiles = load_tiles(args.tiles_spec) if args.tiles_spec else first_n_tiles(ref, args.n_tiles)
    if len(tiles) != args.n_tiles:
        raise RuntimeError(f"expected {args.n_tiles} tiles, got {len(tiles)}")

    records = []
    for i, j in tiles:
        if not ref.is_tile_active(i, j):
            raise RuntimeError(f"tile ({i},{j}) is not active")
        records.append(ref.process_tile(i, j))

    blob, manifest = ref.build_snapshot(records, tiles)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    manifest_path_for(args.output).write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    if args.region_output:
        args.region_output.parent.mkdir(parents=True, exist_ok=True)
        write_region(args.region_output, tiles)

    print(f"oracle wrote {args.output} ({len(tiles)} tiles)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
