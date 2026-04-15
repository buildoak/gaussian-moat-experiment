"""
compositor.py — Tile Group Compositor

Merges per-tile connected components into a global connectivity picture
using union-find over tile group labels. This is the "composition" step
that reconstructs global connectivity from local tile data + boundary ports.

THE KEY INSIGHT:
Each tile independently discovers its internal connected components (groups)
and which of those groups touch each face (ports). Two adjacent tiles share
a boundary — if both tiles have a port at the same position on that shared
boundary, the ports' groups must be in the same global component.

COLLAR OVERLAP MERGING:
When a collar > 0 is used, adjacent tiles' sieve domains overlap. A point
in the overlap zone appears in BOTH tiles' point_group maps, potentially
assigned to different local group IDs. The compositor must also merge groups
that share ANY point in the overlap zone — not just ports on the nominal
boundary. This is essential when r > sqrt(2) and edges bridge across the
nominal boundary through the collar zone without touching any port position.

PORT MATCHING RULES:
- Vertical (O/I): tile_below.O ports matched with tile_above.I ports by position.
  Position = x offset along the shared row. Same position = same lattice point.
- Horizontal (R/L): tile_left.R ports matched with tile_right.L ports by position.
  Position = y offset along the shared column. Same position = same lattice point.

VERDICT:
After all merges, check if any group in a bottom-row tile (that has points on
the tile's bottom row) shares a root with any group in a top-row tile (that has
points on the tile's top row). If yes → SPANNING. If no → MOAT.
"""

from __future__ import annotations

from dataclasses import dataclass

from grid import Grid
from tile import Face, Port, Tile
from uf import UnionFind


@dataclass
class CompositorResult:
    """Output of the composition step."""
    verdict: str           # "SPANNING" or "MOAT"
    total_groups: int      # number of per-tile groups before merging
    merge_count: int       # number of successful union operations
    global_components: int # distinct components after all merges


