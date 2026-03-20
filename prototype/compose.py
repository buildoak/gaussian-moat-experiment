"""Hierarchical composition of TileOperators."""

from __future__ import annotations

import math
from typing import Dict, List, Optional, Set, Tuple

from .tiling import BOTTOM, FACES, LEFT, RIGHT, TOP, TileOperator
from .unionfind import UnionFind


def _seam_merge(
    ports_a: List[Tuple[int, int, int]],
    ports_b: List[Tuple[int, int, int]],
    k_sq: int,
) -> List[Tuple[int, int]]:
    """Find pairs of component IDs that should be merged across a seam.

    ports_a: face ports from tile A's seam face.
    ports_b: face ports from tile B's seam face.
    k_sq: squared step threshold.

    Returns list of (comp_a, comp_b) pairs to merge.
    """
    merges = []
    for (a1, b1, c1) in ports_a:
        for (a2, b2, c2) in ports_b:
            da = a1 - a2
            db = b1 - b2
            if da * da + db * db <= k_sq:
                merges.append((c1, c2))
    return merges


def compose_vertical(top_tile: TileOperator, bot_tile: TileOperator, k_sq: int) -> TileOperator:
    """Compose two vertically adjacent tiles (bot below top).

    bot_tile's TOP face meets top_tile's BOTTOM face.
    Result keeps: top_tile's TOP, bot_tile's BOTTOM, merged LEFT/RIGHT.
    """
    # Collect all unique component IDs from both tiles
    all_comps = set()
    for f in FACES:
        for (_, _, c) in top_tile.face_ports.get(f, []):
            all_comps.add(("T", c))
        for (_, _, c) in bot_tile.face_ports.get(f, []):
            all_comps.add(("B", c))

    # Also include origin components
    if top_tile.origin_component is not None:
        all_comps.add(("T", top_tile.origin_component))
    if bot_tile.origin_component is not None:
        all_comps.add(("B", bot_tile.origin_component))

    comp_list = sorted(all_comps)
    comp_to_idx = {c: i for i, c in enumerate(comp_list)}
    uf = UnionFind(len(comp_list))

    # Find seam merges: bot's TOP meets top's BOTTOM
    seam_top = [(a, b, ("B", c)) for (a, b, c) in bot_tile.face_ports.get(TOP, [])]
    seam_bot = [(a, b, ("T", c)) for (a, b, c) in top_tile.face_ports.get(BOTTOM, [])]

    for (a1, b1, c1) in seam_top:
        for (a2, b2, c2) in seam_bot:
            da = a1 - a2
            db = b1 - b2
            if da * da + db * db <= k_sq:
                if c1 in comp_to_idx and c2 in comp_to_idx:
                    uf.union(comp_to_idx[c1], comp_to_idx[c2])

    # Build result tile
    result = TileOperator(
        a_min=min(top_tile.a_min, bot_tile.a_min),
        a_max=max(top_tile.a_max, bot_tile.a_max),
        b_min=bot_tile.b_min,
        b_max=top_tile.b_max,
    )

    # Result faces:
    # TOP from top_tile, BOTTOM from bot_tile, LEFT/RIGHT merged from both
    def _remap(face_ports, prefix):
        return [(a, b, uf.find(comp_to_idx[(prefix, c)])) for (a, b, c) in face_ports]

    result.face_ports[TOP] = _remap(top_tile.face_ports.get(TOP, []), "T")
    result.face_ports[BOTTOM] = _remap(bot_tile.face_ports.get(BOTTOM, []), "B")
    result.face_ports[LEFT] = (
        _remap(top_tile.face_ports.get(LEFT, []), "T")
        + _remap(bot_tile.face_ports.get(LEFT, []), "B")
    )
    result.face_ports[RIGHT] = (
        _remap(top_tile.face_ports.get(RIGHT, []), "T")
        + _remap(bot_tile.face_ports.get(RIGHT, []), "B")
    )

    # Component faces
    result.component_faces = {}
    for f in FACES:
        for (_, _, c) in result.face_ports[f]:
            if c not in result.component_faces:
                result.component_faces[c] = set()
            result.component_faces[c].add(f)

    # Origin component
    if top_tile.origin_component is not None:
        result.origin_component = uf.find(comp_to_idx[("T", top_tile.origin_component)])
    elif bot_tile.origin_component is not None:
        result.origin_component = uf.find(comp_to_idx[("B", bot_tile.origin_component)])
    else:
        result.origin_component = None

    return result


