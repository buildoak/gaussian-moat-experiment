"""
grid.py — Tower-Based Tile Grid

Manages a 2D grid of tiles organized into vertical towers, matching the
production pipeline's tower-based layout. Tracks tile positions, neighbor
relationships, and which tiles carry global boundary exposure.

LAYOUT:
- Towers are vertical columns of tiles. Tower 0 is the leftmost.
- Within a tower, row 0 is the bottom (inner boundary), row N-1 is the top.
- The full grid is n_cols towers wide × n_rows tiles tall.
- Total lattice dimensions: (n_cols × 256 + 1) columns × (n_rows × 256 + 1) rows.
  The +1 comes from the fencepost: tile 0 covers [0, 256], tile 1 covers [256, 512],
  and the shared boundary point at 256 is counted once in the lattice.

NEIGHBOR RELATIONSHIPS:
- Vertical (within-tower): tile (c, r) faces tile (c, r+1) via O/I faces.
- Horizontal (between-tower): tile (c, r) faces tile (c+1, r) via R/L faces.
"""

from __future__ import annotations

import sys
from typing import Iterator

from graph import Graph
from tile import Face, Tile, TileResult, TILE_SIZE


class Grid:
    """Tower-based grid of tiles. Manages tile positions and neighbor relationships.

    The grid is the bridge between the raw Graph and the Compositor.
    It partitions the graph into tiles, runs per-tile analysis, and
    provides iteration over neighbor pairs for composition.
    """

    def __init__(self, graph: Graph, n_cols: int, n_rows: int,
                 collar: int | None = None) -> None:
        """Create a grid of n_cols × n_rows tiles over the given graph.

        Args:
            graph:  the lattice graph to partition
            n_cols: number of tile columns (towers)
            n_rows: number of tile rows per tower
            collar: sieve extension per tile (default: ceil(r))
        """
        self.graph = graph
        self.n_cols = n_cols
        self.n_rows = n_rows

        # Verify dimensions match. The lattice must be exactly
        # (n_cols * TILE_SIZE + 1) × (n_rows * TILE_SIZE + 1) points.
        expected_w = n_cols * TILE_SIZE + 1
        expected_h = n_rows * TILE_SIZE + 1
        if graph.width != expected_w or graph.height != expected_h:
            raise ValueError(
                f"Grid {n_cols}×{n_rows} tiles expects lattice "
                f"{expected_w}×{expected_h}, got {graph.width}×{graph.height}"
            )

        # Create tile objects. tiles[col][row] = Tile.
        self.tiles: list[list[Tile]] = [
            [Tile(col, row, graph, collar=collar) for row in range(n_rows)]
            for col in range(n_cols)
        ]

    # -- Tile access ------------------------------------------------------

    def tile_at(self, col: int, row: int) -> Tile:
        """Get the tile at grid position (col, row)."""
        return self.tiles[col][row]

    def all_tiles(self) -> Iterator[Tile]:
        """Iterate over all tiles in column-major order (tower by tower)."""
        for col in range(self.n_cols):
            for row in range(self.n_rows):
                yield self.tiles[col][row]

    # -- Analysis ---------------------------------------------------------

    def analyze_all(self, progress: bool = True) -> None:
        """Run per-tile analysis on every tile in the grid.

        This is the "embarrassingly parallel" step — each tile is independent.
        In production this runs on GPU; here we do it sequentially.
        """
        total = self.n_cols * self.n_rows
        done = 0
        for tile in self.all_tiles():
            tile.analyze()
            done += 1
            if progress and done % max(1, total // 20) == 0:
                pct = 100 * done / total
                print(f"  tiles analyzed: {done}/{total} ({pct:.0f}%)",
                      file=sys.stderr, flush=True)
        if progress:
            print(f"  tiles analyzed: {done}/{total} (100%)",
                  file=sys.stderr, flush=True)

    # -- Neighbor iteration -----------------------------------------------

    def vertical_pairs(self) -> Iterator[tuple[Tile, Tile]]:
        """Yield (lower_tile, upper_tile) for all vertical adjacencies.

        lower_tile's O face matches upper_tile's I face.
        They share the boundary row at y = lower_tile.y_hi = upper_tile.y_lo.
        """
        for col in range(self.n_cols):
            for row in range(self.n_rows - 1):
                yield (self.tiles[col][row], self.tiles[col][row + 1])

    def horizontal_pairs(self) -> Iterator[tuple[Tile, Tile]]:
        """Yield (left_tile, right_tile) for all horizontal adjacencies.

        left_tile's R face matches right_tile's L face.
        They share the boundary column at x = left_tile.x_hi = right_tile.x_lo.
        """
        for col in range(self.n_cols - 1):
            for row in range(self.n_rows):
                yield (self.tiles[col][row], self.tiles[col + 1][row])

    def diagonal_pairs(self) -> Iterator[tuple[Tile, Tile]]:
        """Yield all diagonally adjacent tile pairs.

        These have overlapping sieve domains only when collar > 0.
        Each pair is yielded once: (lower-left, upper-right) and
        (lower-right, upper-left).
        """
        for col in range(self.n_cols - 1):
            for row in range(self.n_rows - 1):
                # (col,row) and (col+1,row+1) — NE diagonal
                yield (self.tiles[col][row], self.tiles[col + 1][row + 1])
                # (col+1,row) and (col,row+1) — NW diagonal
                yield (self.tiles[col + 1][row], self.tiles[col][row + 1])

    # -- Boundary exposure ------------------------------------------------

    def inner_boundary_tiles(self) -> list[Tile]:
        """Tiles on the bottom row (row 0). Their I face is the global inner boundary."""
        return [self.tiles[col][0] for col in range(self.n_cols)]

    def outer_boundary_tiles(self) -> list[Tile]:
        """Tiles on the top row (row n_rows-1). Their O face is the global outer boundary."""
        return [self.tiles[col][self.n_rows - 1] for col in range(self.n_cols)]

    # -- Stats ------------------------------------------------------------

    def total_groups(self) -> int:
        """Total number of per-tile groups across all tiles."""
        return sum(t.result.group_count for t in self.all_tiles())

    def total_alive(self) -> int:
        """Total alive points across all tiles (includes double-counted shared boundaries)."""
        return sum(t.result.alive_count for t in self.all_tiles())

    def __repr__(self) -> str:
        return (f"Grid(cols={self.n_cols}, rows={self.n_rows}, "
                f"tiles={self.n_cols * self.n_rows})")