class Compositor:
    """Merges tile groups across shared faces via union-find.

    Operates on a Grid of already-analyzed tiles. Creates a global union-find
    where each element is a (tile_id, group_id) pair, then unites elements
    across matching face ports.
    """

    def __init__(self, grid: Grid) -> None:
        self.grid = grid
        # Populated by compose(), available for component-equivalence tracing.
        self.uf: UnionFind | None = None
        self.global_idx: dict[tuple[int, int, int], int] = {}

    def compose(self) -> CompositorResult:
        """Run the full composition pipeline.

        Steps:
        1. Assign a global index to every (tile, group) pair.
        2. Build a global union-find over these indices.
        3. For each vertical neighbor pair: match O/I ports, unite groups.
        4. For each horizontal neighbor pair: match R/L ports, unite groups.
        5. Check if any inner-boundary group root == any outer-boundary group root.

        Returns:
            CompositorResult with verdict and diagnostics.
        """
        grid = self.grid

        # Step 1: assign global indices.
        # Each tile's groups are numbered 1..N locally. We need a flat global
        # namespace for the union-find.
        # global_idx[(col, row, group_id)] → UF index
        global_idx: dict[tuple[int, int, int], int] = {}
        idx_counter = 0

        for tile in grid.all_tiles():
            result = tile.result
            for gid in range(1, result.group_count + 1):
                global_idx[(tile.col, tile.row, gid)] = idx_counter
                idx_counter += 1

        total_groups = idx_counter
        if total_groups == 0:
            # Degenerate: no alive points anywhere.
            return CompositorResult(
                verdict="MOAT", total_groups=0,
                merge_count=0, global_components=0,
            )

        # Expose global_idx for component tracing.
        self.global_idx = global_idx

        # Step 2: build global UF.
        uf = UnionFind(total_groups)
        self.uf = uf  # Expose for component tracing.
        merge_count = 0

        # Step 3: vertical merges (O/I face matching).
        # For each pair (lower, upper), lower.O ports ↔ upper.I ports.
        # Ports match when they share the same position (= same x offset
        # on the shared boundary row).
        for lower, upper in grid.vertical_pairs():
            merge_count += self._match_faces(
                uf, global_idx,
                lower, Face.O,
                upper, Face.I,
            )

        # Step 4: horizontal merges (R/L face matching).
        # For each pair (left, right), left.R ports ↔ right.L ports.
        # Ports match when they share the same position (= same y offset
        # on the shared boundary column).
        for left, right in grid.horizontal_pairs():
            merge_count += self._match_faces(
                uf, global_idx,
                left, Face.R,
                right, Face.L,
            )

        # Step 4b: collar-overlap merges.
        # When collar > 0, adjacent tiles' sieve domains overlap beyond the
        # shared boundary.  Any point that appears in BOTH tiles' point_group
        # maps implies their respective groups must be merged.  This captures
        # connectivity that routes through the collar zone without touching
        # the nominal boundary (and thus has no port to match).
        #
        # We iterate all tile pairs (vertical + horizontal) and compare
        # point_group entries in the overlap region.
        for tile_a, tile_b in grid.vertical_pairs():
            merge_count += self._merge_overlap(
                uf, global_idx, tile_a, tile_b,
            )
        for tile_a, tile_b in grid.horizontal_pairs():
            merge_count += self._merge_overlap(
                uf, global_idx, tile_a, tile_b,
            )
        for tile_a, tile_b in grid.diagonal_pairs():
            merge_count += self._merge_overlap(
                uf, global_idx, tile_a, tile_b,
            )

        # Step 5: spanning check.
        # Collect all group roots from inner boundary (bottom-row tiles, groups
        # on the tile's bottom row) and outer boundary (top-row tiles, groups
        # on the tile's top row).
        #
        # We use the pre-computed bottom_groups/top_groups from TileResult,
        # which records ALL groups that have at least one point on the
        # respective boundary row — not just groups with face ports.
        # This is critical: a group touching the global boundary might not
        # have cross-tile edges (e.g., an isolated boundary point).

        inner_roots: set[int] = set()
        for tile in grid.inner_boundary_tiles():
            result = tile.result
            for gid in result.bottom_groups:
                key = (tile.col, tile.row, gid)
                if key in global_idx:
                    inner_roots.add(uf.find(global_idx[key]))

        outer_roots: set[int] = set()
        for tile in grid.outer_boundary_tiles():
            result = tile.result
            for gid in result.top_groups:
                key = (tile.col, tile.row, gid)
                if key in global_idx:
                    outer_roots.add(uf.find(global_idx[key]))

        # SPANNING iff any inner root is also an outer root.
        spanning = bool(inner_roots & outer_roots)

        return CompositorResult(
            verdict="SPANNING" if spanning else "MOAT",
            total_groups=total_groups,
            merge_count=merge_count,
            global_components=uf.component_count(),
        )

    # -- Internal helpers -------------------------------------------------

    def _merge_overlap(
        self,
        uf: UnionFind,
        global_idx: dict[tuple[int, int, int], int],
        tile_a: Tile,
        tile_b: Tile,
    ) -> int:
        """Merge groups from two adjacent tiles that share any point
        in their overlapping sieve domains.

        For each point that appears in BOTH tiles' point_group maps,
        the point's group in tile_a must be in the same global component
        as its group in tile_b.

        Returns the number of successful (non-redundant) union operations.
        """
        # Quick skip: if neither tile has a collar, the only overlap is
        # the shared boundary — already handled by port matching.
        if tile_a.collar == 0 and tile_b.collar == 0:
            return 0

        # Compute the overlap rectangle of the two sieve domains.
        ox_lo = max(tile_a.sx_lo, tile_b.sx_lo)
        ox_hi = min(tile_a.sx_hi, tile_b.sx_hi)
        oy_lo = max(tile_a.sy_lo, tile_b.sy_lo)
        oy_hi = min(tile_a.sy_hi, tile_b.sy_hi)

        if ox_lo > ox_hi or oy_lo > oy_hi:
            return 0

        pg_a = tile_a.point_group
        pg_b = tile_b.point_group

        merges = 0
        for y in range(oy_lo, oy_hi + 1):
            for x in range(ox_lo, ox_hi + 1):
                pt = (x, y)
                gid_a = pg_a.get(pt)
                gid_b = pg_b.get(pt)
                if gid_a is not None and gid_b is not None:
                    idx_a = global_idx[(tile_a.col, tile_a.row, gid_a)]
                    idx_b = global_idx[(tile_b.col, tile_b.row, gid_b)]
                    if uf.union(idx_a, idx_b):
                        merges += 1
        return merges

    def _match_faces(
        self,
        uf: UnionFind,
        global_idx: dict[tuple[int, int, int], int],
        tile_a: Tile, face_a: Face,
        tile_b: Tile, face_b: Face,
    ) -> int:
        """Match ports on two adjacent faces and unite their groups.

        Two ports match if they have the same position along their respective
        faces — which means they represent the same lattice point on the
        shared boundary.

        Returns the number of successful (non-redundant) union operations.
        """
        result_a = tile_a.result
        result_b = tile_b.result

        # Index face_b ports by position for O(1) lookup.
        b_ports_by_pos: dict[int, list[Port]] = {}
        for port in result_b.face_ports[face_b]:
            b_ports_by_pos.setdefault(port.position, []).append(port)

        merges = 0
        for port_a in result_a.face_ports[face_a]:
            matching = b_ports_by_pos.get(port_a.position, [])
            for port_b in matching:
                idx_a = global_idx[(tile_a.col, tile_a.row, port_a.group_id)]
                idx_b = global_idx[(tile_b.col, tile_b.row, port_b.group_id)]
                if uf.union(idx_a, idx_b):
                    merges += 1
        return merges