def compose_horizontal(left_tile: TileOperator, right_tile: TileOperator, k_sq: int) -> TileOperator:
    """Compose two horizontally adjacent tiles (left | right).

    left_tile's RIGHT face meets right_tile's LEFT face.
    Result keeps: left_tile's LEFT, right_tile's RIGHT, merged TOP/BOTTOM.
    """
    all_comps = set()
    for f in FACES:
        for (_, _, c) in left_tile.face_ports.get(f, []):
            all_comps.add(("L", c))
        for (_, _, c) in right_tile.face_ports.get(f, []):
            all_comps.add(("R", c))

    if left_tile.origin_component is not None:
        all_comps.add(("L", left_tile.origin_component))
    if right_tile.origin_component is not None:
        all_comps.add(("R", right_tile.origin_component))

    comp_list = sorted(all_comps)
    comp_to_idx = {c: i for i, c in enumerate(comp_list)}
    uf = UnionFind(len(comp_list))

    # Seam: left's RIGHT meets right's LEFT
    seam_l = [(a, b, ("L", c)) for (a, b, c) in left_tile.face_ports.get(RIGHT, [])]
    seam_r = [(a, b, ("R", c)) for (a, b, c) in right_tile.face_ports.get(LEFT, [])]

    for (a1, b1, c1) in seam_l:
        for (a2, b2, c2) in seam_r:
            da = a1 - a2
            db = b1 - b2
            if da * da + db * db <= k_sq:
                if c1 in comp_to_idx and c2 in comp_to_idx:
                    uf.union(comp_to_idx[c1], comp_to_idx[c2])

    result = TileOperator(
        a_min=left_tile.a_min,
        a_max=right_tile.a_max,
        b_min=min(left_tile.b_min, right_tile.b_min),
        b_max=max(left_tile.b_max, right_tile.b_max),
    )

    def _remap(face_ports, prefix):
        return [(a, b, uf.find(comp_to_idx[(prefix, c)])) for (a, b, c) in face_ports]

    result.face_ports[LEFT] = _remap(left_tile.face_ports.get(LEFT, []), "L")
    result.face_ports[RIGHT] = _remap(right_tile.face_ports.get(RIGHT, []), "R")
    result.face_ports[TOP] = (
        _remap(left_tile.face_ports.get(TOP, []), "L")
        + _remap(right_tile.face_ports.get(TOP, []), "R")
    )
    result.face_ports[BOTTOM] = (
        _remap(left_tile.face_ports.get(BOTTOM, []), "L")
        + _remap(right_tile.face_ports.get(BOTTOM, []), "R")
    )

    result.component_faces = {}
    for f in FACES:
        for (_, _, c) in result.face_ports[f]:
            if c not in result.component_faces:
                result.component_faces[c] = set()
            result.component_faces[c].add(f)

    if left_tile.origin_component is not None:
        result.origin_component = uf.find(comp_to_idx[("L", left_tile.origin_component)])
    elif right_tile.origin_component is not None:
        result.origin_component = uf.find(comp_to_idx[("R", right_tile.origin_component)])
    else:
        result.origin_component = None

    return result


def compose_grid_hierarchical(
    grid: List[List[TileOperator]], k_sq: int, verbose: bool = True
) -> TileOperator:
    """Hierarchically compose a grid of TileOperators into one mega-tile.

    Strategy: alternating horizontal and vertical sweeps.
    1. Compose each row horizontally (reduce columns)
    2. Compose resulting column vertically (reduce rows)
    Repeat until one tile remains.
    """
    n_rows = len(grid)
    n_cols = len(grid[0]) if n_rows > 0 else 0

    if verbose:
        print(f"  Hierarchical composition: {n_rows}×{n_cols} grid")

    level = 0
    current = [row[:] for row in grid]  # copy

    while len(current) > 1 or (len(current) == 1 and len(current[0]) > 1):
        level += 1

        # Step 1: horizontal pairwise composition within each row
        if len(current[0]) > 1:
            new_grid = []
            for ri, row in enumerate(current):
                new_row = []
                i = 0
                while i < len(row):
                    if i + 1 < len(row):
                        merged = compose_horizontal(row[i], row[i + 1], k_sq)
                        new_row.append(merged)
                        i += 2
                    else:
                        new_row.append(row[i])
                        i += 1
                new_grid.append(new_row)
            current = new_grid
            if verbose:
                print(f"    Level {level}h: {len(current)}×{len(current[0])} after horizontal merge")

        # Step 2: vertical pairwise composition across rows
        if len(current) > 1:
            new_grid = []
            i = 0
            while i < len(current):
                if i + 1 < len(current):
                    merged_row = []
                    for ci in range(len(current[i])):
                        # current[i] is the lower row, current[i+1] is upper
                        merged = compose_vertical(current[i + 1][ci], current[i][ci], k_sq)
                        merged_row.append(merged)
                    new_grid.append(merged_row)
                    i += 2
                else:
                    new_grid.append(current[i])
                    i += 1
            current = new_grid
            if verbose:
                print(f"    Level {level}v: {len(current)}×{len(current[0])} after vertical merge")

    if verbose:
        print(f"  Composition complete after {level} levels")

    return current[0][0]
