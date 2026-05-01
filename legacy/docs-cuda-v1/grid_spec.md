---
title: "Grid Specification v6 — Tower Geometry and Compositor"
date: 2026-04-13
version: 6
status: draft
depends: tile_spec.md (v5)
---

# Grid Specification v6 — Tower Geometry and Compositor

## S1. Overview

This specification defines how TileOps (tile_spec.md) compose at scale to
answer the Gaussian moat question: does there exist a connected path of
Gaussian primes, with step distance at most sqrt(k), from the origin to
infinity?

The search operates on annular bands of the Gaussian integer lattice. Each
band is tiled by a rectangular array of **towers** — vertical columns of
variable height (32--46 tiles) — that follow the inner arc of the annulus. A C++ **compositor**
wires TileOps together via union-find and checks whether any connected
component of boundary ports spans from the inner edge to the outer edge of
the annulus. If no component spans: the annulus is a moat at that radius.

Key properties:

- **Pure rectangular array** — no irregular tiles, no gap-filling, no special
  zones. Tower height varies (32--46 tiles) for consistent radial coverage.
- **O(1) neighbor lookup** — within a tower by row arithmetic, between towers
  by a precomputed delta table.
- **Two matching modes** — I/O faces use the positional shortcut (groups
  only); L/R faces use decoded h1 equality with a per-tower-pair delta_h.
- **8-fold symmetry** — only one octant is tiled. Octant stitching at the
  y = x diagonal completes the annulus.
- **Memory-bounded** — 10.85 GB TileOps + ~2.04 GB group-level UF metadata at
  R = 830M (~12.9 GB total). Fits a single 80 GB GPU with room for working
  memory.

### S1.1 Relationship to tile_spec.md

The tile_spec defines the TileOp format, CUDA kernel pipeline, and matching
predicates. This grid_spec defines:

1. How tiles are placed in the lattice (tower geometry).
2. How neighbor relationships are determined (delta table).
3. How the compositor wires TileOps and checks spanning (4-phase algorithm).
4. How octant symmetry is exploited (boundary stitching).
5. Memory layout and budget.

The grid_spec does NOT modify the TileOp format. It consumes TileOps exactly
as tile_spec.md v5 defines them: 128 bytes, with a 3-byte offset header and
dynamic packed face sections. Tile status is derived from that header
(tile_spec S4.3).

---

## S2. Constants

All constants from tile_spec.md S11 apply. Grid-level additions:

| Name | Value | Derivation |
|------|-------|------------|
| S | 256 | Tile side (lattice units). From tile_spec. |
| TILES_PER_TOWER_MIN | 32 | At Y-axis, where radial direction = vertical. |
| TILES_PER_TOWER_MAX | 46 | At 45°, where ceil(32 × sqrt(2)) = 46. |
| W_RADIAL | 8192 | Guaranteed minimum radial depth at every angle = 32 * S. |
| K | 40 | Step distance squared. From tile_spec. |
| COLLAR | 7 | ceil(sqrt(K)). From tile_spec. |
| MIN_OVERLAP | COLLAR | Minimum L/R face overlap for any port to exist in the shared zone (S5.3). |

**Variable tower height.** `TILES_PER_TOWER` is no longer a single constant.
Each tower j has `tiles_per_tower[j]` tiles, computed from the angle between
the radial direction and the vertical at that tower's position (S4.2a).
The per-tower vertical height is `W_j = tiles_per_tower[j] * S`, which
ranges from `TILES_PER_TOWER_MIN * S = 8192` (at the Y-axis) to
`TILES_PER_TOWER_MAX * S = 11776` (at 45°).

**Motivation.** Towers are vertical stacks, but the radial direction tilts
away from vertical as the angle approaches 45°. With a fixed height of 32
tiles, the effective radial coverage at 45° is only 32/sqrt(2) ~ 22.6 tiles
instead of 32. This shortfall can cause false SPANNING: the outer boundary
at high angles falls below a real moat, creating a spurious inner-outer
connection. Variable tower height guarantees W_RADIAL = 8192 lattice units
of radial coverage at every angle.

---

## S3. Annulus and Coordinate System

### S3.1 The Search Annulus

Fix a search radius R (a positive integer, typically 826M -- 870M). The
annulus is the set of lattice points (x, y) with:

```
R^2 <= x^2 + y^2 <= (R + W)^2
```

where W = W_RADIAL = 8192 is the minimum radial depth. By 8-fold Gaussian integer symmetry
(the units {1, -1, i, -i} and the conjugation map z -> z*), it suffices to
search one octant and stitch at the boundary.

### S3.2 The Working Octant

We tile the first-octant wedge: {(x, y) : x >= 0, y >= x}. In this wedge:

- The y-axis (x = 0) is one boundary.
- The line y = x is the other boundary.
- The inner arc is the circle of radius R.
- The outer arc is the circle of radius R + W.

Tiles are axis-aligned squares with sides parallel to the x and y axes.

### S3.3 Coordinate Convention

The lattice x-axis runs rightward (increasing tower index). The lattice
y-axis runs upward (increasing row index within a tower = radially outward
from the origin). This matches the tile_spec convention where Face I is the
bottom edge and Face O is the top edge.

- **Tower index j** increases with x: tower j has base_x = j * S.
- **Row index r** increases with y (outward): row 0 is the innermost tile
  of the tower, row `tiles_per_tower[j]-1` is the outermost.

---

## S4. Tower Construction

### S4.1 Tower Definition

A **tower** is a column of `tiles_per_tower[j]` tiles stacked vertically
(32 at the Y-axis, ramping to 46 at 45°). Tower j is defined by:

```
base_x[j] = j * S
base_y[j] = computed from the inner arc (S4.2)
```

Tile (j, r) occupies the axis-aligned square of lattice points:

```
x in [base_x[j],  base_x[j] + S]
y in [base_y[j] + r*S,  base_y[j] + (r+1)*S]
```

This is a 257x257 lattice-point domain. Adjacent tiles share their boundary
row/column (the last row of one tile is the first row of the next), which is
the shared collar that makes I/O and L/R matching possible.

The origin corner of tile (j, r) is (base_x[j], base_y[j] + r * S),
consistent with tile_spec S2.1.

### S4.2 Inner Arc Placement

Tower j's base_y is the integer y-coordinate where the tower's bottom edge
meets the inner arc of the annulus. The continuous value is:

```
y_cont[j] = sqrt(R^2 - (j * S)^2)
```

This is the inner-arc y-coordinate at x = j * S. The tower's bottom edge is
placed at an integer approximation of this value.

**Threshold rounding.** Each y_cont[j] is rounded to the nearest integer,
then clamped to enforce monotonicity (base_y[j] <= base_y[j-1]):

```
function compute_base_y(R, S):
    base_y = []
    prev_y = 0
    for j = 0, 1, 2, ...:
        x_j = j * S
        if x_j^2 > R^2:
            break       // tower base is beyond the y-axis intercept
        y_cont = sqrt(R^2 - x_j^2)
        y_j = round(y_cont)
        if j > 0 and y_j > prev_y:
            y_j = prev_y    // monotonicity clamp
        prev_y = y_j
        base_y.push(y_j)
    return base_y
```

