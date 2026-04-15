"""
tile.py — Single Tile Analysis

A Tile is a bounded rectangular region of the lattice graph. It:
1. Extracts the subgraph within its bounds (points + local edges).
2. Runs union-find to discover connected components (groups).
3. Identifies face-primes (ports): boundary points with cross-boundary edges.

The tile is the atomic unit of the tiled pipeline. Its output —
group labels + face ports — is what the compositor uses to reconstruct
global connectivity without revisiting individual edges.

BOUNDARY CONVENTION (critical for correctness):
A tile at grid position (col, row) covers lattice points:
    x ∈ [col * TILE_SIZE, col * TILE_SIZE + TILE_SIZE]
    y ∈ [row * TILE_SIZE, row * TILE_SIZE + TILE_SIZE]
That's TILE_SIZE+1 = 257 points per axis. Closed intervals on both sides.
Adjacent tiles SHARE their boundary row/column — the boundary belongs to
BOTH tiles. This is NOT half-open [0, 256). It's closed [0, 256].

COLLAR EXTENSION:
When r > sqrt(2), edges can reach beyond the shared boundary row/column
into the neighboring tile's interior. The collar extends the tile's sieve
domain by C lattice units in each direction so that the internal UF captures
ALL edges incident to nominal-boundary points — including those that cross
into the next tile's territory. The collar defaults to ceil(r).

    Sieve bounds: [x_lo - C, x_hi + C] × [y_lo - C, y_hi + C]
    (clamped to graph extents)

The UF runs on sieve bounds, but face-port extraction and
bottom_groups/top_groups still reference the NOMINAL boundary.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from enum import Enum
from typing import NamedTuple

from graph import Graph
from uf import UnionFind


# Tile size in lattice units. A tile covers [0, TILE_SIZE] = 257 points.
TILE_SIZE: int = 256


class Face(Enum):
    """The four faces of a tile, matching production conventions.

    I (inner/bottom): low-y row — connects to the tile below in the same tower.
    O (outer/top):    high-y row — connects to the tile above in the same tower.
    L (left):         low-x column — connects to the tile in the tower to the left.
    R (right):        high-x column — connects to the tile in the tower to the right.
    """
    I = "inner"   # bottom row (y_lo)
    O = "outer"   # top row (y_hi)
    L = "left"    # left column (x_lo)
    R = "right"   # right column (x_hi)


class Port(NamedTuple):
    """A face-prime: a boundary point that has connectivity across the tile edge.

    Attributes:
        group_id: The connected-component label of this point within the tile.
        position: Offset along the face — x offset for I/O faces, y offset for L/R faces.
                  This is the coordinate used to match ports across adjacent tiles.
        abs_x:    Absolute lattice x coordinate (for visualization/debugging).
        abs_y:    Absolute lattice y coordinate (for visualization/debugging).
    """
    group_id: int
    position: int
    abs_x: int
    abs_y: int


@dataclass
class TileResult:
    """The output of tile analysis — equivalent to a TileOp in production.

    Contains everything the compositor needs to merge this tile with neighbors:
    - group_count: number of distinct connected components
    - face_ports: for each face, the list of ports (boundary points with cross-edges)
    - group_sizes: how many points are in each group (for diagnostics)
    - bottom_groups / top_groups: group IDs present on the tile's bottom/top row.
      Needed by the compositor for the spanning check on global boundary tiles
      (not just ports — ALL groups touching the boundary row, including isolated
      boundary points with no cross-tile edges).
    """
    col: int
    row: int
    group_count: int
    face_ports: dict[Face, list[Port]] = field(default_factory=dict)
    group_sizes: dict[int, int] = field(default_factory=dict)
    bottom_groups: set[int] = field(default_factory=set)
    top_groups: set[int] = field(default_factory=set)
    alive_count: int = 0


class Tile:
    """A single tile: a bounded region of the graph.

    Computes internal connectivity (groups) and boundary ports (face-primes).
    Each tile operates on its local subgraph — edges are computed on-the-fly
    from the parent Graph's alive mask, keeping memory bounded.

    When a collar is specified (default: ceil(r)), the sieve domain extends
    beyond the nominal tile boundary so that the internal UF captures
    cross-boundary edges for r > sqrt(2).  Face-port extraction and
    bottom/top group reporting still use the NOMINAL boundary.
    """

    def __init__(self, col: int, row: int, graph: Graph,
                 collar: int | None = None) -> None:
        """Initialize a tile at grid position (col, row).

        Args:
            col:    column index in the tile grid (0-based, left to right)
            row:    row index in the tile grid (0-based, bottom to top)
            graph:  the parent lattice graph (shared, read-only)
            collar: sieve extension in lattice units beyond each nominal edge.
                    Default: ceil(r) where r is the graph's edge radius.
                    Set to 0 to disable (original behaviour).
        """
        self.col = col
        self.row = row
        self.graph = graph

        # Nominal lattice bounds: closed intervals [x_lo, x_hi] × [y_lo, y_hi].
        # Each tile is TILE_SIZE lattice units wide/tall, yielding
        # TILE_SIZE+1 lattice points per axis.
        self.x_lo: int = col * TILE_SIZE
        self.x_hi: int = col * TILE_SIZE + TILE_SIZE
        self.y_lo: int = row * TILE_SIZE
        self.y_hi: int = row * TILE_SIZE + TILE_SIZE

        # Collar: how far the sieve domain extends past the nominal boundary.
        if collar is None:
            collar = int(math.ceil(graph.r))
        self.collar: int = collar

        # Sieve bounds: nominal ± collar, clamped to graph extents.
        self.sx_lo: int = max(0, self.x_lo - collar)
        self.sx_hi: int = min(graph.width - 1, self.x_hi + collar)
        self.sy_lo: int = max(0, self.y_lo - collar)
        self.sy_hi: int = min(graph.height - 1, self.y_hi + collar)

        self._result: TileResult | None = None
        # Mapping from (x,y) → group_id for ALL sieve-domain points.
        # Populated by analyze(), consumed by the compositor for tracing.
        self.point_group: dict[tuple[int, int], int] = {}

    # -- Analysis ---------------------------------------------------------

    def analyze(self) -> TileResult:
        """Run the full tile pipeline: group discovery + port extraction.

        Steps:
        1. Enumerate all alive points in the SIEVE domain (nominal + collar).
        2. Build a local union-find over these points.
        3. For each point, check its neighbors (within the sieve bounds)
           and unite them in the UF.
        4. Assign sequential group labels 1..N.
        5. For each face, find ports: NOMINAL boundary points with at least
           one neighbor OUTSIDE the NOMINAL tile bounds.  Group labels come
           from the sieve-domain UF, so cross-boundary edges within the
           collar have already been captured.

        Returns:
            TileResult with group labels and face ports.
        """
        graph = self.graph
        # Nominal bounds (for port extraction / boundary group reporting).
        x_lo, x_hi = self.x_lo, self.x_hi
        y_lo, y_hi = self.y_lo, self.y_hi
        # Sieve bounds (for UF computation — includes collar).
        sx_lo, sx_hi = self.sx_lo, self.sx_hi
        sy_lo, sy_hi = self.sy_lo, self.sy_hi

        # Step 1: enumerate alive points in the SIEVE domain.
        point_to_idx: dict[tuple[int, int], int] = {}
        idx_to_point: list[tuple[int, int]] = []

        for y in range(sy_lo, sy_hi + 1):
            for x in range(sx_lo, sx_hi + 1):
                if graph.alive[y, x]:
                    point_to_idx[(x, y)] = len(idx_to_point)
                    idx_to_point.append((x, y))

        n_points = len(idx_to_point)
        if n_points == 0:
            self.point_group = {}
            self._result = TileResult(
                col=self.col, row=self.row, group_count=0,
                face_ports={f: [] for f in Face},
                group_sizes={}, alive_count=0,
            )
            return self._result

        # Step 2-3: build UF over sieve domain and unite neighbors.
        uf = UnionFind(n_points)

        for idx, (px, py) in enumerate(idx_to_point):
            # Unite with neighbors that are WITHIN the sieve bounds.
            for nx, ny in graph.neighbors(px, py):
                if sx_lo <= nx <= sx_hi and sy_lo <= ny <= sy_hi:
                    n_idx = point_to_idx.get((nx, ny))
                    if n_idx is not None:
                        uf.union(idx, n_idx)

        # Step 4: assign sequential group labels.
        root_to_group: dict[int, int] = {}
        group_id_counter = 0
        point_group: dict[tuple[int, int], int] = {}

        for idx, pt in enumerate(idx_to_point):
            root = uf.find(idx)
            if root not in root_to_group:
                group_id_counter += 1
                root_to_group[root] = group_id_counter
            point_group[pt] = root_to_group[root]

        # Store point_group on self for compositor tracing.
        self.point_group = point_group

        # Count alive points in the NOMINAL domain (for TileResult).
        nominal_alive = 0
        for y in range(y_lo, y_hi + 1):
            for x in range(x_lo, x_hi + 1):
                if graph.alive[y, x]:
                    nominal_alive += 1

        # Group sizes for diagnostics (sieve-domain counts).
        group_sizes: dict[int, int] = {}
        for g in point_group.values():
            group_sizes[g] = group_sizes.get(g, 0) + 1

        # Step 5: extract face ports on the NOMINAL boundary.
        # A point on a nominal face is a port if it has at least one
        # alive neighbor OUTSIDE the nominal tile bounds.  The group_id
        # comes from the sieve-domain UF, which already captured
        # cross-boundary edges through the collar.
        face_ports: dict[Face, list[Port]] = {f: [] for f in Face}

        def _has_cross_nominal_neighbor(px: int, py: int) -> bool:
            """Does (px, py) have any alive neighbor outside nominal bounds?"""
            return len(graph.cross_boundary_neighbors(
                px, py, x_lo, y_lo, x_hi, y_hi
            )) > 0

        # Face I (inner/bottom): y == y_lo. Position = x offset from x_lo.
        for x in range(x_lo, x_hi + 1):
            pt = (x, y_lo)
            if pt in point_group and _has_cross_nominal_neighbor(x, y_lo):
                face_ports[Face.I].append(Port(
                    group_id=point_group[pt],
                    position=x - x_lo,
                    abs_x=x, abs_y=y_lo,
                ))

        # Face O (outer/top): y == y_hi. Position = x offset from x_lo.
        for x in range(x_lo, x_hi + 1):
            pt = (x, y_hi)
            if pt in point_group and _has_cross_nominal_neighbor(x, y_hi):
                face_ports[Face.O].append(Port(
                    group_id=point_group[pt],
                    position=x - x_lo,
                    abs_x=x, abs_y=y_hi,
                ))

        # Face L (left): x == x_lo. Position = y offset from y_lo.
        for y in range(y_lo, y_hi + 1):
            pt = (x_lo, y)
            if pt in point_group and _has_cross_nominal_neighbor(x_lo, y):
                face_ports[Face.L].append(Port(
                    group_id=point_group[pt],
                    position=y - y_lo,
                    abs_x=x_lo, abs_y=y,
                ))

        # Face R (right): x == x_hi. Position = y offset from y_lo.
        for y in range(y_lo, y_hi + 1):
            pt = (x_hi, y)
            if pt in point_group and _has_cross_nominal_neighbor(x_hi, y):
                face_ports[Face.R].append(Port(
                    group_id=point_group[pt],
                    position=y - y_lo,
                    abs_x=x_hi, abs_y=y,
                ))

        # Step 6: record which groups appear on the NOMINAL bottom/top rows.
        bottom_groups: set[int] = set()
        for x in range(x_lo, x_hi + 1):
            pt = (x, y_lo)
            if pt in point_group:
                bottom_groups.add(point_group[pt])

        top_groups: set[int] = set()
        for x in range(x_lo, x_hi + 1):
            pt = (x, y_hi)
            if pt in point_group:
                top_groups.add(point_group[pt])

        self._result = TileResult(
            col=self.col, row=self.row,
            group_count=group_id_counter,
            face_ports=face_ports,
            group_sizes=group_sizes,
            bottom_groups=bottom_groups,
            top_groups=top_groups,
            alive_count=nominal_alive,
        )
        return self._result

    @property
    def result(self) -> TileResult:
        """Access the analysis result, computing it if needed."""
        if self._result is None:
            self.analyze()
        return self._result

    def __repr__(self) -> str:
        status = f"{self._result.group_count} groups" if self._result else "not analyzed"
        collar_str = f", collar={self.collar}" if self.collar > 1 else ""
        return (f"Tile(col={self.col}, row={self.row}, "
                f"bounds=[{self.x_lo}..{self.x_hi}]×[{self.y_lo}..{self.y_hi}]"
                f"{collar_str}, {status})")
