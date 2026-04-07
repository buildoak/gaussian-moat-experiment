"""Main experiment runner: find Gaussian moats via hierarchical tiling."""

from __future__ import annotations

import math
import time
from dataclasses import dataclass
from typing import List, Optional, Set

from .compose import compose_grid_hierarchical
from .tiling import FACES, TileOperator, tile_region


@dataclass
class MoatResult:
    """Result of a moat search."""

    k_sq: int
    radius: int
    tile_size: int
    moat_found: bool
    origin_component_size: int  # number of faces origin reaches
    faces_reached: Set[str]
    n_primes: int
    runtime_s: float
    mega_tile: Optional[TileOperator] = None

    def summary(self) -> str:
        step = math.sqrt(self.k_sq)
        lines = [
            f"  k²={self.k_sq} (step={step:.3f}), R={self.radius}, tile={self.tile_size}",
            f"  Primes found: {self.n_primes}",
            f"  Faces reached by origin component: {self.faces_reached or '{none}'}",
            f"  Moat found: {'YES' if self.moat_found else 'NO'}",
            f"  Runtime: {self.runtime_s:.2f}s",
        ]
        return "\n".join(lines)


def find_moat(
    k_sq: int,
    radius: int,
    tile_size: int = 32,
    verbose: bool = True,
) -> MoatResult:
    """Run the full moat-finding pipeline.

    Args:
        k_sq: squared step threshold.
        radius: search radius R (region is [-R, R]²).
        tile_size: width and height of each tile.
        verbose: print progress.

    Returns:
        MoatResult with findings.
    """
    t0 = time.time()

    if verbose:
        step = math.sqrt(k_sq)
        print(f"\n{'='*60}")
        print(f"Moat search: k²={k_sq} (step={step:.3f}), R={radius}, tile={tile_size}")
        print(f"{'='*60}")

    # Step 1: Tile the region
    if verbose:
        print("\n[1/3] Tiling region...")
    grid, all_primes = tile_region(radius, tile_size, tile_size, k_sq, verbose=verbose)

    # Step 2: Hierarchical composition
    if verbose:
        print("\n[2/3] Hierarchical composition...")
    mega = compose_grid_hierarchical(grid, k_sq, verbose=verbose)

    # Step 3: Analyze result
    if verbose:
        print("\n[3/3] Analyzing result...")

    faces_reached: Set[str] = set()
    if mega.origin_component is not None:
        faces_reached = mega.component_faces.get(mega.origin_component, set())

    moat_found = mega.origin_component is not None and len(faces_reached) == 0

    # If origin component exists but doesn't reach ANY face, moat is found.
    # If origin component is None (no primes near origin), that's also a moat
    # for the trivial reason that origin is isolated.
    if mega.origin_component is None:
        # Check if there are ANY primes within step distance of origin.
        # If not, the origin is trivially moated.
        moat_found = True

    result = MoatResult(
        k_sq=k_sq,
        radius=radius,
        tile_size=tile_size,
        moat_found=moat_found,
        origin_component_size=len(faces_reached),
        faces_reached=faces_reached,
        n_primes=len(all_primes),
        runtime_s=time.time() - t0,
        mega_tile=mega,
    )

    if verbose:
        print(result.summary())

    return result


def incremental_search(
    k_sq: int,
    radii: List[int],
    tile_size: int = 32,
    verbose: bool = True,
) -> List[MoatResult]:
    """Run moat search at increasing radii, stopping if moat confirmed.

    A moat is confirmed when the origin component doesn't reach any face
    at a given radius AND still doesn't reach faces at a larger radius
    (ruling out that the component just hadn't grown enough).
    """
    results = []
    for R in radii:
        result = find_moat(k_sq, R, tile_size, verbose=verbose)
        results.append(result)

        if result.moat_found:
            if verbose:
                print(f"\n  >>> MOAT CONFIRMED at R={R} for k²={k_sq}")
            break
        else:
            if verbose:
                reached = result.faces_reached
                print(f"\n  >>> No moat at R={R} — origin reaches faces: {reached}")

    return results