**Invariant:** |base_y[j] - y_cont[j]| <= 0.5 for all j. This holds because
round() gives at most 0.5 deviation, and the min-clamp only moves base_y[j]
downward (closer to the arc, since y_cont is non-increasing along the octant).
Monotonicity is structural — no accumulated error state is needed.

**Dual construction modes.** Two grid construction functions exist:

1. `compute_grid(R)` — production mode. Uses threshold rounding
   as described above. Used by the campaign runner when generating new tiles.
2. `compute_grid_from_coords(R, coords, n_tiles)` — extraction mode. Reads
   `base_y` directly from the first tile of each tower in pre-existing tile
   data (using `b_lo` from the tile coordinates). Used when compositing tiles
   generated by external tools (e.g., the CUDA pipeline via `gen_coords.py`).
   These two modes produce DIFFERENT `base_y` values because the external
   tool uses `isqrt(R^2 - x_j^2)` per-tower, while `compute_grid` uses
   threshold rounding with monotonicity clamp. The compositor must use the
   same grid that generated the tiles.

**Integer arithmetic note.** In the CUDA/Rust implementation, R^2 and x_j^2
are computed in u128 or i128 to avoid overflow at R ~ 870M (R^2 ~ 7.6e17,
which exceeds u32 and i64 ranges). The sqrt is computed via integer Newton's
method or via f64 with a +-1 correction step.

### S4.2a Per-Tower Height Computation

Each tower gets enough tiles to guarantee W_RADIAL = 8192 lattice units of
radial coverage. At tower j, the angle theta_j between the radial direction
and the vertical satisfies:

```
cos_theta_j = base_y[j] / sqrt((j*S)^2 + base_y[j]^2)
tiles_per_tower[j] = ceil(32 / cos_theta_j)
```

Near the Y-axis (j ~ 0), cos_theta_j ~ 1, so tiles_per_tower[j] = 32.
At the 45-degree diagonal (j*S ~ base_y[j]), cos_theta_j ~ 1/sqrt(2),
so tiles_per_tower[j] = ceil(32 * sqrt(2)) = 46.

**Gentle ramp property.** Between adjacent towers, the height changes by at
most 1 tile. This follows from the smoothness of the arc: the change in
cos_theta between adjacent towers at spacing S = 256 is small relative to
the quantization step (1/32). Formally,
`|tiles_per_tower[j] - tiles_per_tower[j+1]| <= 1` for all j.

**Extra tiles at the top.** The base of every tower stays on the inner arc
(base_y[j] is unchanged). The additional tiles are appended at the top,
extending the tower's outer reach. This preserves all inner-arc geometry
and the inner staircase structure.

**Storage.** `tiles_per_tower[]` is stored as grid metadata alongside
`base_y[]` and `delta[]`. It is a per-tower u8 array (1 byte per tower,
~2.3 MB at R = 830M).

### S4.3 Tower Count and Termination

Towers march rightward from j = 0 (at the y-axis) following the inner arc
downward. The octant boundary is the line y = x. We continue placing towers
**past** y = x until the last tower's top edge is fully below the diagonal:

```
Termination predicate: stop adding towers when
    base_y[j] + tiles_per_tower[j] * S + MARGIN <= j * S
```

where MARGIN = 2 * S = 512. That is, tower j is included only if its
topmost tile's top edge plus MARGIN is strictly above the line y = x
(which at x = j*S has y = j*S). The first tower that fails this test is
NOT included. The MARGIN guarantees sufficient sub-diagonal coverage for
cross-diagonal connectivity (S7.1, S7.4).

Let J denote the number of towers (j ranges from 0 to J-1).

**Justification.** Tower generation extends past y = x until the
termination predicate (S7.1) is satisfied. Sub-diagonal tiles in extended
towers provide second-octant coverage for cross-diagonal connectivity —
no separate infill stage is needed (S8.2).

### S4.4 Tower Geometry at Search Radius

At R = 830M:

```
J ≈ R / (sqrt(2) * S) + O(1) ≈ 830e6 / (1.414 * 256) ≈ 2,293,126 towers
```

The "+O(1)" accounts for the overshoot past y = x. Total tiles (variable
tower height; average ~37 tiles/tower across the octant):

```
N = sum(tiles_per_tower[j] for j in 0..J) ≈ 84.8M tiles
```

---

## S5. Inter-Tower Geometry

### S5.1 The Delta Table

For each adjacent tower pair (j, j+1), define:

```
delta[j] = base_y[j] - base_y[j+1]
```

This is the **downward vertical offset** of tower j+1 relative to tower j,
measured in lattice units. Since the inner arc curves downward as x increases
(in the first octant), delta[j] >= 0 for all j.

**Properties of delta[j]:**

- Near the y-axis (j ~ 0): the arc is nearly horizontal, so delta[j] ~ 0.
- At the 45-degree line (j*S ~ R/sqrt(2)): the arc descends steeply,
  delta[j] ~ S = 256.
- delta[j] is monotonically non-decreasing in j (the arc's slope steepens).
- Exact formula (continuous approximation):

```
delta_cont[j] ≈ S^2 * j / sqrt(R^2 - (j*S)^2)
```

This is the discrete derivative of -y_cont[j] with respect to j, times S.

The delta table is precomputed once per band and stored as an array of J-1
integers. At J ~ 2.3M, this is ~9 MB (u32 per entry) — negligible.

### S5.2 I/O Face Alignment (Within Tower)

Tiles (j, r) and (j, r+1) in the same tower share an exact horizontal face.
The shared face is at y = base_y[j] + (r+1)*S. Both tiles have identical
origin-corner x-coordinates (base_x[j]) and their shared face spans the same
256 lattice units along x.

**Therefore delta_h = 0 for all I/O face pairs.**

By tile_spec S5.2, when delta_h = 0 the positional shortcut applies:
port[i] on tile A's O-face corresponds to port[i] on tile B's I-face. The
compositor reads only the packed group sections, not any decoded h1 values.

### S5.3 L/R Face Geometry (Between Towers)

Tiles in tower j and tower j+1 are vertically offset by delta[j] lattice
units. Their shared vertical faces (R-face of tower j, L-face of tower j+1)
are NOT aligned — they overlap partially.

**Overlap geometry for tile (j, r).**

Let d = delta[j]. Tile (j, r) has its R-face spanning the y-interval:

```
[base_y[j] + r*S,  base_y[j] + (r+1)*S)
```

Tower j+1's tiles have their L-faces at y-intervals:

```
tile (j+1, r'): [base_y[j+1] + r'*S,  base_y[j+1] + (r'+1)*S)
```

Since base_y[j+1] = base_y[j] - d, tile (j+1, r') spans:

```
[base_y[j] - d + r'*S,  base_y[j] - d + (r'+1)*S)
```

**Note on interval convention.** The half-open intervals above describe
face *segments* (continuous spans along the y-axis) used to compute
overlap lengths. This is distinct from the closed lattice-point domain
in S4.1: a tile's sieve domain is [lo, lo+S] (257 lattice points, shared
boundaries), while the face segment [lo, lo+S) has length S for clean
overlap arithmetic. Both conventions are correct in their respective
contexts.

**Decompose d into tile-rows and fractional offset:**

```
q = d div S       // number of full tile-rows of offset
f = d mod S       // fractional offset within a tile (0 <= f < S)
```

(Here "div" is integer floor division and "mod" is the non-negative
remainder.)

Tile (j, r)'s R-face interval, in tower-j-local coordinates starting from
base_y[j], is [r*S, (r+1)*S). In tower j+1's local coordinates (starting
from base_y[j+1] = base_y[j] - d), this becomes [(r+q)*S + f, (r+q+1)*S + f).

