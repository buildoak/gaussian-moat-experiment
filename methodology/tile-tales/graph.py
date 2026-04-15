"""
graph.py — Lattice Point Graph

Represents a 2D integer lattice where points are connected if their
Euclidean distance ≤ r. The default r = sqrt(2) gives standard
8-connectivity (horizontal, vertical, and diagonal neighbors).

MEMORY MODEL:
The full grid for a 64×32 tile layout is 16384×8192 ≈ 134M points.
Storing explicit edge tuples for ~1 billion edges would be catastrophic.
Instead, the graph stores a 2D boolean numpy array (alive mask) and edges
are computed on-the-fly by consumers (Tile, DirectVerifier) who only need
local neighborhoods at a time.

The graph is the shared source of truth — tiles and the direct verifier
both read from the same alive mask, ensuring they see identical topology.
"""

from __future__ import annotations

import math
from typing import Iterator

import numpy as np
from numpy.typing import NDArray


# Pre-computed neighbor offsets for integer-lattice connectivity.
# For r = sqrt(2): 8 neighbors (king moves).
# For r = sqrt(5): 8-connectivity + knight-move-ish neighbors at dist=2 and sqrt(5).
def _neighbor_offsets(r: float) -> list[tuple[int, int]]:
    """Return all (dx, dy) offsets with 0 < sqrt(dx²+dy²) ≤ r, dx/dy integers."""
    r_sq = r * r
    bound = int(math.ceil(r))
    offsets: list[tuple[int, int]] = []
    for dx in range(-bound, bound + 1):
        for dy in range(-bound, bound + 1):
            if dx == 0 and dy == 0:
                continue
            if dx * dx + dy * dy <= r_sq + 1e-9:  # epsilon for float comparison
                offsets.append((dx, dy))
    return offsets


class Graph:
    """A 2D lattice point graph with distance-based edges.

    Points live on an integer grid [0, width) × [0, height).
    An 'alive' mask tracks which points exist — removing a point
    removes it and all its incident edges.

    Edges are implicit: two alive points are connected iff their
    Euclidean distance ≤ r. This avoids materializing billions of edges.
    """

    def __init__(self, width: int, height: int, r: float = math.sqrt(2)) -> None:
        """Create a full lattice of width × height points, all alive.

        Args:
            width:  number of lattice columns (x ranges 0..width-1)
            height: number of lattice rows    (y ranges 0..height-1)
            r:      connectivity radius. Default sqrt(2) = 8-connectivity.
        """
        self.width: int = width
        self.height: int = height
        self.r: float = r
        self.r_sq: float = r * r

        # Alive mask: True = point exists. Shape (height, width) — row-major.
        self.alive: NDArray[np.bool_] = np.ones((height, width), dtype=np.bool_)

        # Pre-computed neighbor offsets for this radius.
        self.offsets: list[tuple[int, int]] = _neighbor_offsets(r)

        # Optional: set of explicitly removed edges (as frozensets of point tuples).
        # Used for surgical edge removal without killing whole points.
        self._removed_edges: set[frozenset[tuple[int, int]]] = set()

    # -- Point manipulation -----------------------------------------------

    def remove_point(self, x: int, y: int) -> None:
        """Remove a single lattice point (and implicitly all its edges)."""
        self.alive[y, x] = False

    def remove_points(self, points: list[tuple[int, int]]) -> None:
        """Batch-remove a list of (x, y) points."""
        for x, y in points:
            self.alive[y, x] = False

    def remove_horizontal_band(self, y_lo: int, y_hi: int,
                               x_lo: int = 0, x_hi: int | None = None) -> None:
        """Remove all points in rows [y_lo, y_hi] within columns [x_lo, x_hi].

        Useful for creating moats (horizontal barriers that block spanning).
        """
        if x_hi is None:
            x_hi = self.width - 1
        self.alive[y_lo:y_hi + 1, x_lo:x_hi + 1] = False

    def restore_point(self, x: int, y: int) -> None:
        """Re-add a previously removed point."""
        self.alive[y, x] = True

    # -- Edge manipulation ------------------------------------------------

    def remove_edge(self, p1: tuple[int, int], p2: tuple[int, int]) -> None:
        """Remove a specific edge without removing either endpoint."""
        self._removed_edges.add(frozenset((p1, p2)))

    def edge_exists(self, x1: int, y1: int, x2: int, y2: int) -> bool:
        """Check if an edge exists between two points.

        Both points must be alive, within radius, and not explicitly removed.
        """
        if not self.alive[y1, x1] or not self.alive[y2, x2]:
            return False
        dx, dy = x2 - x1, y2 - y1
        if dx * dx + dy * dy > self.r_sq + 1e-9:
            return False
        if frozenset(((x1, y1), (x2, y2))) in self._removed_edges:
            return False
        return True

    # -- Neighbor queries -------------------------------------------------

    def neighbors(self, x: int, y: int) -> Iterator[tuple[int, int]]:
        """Yield all alive neighbors of (x, y) within radius r.

        This is the fundamental edge query. Consumers iterate neighbors
        rather than loading an edge list — keeping memory bounded.
        """
        for dx, dy in self.offsets:
            nx, ny = x + dx, y + dy
            if 0 <= nx < self.width and 0 <= ny < self.height:
                if self.alive[ny, nx]:
                    if frozenset(((x, y), (nx, ny))) not in self._removed_edges:
                        yield (nx, ny)

    # -- Boundary queries -------------------------------------------------

    def points_on_row(self, y: int, x_lo: int = 0,
                      x_hi: int | None = None) -> list[tuple[int, int]]:
        """Return alive points on row y within [x_lo, x_hi]."""
        if x_hi is None:
            x_hi = self.width - 1
        return [(x, y) for x in range(x_lo, x_hi + 1) if self.alive[y, x]]

    def points_on_col(self, x: int, y_lo: int = 0,
                      y_hi: int | None = None) -> list[tuple[int, int]]:
        """Return alive points on column x within [y_lo, y_hi]."""
        if y_hi is None:
            y_hi = self.height - 1
        return [(x, y) for y in range(y_lo, y_hi + 1) if self.alive[y, x]]

    def cross_boundary_neighbors(
        self, x: int, y: int,
        x_lo: int, y_lo: int, x_hi: int, y_hi: int,
    ) -> list[tuple[int, int]]:
        """Return neighbors of (x,y) that lie OUTSIDE the box [x_lo..x_hi] × [y_lo..y_hi].

        Used to find face-primes: boundary points whose edges cross into
        adjacent tile territory. A point on a tile's edge is a face-prime
        if it has at least one neighbor in the neighboring tile.
        """
        result: list[tuple[int, int]] = []
        for nx, ny in self.neighbors(x, y):
            if nx < x_lo or nx > x_hi or ny < y_lo or ny > y_hi:
                result.append((nx, ny))
        return result

    # -- Stats ------------------------------------------------------------

    def alive_count(self) -> int:
        """Number of alive points."""
        return int(np.sum(self.alive))

    def __repr__(self) -> str:
        return (f"Graph(width={self.width}, height={self.height}, "
                f"r={self.r:.3f}, alive={self.alive_count()})")
