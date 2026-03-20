"""Rectangular tiling and per-tile connectivity via union-find."""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Dict, List, Set, Tuple

import numpy as np

from .primes import gaussian_primes_in_rect
from .unionfind import UnionFind

# Face identifiers
TOP = "top"
BOTTOM = "bottom"
LEFT = "left"
RIGHT = "right"
FACES = [TOP, BOTTOM, LEFT, RIGHT]


@dataclass
class TileOperator:
    """Boundary operator for a rectangular tile.

    Attributes:
        a_min, a_max, b_min, b_max: logical extent of this tile (no collar).
        face_ports: face -> list of (a, b, component_id).
        component_faces: component_id -> set of faces it touches.
        origin_component: component_id that contains (0,0), or None.
        n_primes: total number of primes in the tile (including collar).
        n_components: total connected components.
    """

    a_min: int
    a_max: int
    b_min: int
    b_max: int
    face_ports: Dict[str, List[Tuple[int, int, int]]] = field(default_factory=dict)
    component_faces: Dict[int, Set[str]] = field(default_factory=dict)
    origin_component: int | None = None
    n_primes: int = 0
    n_components: int = 0


def _face_of_prime(
    a: int,
    b: int,
    a_min: int,
    a_max: int,
    b_min: int,
    b_max: int,
    collar: int,
) -> List[str]:
    """Return which faces of the logical tile a prime is on/near (within collar of boundary)."""
    faces = []
    if b >= b_max - collar + 1:
        faces.append(TOP)
    if b <= b_min + collar - 1:
        faces.append(BOTTOM)
    if a <= a_min + collar - 1:
        faces.append(LEFT)
    if a >= a_max - collar + 1:
        faces.append(RIGHT)
    return faces


def build_tile_operator(
    a_min: int,
    a_max: int,
    b_min: int,
    b_max: int,
    k_sq: int,
    all_primes: np.ndarray | None = None,
) -> TileOperator:
    """Build a TileOperator for a single tile.

    Args:
        a_min, a_max, b_min, b_max: logical tile extents.
        k_sq: squared step threshold (primes within sqrt(k_sq) are adjacent).
        all_primes: optional precomputed primes array (N,2). If None, generates them.

    Returns:
        TileOperator with boundary information.
    """
    collar = math.ceil(math.sqrt(k_sq))

    # Generate primes in tile + collar
    ca_min = a_min - collar
    ca_max = a_max + collar
    cb_min = b_min - collar
    cb_max = b_max + collar

    if all_primes is not None:
        # Filter from precomputed primes
        mask = (
            (all_primes[:, 0] >= ca_min)
            & (all_primes[:, 0] <= ca_max)
            & (all_primes[:, 1] >= cb_min)
            & (all_primes[:, 1] <= cb_max)
        )
        primes = all_primes[mask]
    else:
        primes = gaussian_primes_in_rect(ca_min, ca_max, cb_min, cb_max)

    n = len(primes)
    op = TileOperator(a_min=a_min, a_max=a_max, b_min=b_min, b_max=b_max)
    op.n_primes = n

    if n == 0:
        op.n_components = 0
        for f in FACES:
            op.face_ports[f] = []
        return op

    # Build adjacency via direct distance checking
    uf = UnionFind(n)

    # Cell-list acceleration: bucket primes into cells of side collar
    cell_side = max(collar, 1)
    prime_list = [(int(primes[i, 0]), int(primes[i, 1])) for i in range(n)]

    cells: Dict[Tuple[int, int], List[int]] = {}
    for idx, (a, b) in enumerate(prime_list):
        cx, cy = a // cell_side, b // cell_side
        cells.setdefault((cx, cy), []).append(idx)

    # For each prime, check neighboring cells
    for idx, (a, b) in enumerate(prime_list):
        cx, cy = a // cell_side, b // cell_side
        for dcx in range(-2, 3):
            for dcy in range(-2, 3):
                nbr_cell = (cx + dcx, cy + dcy)
                if nbr_cell not in cells:
                    continue
                for jdx in cells[nbr_cell]:
                    if jdx <= idx:
                        continue
                    da = prime_list[jdx][0] - a
                    db = prime_list[jdx][1] - b
                    if da * da + db * db <= k_sq:
                        uf.union(idx, jdx)

    op.n_components = uf.count

    # Map primes to components (using find to get canonical labels)
    comp_labels = [uf.find(i) for i in range(n)]

    # Identify boundary primes and face ports
    for f in FACES:
        op.face_ports[f] = []
    op.component_faces = {}

    for idx, (a, b) in enumerate(prime_list):
        comp = comp_labels[idx]
        # Only consider primes within the logical tile for face assignment
        if a < a_min or a > a_max or b < b_min or b > b_max:
            continue
        faces = _face_of_prime(a, b, a_min, a_max, b_min, b_max, collar)
        for f in faces:
            op.face_ports[f].append((a, b, comp))
            if comp not in op.component_faces:
                op.component_faces[comp] = set()
            op.component_faces[comp].add(f)

    # Check if origin is in this tile
    if a_min <= 0 <= a_max and b_min <= 0 <= b_max:
        for idx, (a, b) in enumerate(prime_list):
            if a == 0 and b == 0:
                # (0,0) is not a Gaussian prime, but we need to find which
                # component the origin connects to. Check all primes within
                # step distance of origin.
                pass

        # Origin (0,0) is NOT a Gaussian prime. Find which component it
        # connects to: any Gaussian prime within step distance of origin.
        origin_neighbors = []
        for idx, (a, b) in enumerate(prime_list):
            if a * a + b * b <= k_sq:
                origin_neighbors.append(idx)

        if origin_neighbors:
            # Union all origin neighbors together, then record that component
            for i in range(1, len(origin_neighbors)):
                uf.union(origin_neighbors[0], origin_neighbors[i])
            op.origin_component = uf.find(origin_neighbors[0])
            # Update comp_labels since unions may have changed
            comp_labels = [uf.find(i) for i in range(n)]
            # Rebuild face ports with updated labels
            for f in FACES:
                op.face_ports[f] = []
            op.component_faces = {}
            for idx, (a, b) in enumerate(prime_list):
                comp = comp_labels[idx]
                if a < a_min or a > a_max or b < b_min or b > b_max:
                    continue
                faces = _face_of_prime(a, b, a_min, a_max, b_min, b_max, collar)
                for f in faces:
                    op.face_ports[f].append((a, b, comp))
                    if comp not in op.component_faces:
                        op.component_faces[comp] = set()
                    op.component_faces[comp].add(f)
        else:
            op.origin_component = None

    return op