**Overlapping tiles in tower j+1.** The interval [(r+q)*S + f, (r+q+1)*S + f)
in tower j+1's local coordinates intersects at most two tile-rows:

- **Primary neighbor:** r' = r + q. Overlap interval in tower j+1 local
  coords: [r'*S, r'*S + (S - f)) intersected with the R-face interval. The
  overlap length is S - f lattice units (the lower portion of the R-face).
- **Secondary neighbor:** r' = r + q + 1. Overlap interval:
  [(r'+1)*S - f, (r'+1)*S). The overlap length is f lattice units (the upper
  portion of the R-face).

When f = 0, only the primary neighbor exists (overlap = S, full alignment).
When f > 0, both neighbors exist, with overlap S - f and f respectively.

**Existence check.** A neighbor (j+1, r') is valid only if
0 <= r' < tiles_per_tower[j+1]. If r + q >= tiles_per_tower[j+1] or
r + q < 0, there is no primary neighbor (the R-face of tile (j, r) extends
beyond tower j+1's vertical extent). Similarly for r + q + 1.

### S5.4 Delta_h for L/R Matching

The matching predicate (tile_spec S5.1) requires delta_h: the signed offset
between tile origins projected onto the shared face axis. For vertical L/R
faces, the shared face axis is the y-axis.

For tile (j, r) matched against tile (j+1, r') on their shared vertical
face:

```
delta_h = origin_y(j+1, r') - origin_y(j, r)
        = (base_y[j+1] + r'*S) - (base_y[j] + r*S)
        = -d + (r' - r)*S
```

where d = delta[j].

**For the primary neighbor** (r' = r + q):

```
delta_h_primary = -d + q*S = -(d - q*S) = -f
```

**For the secondary neighbor** (r' = r + q + 1):

```
delta_h_secondary = -d + (q+1)*S = S - f
```

**Matching rule** (tile_spec S5.1): port a on tile A's R-face matches port b
on tile B's L-face iff (all arithmetic in signed i16):

```
(a.h1 as i16) == (b.h1 as i16) + delta_h    // delta_h: i16
```

The compositor decodes raw h1 locally from the stored L/R bytes:
`group_id = group_byte & 0x7F` and
`h1 = ((group_byte >> 7) << 8) | h1_byte`.
This is exact because `h1` ranges only over tile-proper rows `0..256`.

For the primary neighbor (delta_h = -f): a.h1 + f == b.h1.
For the secondary neighbor (delta_h = S - f): a.h1 - (S - f) == b.h1,
i.e., a.h1 + f == b.h1 + S.

**Validity of matched ports.** A match is geometrically valid only if the
matched port lies in the overlap zone. For the primary neighbor, the overlap
spans the lower S - f units of the R-face, so valid ports have h1 < S - f.
For the secondary neighbor, valid ports have h1 >= S - f (equivalently,
h1 is in [S - f, S] under the shared-boundary convention).

Since h1 is the anchor of the port's minimum-offset prime, and ports are
deterministic functions of the face primes, the h1 predicate automatically
enforces this: a port outside the overlap zone has no corresponding primes
on the other tile's face, so no match exists.

### S5.5 Minimum Overlap Threshold

When the overlap between two tiles is very small, no port can exist in the
shared zone. A port on a face requires at least one prime within COLLAR = 7
lattice units of the face boundary. For two tiles sharing a vertical face
with overlap v lattice units, a prime in the overlap zone that is a port
anchor must be within v units of the overlap boundary.

A port's primes lie within COLLAR perpendicular depth from the face.
Along the face axis, the port's anchor is at h1 >= 0. For the port to lie
entirely within the overlap zone, we need the overlap v to be at least 1
(a single prime suffices for a port). However, the parity constraint
(tile_spec S3.1) means the minimum nonzero h1 is 0 or 1 depending on
parity. A port can exist at the very edge of the overlap.

**Conservative skip rule:** the compositor MAY skip a neighbor when the
overlap is 0 (f = 0 for the secondary neighbor, or f = S for the primary —
but f < S always, so f = 0 means secondary overlap = 0). When f = 0, only
the primary neighbor exists with full overlap S. This is correct by
construction.

**No further skipping is safe.** Even an overlap of 1 lattice unit could
contain a prime. The compositor must check all neighbors with overlap > 0.
Skipping based on overlap < COLLAR would risk missing a port whose anchor
prime falls in the narrow overlap — the unsafe direction (false moat).

### S5.6 Outer Staircase (Variable Tower Heights)

Variable tower heights create a staircase pattern at the **outer** boundary,
analogous to the inner staircase caused by base_y differences. When tower j+1
is taller than tower j by 1 tile, the topmost tile of tower j+1 has its
L-face exposed (tower j does not reach this row) — this is an outer boundary
face.

Define per-tower height deltas:

```
height_delta_left[j]  = tiles_per_tower[j] - tiles_per_tower[j-1]
height_delta_right[j] = tiles_per_tower[j] - tiles_per_tower[j+1]
```

When `height_delta_left[j] > 0`, the top `height_delta_left[j]` tiles of
tower j have their L-face exposed (the left neighbor tower is shorter and
does not reach these rows). These exposed L-faces are outer boundary faces.

When `height_delta_right[j] > 0`, the top `height_delta_right[j]` tiles of
tower j have their R-face exposed (the right neighbor tower is shorter).
These exposed R-faces are outer boundary faces.

A "bump" tower — one taller than both neighbors — has both L-face and R-face
exposed on its topmost tile(s).

**Boundary implications.** The spanning check (S9.7) must include these
staircase-exposed faces when identifying outer boundary ports, in addition to
the O-face of each tower's topmost tile.

### S5.7 Summary: L/R Neighbor Table

For tile (j, r) with d = delta[j], q = d div S, f = d mod S:

| Neighbor | Row in tower j+1 | Overlap (lattice units) | delta_h | Valid if |
|----------|-------------------|-------------------------|---------|----------|
| Primary | r + q | S - f | -f | 0 <= r + q < tiles_per_tower[j+1] |
| Secondary | r + q + 1 | f | S - f | f > 0 AND 0 <= r + q + 1 < tiles_per_tower[j+1] |

The symmetric case (L-face of tile (j, r) matched against R-faces in tower
j-1) uses delta[j-1] with the roles reversed.

---

## S6. Tile Addressing

### S6.1 Composite Tile Address

Each tile is addressed by a composite `(tower_id: u32, tile_pos: u8)`:

```
tower_id: u32   — tower index j (0 to J-1). At R = 830M, J ~ 2.29M;
                  at R = 2B, J ~ 5.5M. Fits u32 comfortably.
tile_pos: u8    — row within tower (0 to tiles_per_tower[j]-1). Max 45.
```

Total tiles: N = sum(tiles_per_tower[j]), where J is the tower count.

**Why not a flat u32 index.** A flat index fits u32 at ~84.8M tiles
(< 4.29B). However, port addressing in the
naive UF scheme requires `tile_index * 64` which reaches 4.7B — overflowing
u32. The composite address avoids this: port addresses use the composite
tile address directly (see S9.2), never requiring a global flat tile index
multiplied by a slot count.

**Flat index for array layout.** TileOps are still stored contiguously in
tower-major order: `tile_ops[tile_offset[j] + r]` where `tile_offset[j]`
is the prefix-sum of `tiles_per_tower` (S10.6). The flat index is used ONLY
for array addressing (always within u32 range at ~84.8M tiles), never for
port identity computation.

### S6.2 Inverse Addressing

With variable tower heights, flat-to-composite mapping requires the
`tile_offset[]` prefix-sum array (S10.6). From a flat array index:

```
tower_id(flat_index) = binary_search(tile_offset, flat_index)
tile_pos(flat_index) = flat_index - tile_offset[tower_id]
```

### S6.3 Absolute Lattice Coordinates

```
tile_x(j, r) = j * S
tile_y(j, r) = base_y[j] + r * S
```

### S6.4 Neighbor Lookup

| Neighbor | Address (tower_id, tile_pos) | Condition |
|----------|------------------------------|-----------|
| I (inner) | (j, r-1) | r > 0 |
| O (outer) | (j, r+1) | r < tiles_per_tower[j]-1 |
| R primary | (j+1, r + q) | j < J-1, 0 <= r+q < tiles_per_tower[j+1] |
| R secondary | (j+1, r + q + 1) | j < J-1, f > 0, 0 <= r+q+1 < tiles_per_tower[j+1] |
| L primary | (j-1, r - q') | j > 0, 0 <= r-q' < tiles_per_tower[j-1] |
| L secondary | (j-1, r - q' - 1) | j > 0, f' > 0, 0 <= r-q'-1 < tiles_per_tower[j-1] |

Where q = delta[j] div S, f = delta[j] mod S for R-neighbors, and
q' = delta[j-1] div S, f' = delta[j-1] mod S for L-neighbors.

All lookups are O(1): one integer division, one modulo, one addition.

### S6.5 Port Address

A port is addressed by `(tower_id: u32, tile_pos: u8, face: u2, slot: u4)`:

```
tower_id: u32   — which tower
tile_pos: u8    — which tile within tower (0 to tiles_per_tower[j]-1, max 45)
face:     u2    — which face (0=I, 1=O, 2=L, 3=R)
slot:     u4    — which port slot on that face (0-15)
```

Packed representation: 8 bytes (u32 + u8 + u8 packed with face:2 + slot:4
+ 2 spare bits). This replaces the flat `global_id = t * 64 + f * 16 + s`
which overflows u32 at ~84.8M tiles.

---

## S7. Tower Termination and Sub-Diagonal Tiles

### S7.1 Tower Termination Predicate

Tower generation extends past the y = x diagonal. A tower j is
**terminated** when its highest tile (row tiles_per_tower[j]-1) is fully
submerged below y = x by at least MARGIN lattice units:

```
terminated(j) := base_y[j] + tiles_per_tower[j] * S + MARGIN <= j * S
```

where MARGIN = 2 * S = 512. Tile generation stops at the first terminated
tower. All towers with index < j are generated; all tiles_per_tower[j]
tiles in each generated tower are emitted to CUDA.

### S7.2 Sub-Diagonal Tiles

Tiles whose area is partially or entirely below y = x are called
**sub-diagonal tiles**. They are NOT filtered — they are processed normally
by CUDA. Their sieve domain includes second-octant primes, which provide
cross-diagonal connectivity via standard face matching with adjacent
towers. This is deliberate and necessary for correctness (S8.2).

### S7.3 Empty Tiles

A tile with zero collar-zone primes (possible but rare) emits the empty
TileOp sentinel: `off_I = off_L = off_R = 3`, zero payload. This is a
**zero-prime condition**, not a diagonal condition — it can occur anywhere
in the grid. The compositor skips empty tiles in all phases.

### S7.4 Coverage Guarantee

At M = 2, the extended towers provide at least 512 lattice units of
second-octant coverage past the diagonal. Since COLLAR <= 7 for all
supported K_SQ values, any multi-step cross-diagonal path within the
annulus is captured by sub-diagonal tiles in adjacent towers — no C++
infill band is needed.

---

## S8. Octant Boundary — Extended Tower Generation

### S8.1 The Problem

We tile the octant {y >= x >= 0} with CUDA at high throughput. A connected
path of Gaussian primes may cross the y = x line, entering the adjacent
octant {x >= y >= 0}. If we compose only within our octant, we miss such
cross-octant paths and may report a false moat.

### S8.2 Architectural Decision: Extended Tower Generation

**Approach:** Tower generation continues past y = x until the termination
predicate (S7.1) is satisfied. All tiles in generated towers — including
sub-diagonal tiles — are processed by CUDA at full throughput.
Cross-diagonal connectivity falls out from standard L/R face matching
between adjacent towers that straddle y = x.

**Why this works:** Adjacent towers on both sides of y = x have standard
L/R face adjacency. The compositor's delta/q/f matching handles the
curvature offset regardless of the diagonal. No special stitching, no
diagonal buffers, no auxiliary tile sources. Sub-diagonal tiles sieve
second-octant primes that participate in standard face matching with their
neighbors — cross-diagonal paths are captured by the same composition
pipeline that handles all other inter-tile connections. UF transitivity
handles multi-hop paths that zigzag across the diagonal.

**Cost:** At R = 830M, ~10-50 extra towers past the diagonal before
termination. A few hundred to ~1600 extra tiles at ~155K tiles/s on
4090 — microseconds of GPU time. The overhead is negligible compared to
the full octant tiling.

**Key property:** No special tile format, no auxiliary buffers, no K5
modifications, no separate tile source. The compositor is unaware of the
diagonal — it composes all tiles uniformly.

### S8.3 Why C++ Infill Is Superseded

The previous approach (grid_spec v3 S8.3) used a separate C++ reference
tiler to fill a narrow band in the second octant. This worked but added:

- A separate pipeline stage (C++ at ~1K tiles/s vs CUDA at ~155K tiles/s).
- A separate tile source that the campaign runner must merge.
- Additional complexity in the compositor's input handling.

Extended tower generation achieves the same coverage using the existing
CUDA pipeline with no modifications to kernels, compositor, or TileOp
format. All cross-diagonal connectivity is handled by standard face
matching on sub-diagonal tiles that CUDA produces natively.

### S8.4 Historical Note

Previous versions of this spec explored alternative cross-diagonal
strategies:

- **v2, S8.2-S8.3:** One-octant approaches — an unproven single-octant
  symmetry shortcut and a face-based boundary stitching protocol.
- **v3, S8.3:** C++ diagonal infill — a separate C++ reference tiler
  filling a narrow second-octant band alongside CUDA tiles.

Both are superseded by extended tower generation (S8.2), which is simpler,
provably correct, and requires zero changes to the CUDA pipeline or
compositor algorithm. Analysis of the abandoned v2 approaches is preserved
in:
- `docs/supportive/2026-04-11-octant-stitching-codex-hypothesis.md`

---

## S9. Compositor Algorithm

### S9.1 Overview

The compositor receives:

- `tile_ops[0..N)`: array of N TileOps (128 bytes each, tile_spec v5 layout)
- `base_y[0..J)`: tower base y-coordinates
- `delta[0..J-1)`: tower delta table

It produces a boolean: **spanning** (path exists from inner to outer boundary)
or **moat** (no such path exists).

**Note:** the old port-level compositor in
`tile-probe/crates/moat-kernel/src/compose.rs` (operating on `FacePort`
structs with O(n^2) distance matching) is superseded by this spec and by
compositor_spec.md v8. Do not reference compose.rs for the production
compositor.

### S9.2 TileOp v2 Access Model

For each non-overflow TileOp, the compositor reads the three-byte header:

```
off_I = tile[0]
off_L = tile[1]
off_R = tile[2]
```

and derives:

```
o_cnt = off_I - 3
i_cnt = off_L - off_I
l_cnt = off_R - off_L
r_cnt = (125 - o_cnt - i_cnt - 2*l_cnt) >> 1
h_start = off_R + r_cnt
```

Face sections:

```
O groups = tile[3 .. off_I]
I groups = tile[off_I .. off_L]
L groups = tile[off_L .. off_R]
R groups = tile[off_R .. off_R + r_cnt]
L h1     = tile[h_start .. h_start + l_cnt]
R h1     = tile[h_start + l_cnt .. h_start + l_cnt + r_cnt]
```

Empty tile (zero primes):

```
tile[0] == 3 && tile[1] == 3 && tile[2] == 3 && tile[3] == 0
```

Overflow tile:

```
tile[0] == 0xFF
```

### S9.3 Group-Level UF

The compositor allocates one UF element per distinct group label per tile, not
per port. Phase 0 scans all TileOps, computes `max_group_label(tile)`, and
builds a prefix-sum:

```
group_offset[0] = 0
for t in 0..N:
    assert(!is_overflow(t))   // caller must pre-process all overflow tiles
    if empty(t):
        group_offset[t + 1] = group_offset[t]
    else:
        // tile_data(t) returns 256-byte data from side table for extended tiles
        budget = is_extended(t) ? 253 : 125
        group_offset[t + 1] = group_offset[t] + max_group_label(tile_data(t), budget)
```

Global ID:

```
global_id(t, G) = group_offset[t] + (G - 1)
```

This is the canonical compositor addressing model. The old fixed-slot or
embedded-parent schemes are not part of v2.

### S9.4 Phase 1: I/O Matching (Within Towers)

For each tower `j`, for each row `r` in `[0, tiles_per_tower[j]-1)`:

```
a = tile_index(j, r)       // lower tile, O-face
b = tile_index(j, r + 1)   // upper tile, I-face

if empty(a) || empty(b): continue

a_groups = face_groups(a, O)   // packed O section
b_groups = face_groups(b, I)   // packed I section
assert(len(a_groups) == len(b_groups))

for s in 0..len(a_groups):
    uf.union(global_id(a, a_groups[s]), global_id(b, b_groups[s]))
```

I/O matching still uses shared-prime identity on the duplicated boundary row.
TileOp v2 only changes where the face bytes are read from.

### S9.5 Phase 2: L/R Matching (Between Towers)

For each adjacent tower pair `(j, j+1)`:

```
d = delta[j]
q = d / S
f = d % S

for r in 0..tiles_per_tower[j]:
    a = tile_index(j, r)
    if empty(a): continue

    // primary neighbor
    if r + q < tiles_per_tower[j + 1]:
        b = tile_index(j + 1, r + q)
        if !empty(b):
            match_lr(a, b, -f)

    // secondary neighbor
    if f > 0 && r + q + 1 < tiles_per_tower[j + 1]:
        b = tile_index(j + 1, r + q + 1)
        if !empty(b):
            match_lr(a, b, S - f)
```

Where:

```
function match_lr(a, b, delta_h):
    a_groups = face_groups(a, R)
    a_h1     = face_h1(a, R)   // decoded from group byte bit 7 + h1 byte
    b_groups = face_groups(b, L)
    b_h1     = face_h1(b, L)   // decoded from group byte bit 7 + h1 byte
    for sa in 0..len(a_groups):
        gid_a = decode_group_id(a_groups[sa])
        if gid_a == 0: continue   // zero-padded R slot
        target = a_h1[sa] - delta_h
        for sb in 0..len(b_groups):
            gid_b = decode_group_id(b_groups[sb])
            if gid_b == 0: continue   // zero-padded L slot
            if b_h1[sb] == target:
                uf.union(global_id(a, gid_a),
                         global_id(b, gid_b))
                // No break — a port at h1_l == f can match both
                // primary and secondary neighbors (dual-neighbor matching)
```

The packed sections are variable-length, so the inner loops run to the actual
face counts rather than to a fixed slot count. The derived `r_cnt` may include
zero-padded trailing entries where `decode_group_id(group_byte) == 0` — these
must be skipped to avoid unsigned underflow in `global_id(t, 0)`.

**No overflow tiles reach the compositor.** The caller pre-processes all
overflow tiles into 256-byte extended TileOps before invoking the compositor.
See compositor_spec.md v8, S7.1.

### S9.6 Overflow Handling

**No overflow tiles reach the compositor.** The caller (campaign runner or
test harness) detects overflow tiles (`tile[0] == 0xFF`) and reprocesses
them via C++ into 256-byte extended TileOps BEFORE calling the compositor.
The compositor asserts `!is_overflow(tile)` for every tile. Extended tiles
are parsed normally via the side table with `payload_budget = 253`. See
compositor_spec.md v8, S7.1 for details.

### S9.7 Phase 3: Spanning Check

After all matching:

```
inner_members = []   // raw global_ids, not roots

for each tower j:
    // I-face of row 0 (horizontal tread)
    add global_ids of all I-face groups of tile(j, 0)

    // Exposed L-face ports at inner staircase risers (j > 0 only)
    if j > 0:
        q_prev, f_prev from delta[j-1]
        if q_prev > 0: add ALL L-face ports of rows 0..q_prev-1 (skip empty, skip group_id==0)
        if f_prev > 0: add L-face ports of row q_prev with h1 < f_prev (skip empty, skip group_id==0)

// Re-find all inner members to get current roots
inner_roots = { find(id) for id in inner_members }

for each tower j:
    let T_j = tiles_per_tower[j]

    // O-face of topmost row (horizontal tread)
    if any O-face group root of tile(j, T_j-1) is in inner_roots: return Spanning

    // Exposed R-face ports at delta-based outer staircase risers (j < J-1 only)
    if j < J-1:
        q, f from delta[j]
        if q > 0: check ALL R-face ports of rows (T_j-q)..T_j-1 (skip empty, skip group_id==0)
        if f > 0: check R-face ports of row (T_j-1-q) with h1 >= S-f (skip empty, skip group_id==0)

    // Exposed R-face ports at height-staircase risers (tower j taller than j+1)
    if j < J-1:
        let hd = T_j - tiles_per_tower[j+1]
        if hd > 0: check ALL R-face ports of rows (T_j-hd)..T_j-1 (skip empty, skip group_id==0)

    // Exposed L-face ports at height-staircase risers (tower j taller than j-1)
    if j > 0:
        let hd = T_j - tiles_per_tower[j-1]
        if hd > 0: check ALL L-face ports of rows (T_j-hd)..T_j-1 (skip empty, skip group_id==0)

return Moat
```

The exposure rules from S5 still apply. Boundary tracking stores member
global_ids (not roots) during ingestion; `finalize()` re-finds all members.
See compositor_spec.md v8, S6.5 for the detailed pseudocode.

### S9.8 Cost Model

The compositor remains `O(N + M * alpha(M))`, where `M` is the number of
group-level UF operations. TileOp v2 adds only three header reads and simple
count arithmetic per tile. This is negligible relative to UF traffic.

---

## S10. Memory Layout

### S10.1 Tower-Major Ordering

TileOps are stored in tower-major order:

```
tile_ops[tile_offset[j] + r]    // 128 bytes each
```

where `tile_offset[j] = sum(tiles_per_tower[0..j])` is the prefix-sum array
(S10.6).

**Spatial locality:**
- I/O matching (Phase 2): accesses tile_ops[tile_offset[j] + r] and
  tile_ops[tile_offset[j] + r+1] — consecutive in memory. Sequential scan
  within each tower. Stride: 128 bytes. Excellent prefetch behavior.
- L/R matching (Phase 3): accesses tiles in tower j and tower j+1 — stride
  of tiles_per_tower[j] * 128 bytes (4096--5888 bytes) between towers.
  Still L2-friendly on modern CPUs.

### S10.2 Tile Status Access

Tile status for tile (j, r) is derived from group data in `tile_ops[tile_offset[j] + r]`
(tile_spec S4.3). Overflow: `bytes[0] == 0xFF`. Empty (zero primes):
`bytes[0] == bytes[1] == bytes[2] == 3` and `bytes[3] == 0`. No separate
status array exists.

### S10.3 Delta Table

```
delta[j]    // 4 bytes each (u32), j in [0, J-2]
```

Total: (J-1) * 4 ~ 9 MB. Fits in L3 cache.

### S10.4 Base_y Table

```
base_y[j]    // 8 bytes each (i64), j in [0, J-1]
```

Total: J * 8 ~ 18 MB. Fits in L3 cache. Used for lattice coordinate
derivation and octant stitching, not for the hot composition loop.

### S10.5 Memory Budget at R = 830M

| Structure | Size | Notes |
|-----------|------|-------|
| tile_ops | 10.85 GB | ~84.8M tiles * 128 B |
| group-level UF parent | 1.70 GB | ~424M groups * 4 B (5 groups/tile avg) |
| group_offset prefix-sum | 339 MB | ~84.8M tiles * 4 B |
| delta table | 9 MB | 2.3M towers * 4 B |
| base_y table | 18 MB | 2.3M towers * 8 B |
| tiles_per_tower table | 2.3 MB | 2.3M towers * 1 B (u8) |
| tile_offset prefix-sum | 9.2 MB | 2.3M towers * 4 B |
| **Total** | **~12.9 GB** | |

An 80 GB A100 has ample room. A 24 GB consumer GPU can also hold this footprint
with tighter overhead margins, though sector batching remains useful for
pipeline flexibility and future larger-band experiments.

### S10.6 Tile Offset Prefix-Sum

```
tile_offset[0] = 0
tile_offset[j] = tile_offset[j-1] + tiles_per_tower[j-1]
```

Total: J * 4 bytes (u32) ~ 9.2 MB. Used for tower-major array indexing
and inverse addressing (S6.2).

---

## S11. Grid Construction Algorithm

### S11.1 Pseudocode

```
function build_grid(R: i64, S: i64 = 256) -> Grid:
    let mut towers: Vec<Tower> = []
    let mut prev_y: i64 = 0
    let mut tpt: Vec<u8> = []       // tiles_per_tower
    
    for j in 0, 1, 2, ...:
        let x_j: i64 = j * S
        
        // Check if x_j exceeds the y-axis intercept of the inner arc
        if x_j as i128 * x_j as i128 > R as i128 * R as i128:
            break
        
        // Inner arc y-coordinate (continuous)
        let y_cont: f64 = sqrt((R as f64)^2 - (x_j as f64)^2)
        
        // Threshold rounding with monotonicity clamp
        let y_j: i64 = round(y_cont)
        if j > 0 && y_j > prev_y:
            y_j = prev_y
        prev_y = y_j
        
        // Per-tower height (S4.2a)
        let cos_theta = y_j as f64 / sqrt((x_j as f64)^2 + (y_j as f64)^2)
        let h: u8 = ceil(32.0 / cos_theta).clamp(32, 46)
        
        // Termination: tower top + MARGIN must be above y = x (S4.3, S7.1)
        let MARGIN: i64 = 2 * S
        if y_j + (h as i64) * S + MARGIN <= x_j:
            break
        
        towers.push(Tower { base_x: x_j, base_y: y_j, id: j })
        tpt.push(h)
    
    let J = towers.len()
    
    // Precompute delta table
    let mut delta: Vec<u32> = Vec::with_capacity(J - 1)
    for j in 0..J-1:
        let d = towers[j].base_y - towers[j+1].base_y
        assert(d >= 0, "delta must be non-negative (arc descends)")
        delta.push(d as u32)
    
    // Precompute tile_offset prefix-sum (S10.6)
    let mut tile_offset: Vec<u32> = Vec::with_capacity(J)
    tile_offset.push(0)
    for j in 0..J-1:
        tile_offset.push(tile_offset[j] + tpt[j] as u32)
    
    // Count sub-diagonal tiles (informational; all tiles are processed)
    let mut sub_diag_count = 0
    for j in 0..J:
        for r in 0..tpt[j]:
            let top_y = towers[j].base_y + (r + 1) * S
            let min_x = towers[j].base_x
            if top_y <= min_x:
                sub_diag_count += 1
    
    let total_tiles = tile_offset[J-1] + tpt[J-1] as u32
    
    return Grid {
        R, S,
        towers,
        delta,
        tiles_per_tower: tpt,
        tile_offset,
        total_tiles,
    }
```

### S11.2 Numerical Precision

The inner-arc computation uses f64 for sqrt. At R = 870M:

- R^2 = 7.569e17, which has 60 bits. f64 has 53-bit mantissa. The
  subtraction R^2 - x_j^2 may lose precision when x_j is close to R
  (catastrophic cancellation).
- **Mitigation:** for towers near the diagonal (x_j ~ R/sqrt(2)),
  R^2 - x_j^2 ~ R^2/2 ~ 3.8e17, which has 59 bits — still exceeds f64
  mantissa. However, the loss is at most a few ULP, and the threshold
  rounding absorbs this: the absolute error in y_j is at most 0.5
  lattice units per tower by construction.
- **Stronger mitigation (recommended):** compute R^2 - x_j^2 in i128, then
  apply integer sqrt (Newton's method) to get an exact integer floor. This
  eliminates all floating-point concerns. The threshold rounding
  then operates on the integer floor value.

### S11.3 Verification

The grid construction can be verified by checking:

1. **Arc tracking:** for each tower j, |base_y[j] - sqrt(R^2 - (j*S)^2)| <= 0.5.
2. **Delta consistency:** delta[j] = base_y[j] - base_y[j+1] for all j.
3. **Monotonicity:** delta[j] >= 0 and non-decreasing (approximately; exact
   monotonicity depends on rounding).
4. **Termination:** the last tower has its top edge + MARGIN above y = x;
   the next tower would fail the termination predicate (S4.3).
5a. **Tower height bounds:** tiles_per_tower[j] >= 32 for all j, and
   tiles_per_tower[j] <= 46 for all j.
5b. **Gentle ramp:** |tiles_per_tower[j] - tiles_per_tower[j+1]| <= 1.
6. **Coverage:** every lattice point in the octant annulus with y >= x is
   within S/2 lattice units of some tile's center (approximate; exact
   coverage depends on arc curvature vs. tile size).

---

## S12. Ring Expansion (Search Procedure)

### S12.1 The Search Loop

The moat search proceeds outward from a starting radius R_0 (identified by
ISE or prior computation as a region of low connectivity):

```
R = R_0
while R < R_max:
    grid = build_grid(R)
    tile_ops = cuda_kernel(grid)                    // batched launch
    verdict = compositor(tile_ops, grid)
    
    if verdict == Moat:
        log("Moat candidate at R = {R}")
        verify_moat(R)       // finer resolution, optional wider band
        break
    
    // Spanning: advance to next band
    R = R + W                // non-overlapping bands
```

### S12.2 Why Not Binary Search

Spanning is NOT monotone in R in the transition zone (826M--835M). A band
at radius R may span while R + W does not, and R + 2W spans again. This
non-monotonicity is caused by local fluctuations in Gaussian prime density.
Binary search assumes monotonicity and would skip valid moat candidates.

Ring expansion (sequential scan) is correct: it examines every band and
cannot miss a moat.

### S12.3 Band Overlap

Adjacent bands share no tiles (they are non-overlapping: band k covers
[R + k*W, R + (k+1)*W]). A spanning path that crosses a band boundary is
detected in each band independently — the path spans the full radial depth
of each band it crosses.

**Edge case:** a moat that is narrower than W might straddle a band
boundary. If the moat gap falls exactly at the seam between two bands, each
band individually spans (the gap is only in part of the band). To handle
this:

- **Sliding window:** instead of non-overlapping bands, advance by W/2 so
  consecutive bands overlap by W/2. Any moat of width <= W/2 is fully
  contained in at least one band.
- **Phase 1 approach (recommended):** use non-overlapping bands. If no moat
  is found, the search is negative and we advance. Overlapping windows are a
  Phase 2 refinement.

---

## S13. Compositor Complexity

### S13.1 Per-Phase Cost

Let N = total tiles, G = total active groups, and J = number of towers.

| Phase | Operations | Data reads | Notes |
|-------|-----------|------------|-------|
| 0 (prefix-sum) | O(N) | header + packed group sections | One pass over all TileOps |
| 1 (I/O matching) | O(P_IO) | packed O/I sections | Shared-prime identity |
| 2 (L/R matching) | O(P_LR * avg_face_ports) | packed L/R sections + decoded h1 | h1 matching, two faces per pair |
| 3 (octant boundary) | 0 | — | No computation: first-octant sufficiency via extended tower generation (S8.2) |
| 4 (spanning check) | O(P_boundary) | boundary port roots | Hash set lookup |

Where P_IO = total O-face ports matched (= I-face ports of the next row) and
P_LR = total L/R face pairs matched. TileOp v2 removes the old fixed-slot face
cap; the relevant bound is the actual packed face count.

### S13.2 Total Cost

```
O(N + G * alpha(G))
```

where alpha is the inverse Ackermann function from union-find. Effectively
linear in the number of tiles.

At R = 830M: N ~ 84.8M tiles, G ~ 424M groups (about 5 groups/tile average).
With union-find operations dominating, and each operation being a few
nanoseconds after path compression:

```
Estimated UF work: 424M * 5 ns ~ 2.1 seconds
```

Plus TileOp scanning and boundary handling, the compositor remains comfortably
below the CUDA kernel runtime at operating radii.

**Order-of-magnitude estimate:** low-single-digit seconds per full band on CPU,
with the exact figure dominated by UF locality and boundary-root hashing rather
than by TileOp v2 parsing overhead.

---

## S14. TileOp Layout and Cache Behavior

### S14.1 Current Layout (tile_spec v5 — Dynamic Packed)

TileOp v2 uses a 3-byte header and a packed payload:

```
Byte 0: off_I
Byte 1: off_L
Byte 2: off_R

Bytes 3 .. off_I - 1                 Face O groups
Bytes off_I .. off_L - 1             Face I groups
Bytes off_L .. off_R - 1             Face L groups
Bytes off_R .. off_R + r_cnt - 1     Face R groups
Bytes h_start .. h_start + l_cnt - 1 Face L h1 bytes
Bytes h_start + l_cnt .. 127         Face R h1 bytes
```

The payload order is **O-I-L-R** so that the most frequent I/O matching data
resides in the lowest byte offsets.

### S14.2 Cache Behavior

- Header parsing costs three byte loads plus simple arithmetic.
- I/O matching usually stays within cache line 0 because O/I groups are packed
  immediately after the header.
- L/R matching reads deeper into the TileOp, typically touching late group
  bytes and packed `h1` bytes.
- The old split "all groups in line 0, h1 in line 1" no longer applies.

### S14.3 Octant Boundary (Resolved)

TileOp v2 omits I/O h1 storage. This was previously a blocker for
face-based diagonal stitching. With extended tower generation (S8.2),
no diagonal stitching is performed — sub-diagonal tiles are standard
CUDA-produced TileOps, and standard I/O/L/R composition handles all
cross-diagonal connectivity. No spare-byte escape hatch is needed.

---

## S15. Open Questions

### Q1. Octant Stitching Detail — RESOLVED

Resolved by extended tower generation (S8.2). No face-based diagonal
stitching is performed. Sub-diagonal tiles in extended towers provide
second-octant coverage, and standard composition handles cross-diagonal
connectivity. The face-mapping and delta_h questions are moot.

### Q2. Parallelism Strategy

The compositor phases have different parallelism profiles:

- Phase 1 (group wiring): embarrassingly parallel across tiles.
- Phase 2 (I/O matching): parallel across towers (sequential within each
  tower due to row ordering, but towers are independent).
- Phase 3 (L/R matching): parallel across tower pairs.
- Phase 4 (spanning check): sequential scan with hash set.

Union-find is the synchronization bottleneck. Options:

- **Single-threaded compositor:** simplest, ~4 seconds. Acceptable if the
  CUDA kernel dominates.
- **Concurrent UF (Jayanti-Tarjan):** lock-free union-find with atomic
  compare-and-swap. Phases 1-3 run in parallel across tiles.
- **Two-phase compositor:** compose within each tower (Phases 1-2) in
  parallel with independent per-tower UFs, then merge across towers
  (Phase 3) with a global UF.

Recommended starting point: single-threaded. Profile, then parallelize if
the compositor is the bottleneck.

### Q3. Transfer Reduction

Each tile's TileOp encodes full face-to-face connectivity. The compositor
could pre-reduce each tile to its I-to-O transfer function (which I-face
ports reach which O-face ports), then chain these transfer functions
row-by-row within each tower. This would replace the full UF with a smaller
per-tower transfer chain.

**Benefit:** UF memory drops from O(P) to O(P_boundary) — only boundary
ports need global UF entries. Intra-tower wiring is done via transfer
matrices.

**Cost:** additional preprocessing per tile to extract the I-to-O transfer
matrix. Under TileOp v2 this scales with the actual packed O/I face counts
rather than with a fixed-size face array.

Deferred to implementation.

### Q3a. EXPERIMENTAL: Face-pair connectivity bits for transfer reduction

Six bits encoding which face pairs are internally connected within a tile:
IO, IL, IR, LO, LR, RO. Bit X_Y = 1 iff some group touches both face X
and face Y.

These were originally considered for inclusion in the TileOp but removed — no
current compositor phase consumes them. The compositor determines
face-pair connectivity implicitly through group label wiring in Phase 1.

**Potential value:** if IO = 0 for a tile, the transfer reduction can skip
it immediately (no I-to-O path exists through this tile). For row-by-row
composition, this is a free O(1) filter that avoids loading the full
TileOp.

**Implementation path:** if adopted, these bits would need a dedicated side
buffer or a widened TileOp variant. TileOp v2 has no spare in-record bytes for them.

**Status:** EXPERIMENTAL — not part of the current tile_spec or compositor
design. Requires the transfer reduction architecture (Q3 above) to be
useful. Do not implement until Q3 is resolved.

### Q4. Sector Batching

At R = 830M, the full octant has J ~ 2.3M towers and ~85M tiles. If memory
permits processing the full octant at once (~12.9 GB), no batching is needed.
If the target GPU has less memory (e.g., 24 GB RTX 4090, which cannot hold
TileOps + UF simultaneously), the octant must be split into angular sectors.

Sector boundaries are vertical lines x = const (specific tower indices).
Each sector is composed independently. At sector boundaries, L/R matching
connects the sectors. This is identical to normal L/R matching — no special
logic needed.

### Q5. Sliding Window Refinement

The non-overlapping band scan (S12.3) may miss moats narrower than W that
straddle band boundaries. The sliding-window approach (advance by W/2) fixes
this at 2x the computation cost. Whether this refinement is needed depends
on the expected moat width relative to W = 8192.

---

## S16. Invariants

The following invariants must hold throughout grid construction and
composition. Violation of any invariant is a hard error.

1. **Tower count:** J >= 1. (The annulus at any valid R intersects the
   y-axis, producing at least one tower.)

2. **Delta non-negativity:** delta[j] >= 0 for all j. (The inner arc
   descends monotonically in the first octant.)

3. **Delta boundedness:** delta[j] < 2 * S = 512 for all j. (The arc's
   discrete slope between adjacent towers is less than 2 tile-widths.
   This follows from the arc geometry: the maximum slope of the inner arc
   in the first octant is 1 (at y = x), yielding delta ~ S = 256. The
   factor-of-2 bound provides margin for rounding.)

4. **Row validity:** all computed neighbor rows r' are in
   [0, tiles_per_tower[j]). (Enforced by the bounds checks in S6.4.)

5. **Port count agreement on aligned faces:** for tiles (j, r) and (j, r+1),
   the number of nonzero groups on (j, r)'s O-face equals the number on
   (j, r+1)'s I-face. (Guaranteed by tile_spec determinism: both kernels
   see the same primes in the shared collar.)

6. **No false moats from empty tiles:** every empty tile (zero collar-zone
   primes) has the empty TileOp sentinel (S7.3, tile_spec S4.3).
   The compositor never matches an empty tile's ports.

7. **Spanning monotonicity within a band:** not assumed. The spanning check
   examines the full UF after all matching is complete.

8. **Tower height lower bound:** tiles_per_tower[j] >= 32 for all j.
   (Every tower has at least the minimum radial depth.)

9. **Gentle ramp:** |tiles_per_tower[j] - tiles_per_tower[j+1]| <= 1
   for all j. (Tower heights change by at most 1 tile between neighbors.)

10. **Radial coverage:** effective radial coverage >=
    W_RADIAL = 8192 lattice units at all angles. (Guaranteed by the
    per-tower height formula in S4.2a.)

---

## S17. Glossary

| Term | Definition |
|------|------------|
| **Tower** | A vertical column of tiles (32--46), addressed by tower index j. Height varies for consistent radial coverage. |
| **Row** | A tile's position within its tower, r in [0, tiles_per_tower[j]-1]. Row 0 is innermost. |
| **Delta** | The vertical offset delta[j] = base_y[j] - base_y[j+1] between adjacent towers. |
| **Primary neighbor** | The main overlapping tile in the adjacent tower (overlap = S - f). |
| **Secondary neighbor** | The secondary overlapping tile (overlap = f). Exists only when f > 0. |
| **Non-empty tile** | A tile that is neither empty nor overflow — has at least one nonzero group slot. |
| **Empty tile** | A tile with zero collar-zone primes. Emits the empty TileOp sentinel (S7.3). |
| **Sub-diagonal tile** | A tile partially or entirely below y = x. Processed normally by CUDA (S7.2). |
| **Spanning** | A connected component of ports touches both the inner and outer boundary. |
| **Moat** | No connected component spans the annulus — a gap in Gaussian prime connectivity. |
| **Outer staircase** | The staircase pattern at the outer boundary caused by variable tower heights, analogous to the inner staircase from base_y differences. Towers that are taller than their neighbors expose L/R faces at the top (S5.6). |
| **Octant stitching** | Connecting ports across the y = x diagonal between the working octant and its reflection. |