def tile_region(
    R: int, tile_w: int, tile_h: int, k_sq: int, verbose: bool = True
) -> Tuple[List[List[TileOperator]], np.ndarray]:
    """Tile [-R, R] × [-R, R] into a grid of TileOperators.

    Returns:
        (grid, all_primes) where grid[row][col] is a TileOperator,
        and all_primes is the full prime array.
    """
    collar = math.ceil(math.sqrt(k_sq))

    # Generate all primes once for the full region + collar
    if verbose:
        print(f"  Generating primes in [{-R - collar}, {R + collar}]²...")
    all_primes = gaussian_primes_in_rect(
        -R - collar, R + collar, -R - collar, R + collar
    )
    if verbose:
        print(f"  Found {len(all_primes)} Gaussian primes")

    # Create tile grid
    # Tiles cover [-R, R] in both axes
    a_starts = list(range(-R, R, tile_w))
    b_starts = list(range(-R, R, tile_h))

    n_cols = len(a_starts)
    n_rows = len(b_starts)

    if verbose:
        print(f"  Tiling into {n_rows}×{n_cols} grid ({n_rows * n_cols} tiles)")

    grid: List[List[TileOperator]] = []
    for ri, b_start in enumerate(b_starts):
        row = []
        for ci, a_start in enumerate(a_starts):
            a_lo = a_start
            a_hi = min(a_start + tile_w - 1, R)
            b_lo = b_start
            b_hi = min(b_start + tile_h - 1, R)

            op = build_tile_operator(a_lo, a_hi, b_lo, b_hi, k_sq, all_primes)
            row.append(op)
        grid.append(row)

    if verbose:
        total_primes = sum(op.n_primes for row in grid for op in row)
        print(f"  Total primes across tiles (with overlaps): {total_primes}")

    return grid, all_primes
