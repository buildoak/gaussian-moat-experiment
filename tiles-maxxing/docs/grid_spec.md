---
title: "Grid Specification v2 — Tower Geometry and Compositor"
date: 2026-04-09
version: 2
status: draft
depends: tile_spec.md (v5)
---

# Grid Specification v2 — Tower Geometry and Compositor

## S1. Overview

This specification defines how TileOps (tile_spec.md) compose at scale to
answer the Gaussian moat question: does there exist a connected path of
Gaussian primes, with step distance at most sqrt(k), from the origin to
infinity?

The search operates on annular bands of the Gaussian integer lattice. Each
band is tiled by a rectangular array of **towers** — vertical columns of 32
tiles each — that follow the inner arc of the annulus. A Rust **compositor**
wires TileOps together via union-find and checks whether any connected
component of boundary ports spans from the inner edge to the outer edge of
the annulus. If no component spans: the annulus is a moat at that radius.

Key properties:

- **Pure rectangular array** — no irregular tiles, no gap-filling, no special
  zones. Every tower has exactly 32 tiles.
- **O(1) neighbor lookup** — within a tower by row arithmetic, between towers
  by a precomputed delta table.
- **Two matching modes** — I/O faces use the positional shortcut (groups
  only); L/R faces use h1 equality with a per-tower-pair delta_h.
- **8-fold symmetry** — only one octant is tiled. Octant stitching at the
  y = x diagonal completes the annulus.
- **Memory-bounded** — 9.38 GB TileOps + ~1.76 GB group-level UF metadata at
  R = 830M (~11.2 GB total). Fits a single 80 GB GPU with room for working
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
| W | 8192 | Annulus radial depth = TILES_PER_TOWER * S. |
| TILES_PER_TOWER | 32 | W / S. |
| K | 40 | Step distance squared. From tile_spec. |
| COLLAR | 7 | ceil(sqrt(K)). From tile_spec. |
| MIN_OVERLAP | COLLAR | Minimum L/R face overlap for any port to exist in the shared zone (S5.3). |

---

## S3. Annulus and Coordinate System

### S3.1 The Search Annulus

Fix a search radius R (a positive integer, typically 826M -- 870M). The
annulus is the set of lattice points (x, y) with:

```
R^2 <= x^2 + y^2 <= (R + W)^2
```

where W = 8192 is the radial depth. By 8-fold Gaussian integer symmetry
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
  of the tower, row 31 is the outermost.

---

## S4. Tower Construction

### S4.1 Tower Definition

A **tower** is a column of TILES_PER_TOWER = 32 tiles stacked vertically.
Tower j is defined by:

```
base_x[j] = j * S
base_y[j] = computed from the inner arc (S4.2)
```

Tile (j, r) occupies the axis-aligned square:

```
x in [base_x[j],  base_x[j] + S)
y in [base_y[j] + r*S,  base_y[j] + (r+1)*S)
```

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

**Accumulated-correction rounding.** Naive rounding of each y_cont[j]
independently introduces O(1) error per tower, which can accumulate. Instead,
we track the fractional error and correct:

```
function compute_base_y(R, S):
    err = 0.0
    base_y = []
    for j = 0, 1, 2, ...:
        x_j = j * S
        if x_j^2 > R^2:
            break       // tower base is beyond the y-axis intercept
        y_cont = sqrt(R^2 - x_j^2)
        y_j = round(y_cont - err)
        err += y_j - y_cont
        base_y.push(y_j)
    return base_y
```

**Invariant:** |err| <= 0.5 at all times. The accumulated correction
distributes the total rounding error evenly, preventing drift between the
discrete tower base and the continuous arc.

**Integer arithmetic note.** In the CUDA/Rust implementation, R^2 and x_j^2
are computed in u128 or i128 to avoid overflow at R ~ 870M (R^2 ~ 7.6e17,
which exceeds u32 and i64 ranges). The sqrt is computed via integer Newton's
method or via f64 with a +-1 correction step.

### S4.3 Tower Count and Termination

Towers march rightward from j = 0 (at the y-axis) following the inner arc
downward. The octant boundary is the line y = x. We continue placing towers
**past** y = x until the last tower's top edge is fully below the diagonal:

```
Termination predicate: stop adding towers when
    base_y[j] + W <= j * S
```

That is, tower j is included only if its topmost tile's top edge
(base_y[j] + W) is strictly above the line y = x (which at x = j*S has
y = j*S). The first tower that fails this test is NOT included.

Let J denote the number of towers (j ranges from 0 to J-1).

**Justification.** Tiles above y = x are "alive" (part of our octant). Tiles
below y = x are "dead" (belong to the reflected octant). By continuing past
y = x, the topmost alive tiles of the last few towers overlap with the
reflected octant's tiles along the diagonal — this overlap zone is the
natural seam for octant stitching (S8).

### S4.4 Tower Geometry at Search Radius

At R = 830M:

```
J ≈ R / (sqrt(2) * S) + O(1) ≈ 830e6 / (1.414 * 256) ≈ 2,293,126 towers
```

The "+O(1)" accounts for the overshoot past y = x. Total tiles:

```
N = J * 32 ≈ 73.4M tiles
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
compositor reads only group labels (16 bytes per face), not h1 values.

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

**Existence check.** A neighbor (j+1, r') is valid only if 0 <= r' < 32.
If r + q >= 32 or r + q < 0, there is no primary neighbor (the R-face of
tile (j, r) extends beyond tower j+1's vertical extent). Similarly for
r + q + 1.

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

For the primary neighbor (delta_h = -f): a.h1 + f == b.h1.
For the secondary neighbor (delta_h = S - f): a.h1 - (S - f) == b.h1,
i.e., a.h1 + f == b.h1 + S.

**Validity of matched ports.** A match is geometrically valid only if the
matched port lies in the overlap zone. For the primary neighbor, the overlap
spans the lower S - f units of the R-face, so valid ports have h1 < S - f.
For the secondary neighbor, valid ports have h1 >= S - f (equivalently,
h1 is in [S - f, S)).

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

### S5.6 Summary: L/R Neighbor Table

For tile (j, r) with d = delta[j], q = d div S, f = d mod S:

| Neighbor | Row in tower j+1 | Overlap (lattice units) | delta_h | Valid if |
|----------|-------------------|-------------------------|---------|----------|
| Primary | r + q | S - f | -f | 0 <= r + q < 32 |
| Secondary | r + q + 1 | f | S - f | f > 0 AND 0 <= r + q + 1 < 32 |

The symmetric case (L-face of tile (j, r) matched against R-faces in tower
j-1) uses delta[j-1] with the roles reversed.

---

## S6. Tile Addressing

### S6.1 Composite Tile Address

Each tile is addressed by a composite `(tower_id: u32, tile_pos: u8)`:

```
tower_id: u32   — tower index j (0 to J-1). At R = 830M, J ~ 2.29M;
                  at R = 2B, J ~ 5.5M. Fits u32 comfortably.
tile_pos: u8    — row within tower (0 to 31).
```

Total tiles: N = J * 32, where J is the tower count.

**Why not a flat u32 index.** A flat index `j * 32 + r` fits u32 at 73.4M
tiles (max ~2.29M * 32 = 73.4M < 4.29B). However, port addressing in the
naive UF scheme requires `tile_index * 64` which reaches 4.7B — overflowing
u32. The composite address avoids this: port addresses use the composite
tile address directly (see S9.2), never requiring a global flat tile index
multiplied by a slot count.

**Flat index for array layout.** TileOps are still stored contiguously in
tower-major order: `tile_ops[j * 32 + r]`. The flat index is used ONLY for
array addressing (always within u32 range at 73.4M tiles), never for port
identity computation.

### S6.2 Inverse Addressing

From the flat array index:

```
tower_id(flat_index) = flat_index / 32     (integer division)
tile_pos(flat_index) = flat_index mod 32
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
| O (outer) | (j, r+1) | r < 31 |
| R primary | (j+1, r + q) | j < J-1, 0 <= r+q < 32 |
| R secondary | (j+1, r + q + 1) | j < J-1, f > 0, 0 <= r+q+1 < 32 |
| L primary | (j-1, r - q') | j > 0, 0 <= r-q' < 32 |
| L secondary | (j-1, r - q' - 1) | j > 0, f' > 0, 0 <= r-q'-1 < 32 |

Where q = delta[j] div S, f = delta[j] mod S for R-neighbors, and
q' = delta[j-1] div S, f' = delta[j-1] mod S for L-neighbors.

All lookups are O(1): one integer division, one modulo, one addition.

### S6.5 Port Address

A port is addressed by `(tower_id: u32, tile_pos: u8, face: u2, slot: u4)`:

```
tower_id: u32   — which tower
tile_pos: u8    — which tile within tower (0-31)
face:     u2    — which face (0=I, 1=O, 2=L, 3=R)
slot:     u4    — which port slot on that face (0-15)
```

Packed representation: 8 bytes (u32 + u8 + u8 packed with face:2 + slot:4
+ 2 spare bits). This replaces the flat `global_id = t * 64 + f * 16 + s`
which overflows u32 at 73.4M tiles.

---

## S7. Dead Tile Predicate

### S7.1 Definition

A tile (j, r) is **dead** if its entire area lies below the line y = x (i.e.,
it belongs to the reflected octant, not our working octant):

```
dead(j, r) := (base_y[j] + (r+1)*S) <= (j * S)
```

Equivalently: the tile's top edge (max y) is at or below the diagonal at
the tile's x-coordinate. Since the tile is axis-aligned and y = x is linear,
if the top-right corner is below y = x, the entire tile is below.

A more precise predicate (the top-left corner is also below y = x):

```
dead(j, r) := (base_y[j] + (r+1)*S) <= (j * S)
```

This suffices because base_y[j] + (r+1)*S is the tile's max y-value, and
j*S is the tile's min x-value. If max_y <= min_x, then for all points
(x, y) in the tile, y <= max_y <= min_x <= x, so y <= x throughout.

### S7.2 CUDA Kernel Behavior

The CUDA kernel checks the dead predicate before sieving. For dead tiles:

- Emit the empty TileOp v2 header state: `off_I = off_L = off_R = 3`, with
  zero payload. This signals a dead tile to the compositor.
- No sieving, no primality testing, no union-find work.

### S7.3 Alive Tiles Near the Diagonal

Tiles that straddle y = x (partially above, partially below) are alive.
They are processed normally. Their interior prime graph includes primes both
above and below y = x — this is correct and necessary for octant stitching
(S8).

### S7.4 Dead Tile Fraction

At R = 830M, the last ~32 towers before termination contain dead tiles. The
fraction of dead tiles is small: approximately (number of sub-diagonal tiles)
/ N. Since the diagonal only clips the bottom rows of the last few towers,
the dead fraction is < 1% of total tiles.

---

## S8. Octant Stitching

### S8.1 The Problem

We tile only the octant {y >= x >= 0}. A connected path of Gaussian primes
may cross the y = x line, entering the adjacent octant {x >= y >= 0}. If we
compose only within our octant, we miss such cross-octant paths and may
report a false moat.

### S8.2 Symmetry Argument (Single Octant Only)

**Claim:** if a spanning path exists in the full annulus, then a spanning
path exists entirely within a single octant.

**Status: UNPROVEN.** This claim is plausible by symmetry — the 8-fold
Gaussian symmetry maps any prime path to 7 other paths — but the deformation
argument is non-trivial. A path that crosses y = x multiple times cannot
obviously be "reflected" into a single octant without breaking connectivity.

**Risk direction:** if this claim is false, the single-octant shortcut misses real spanning
paths, reporting false moats. This is the UNSAFE direction for moat search.

**Recommendation: do not use the single-octant shortcut without a proof.**

### S8.3 Boundary Stitching (Recommended)

Each octant is composed independently. At the y = x boundary, tiles that
straddle the diagonal share primes with the reflected octant. The stitching
protocol connects these shared primes.

**The reflection map.** For a Gaussian integer z = x + yi, the map
sigma: z -> iz* = y + xi swaps real and imaginary parts. This is a symmetry
of the Gaussian primes (if z is a Gaussian prime, so is sigma(z)). Under
sigma:

```
(x, y) -> (y, x)
```

A tile at (tile_x, tile_y) maps to a tile at (tile_y, tile_x).

**Face mapping under reflection.** Reflection swaps x and y axes:

- Face I (bottom, fixed y) <-> Face L (left, fixed x)
- Face O (top, fixed y) <-> Face R (right, fixed x)

So a port on tile A's R-face, after reflection, appears on the reflected
tile's O-face.

**Stitching protocol for tile (j, r) straddling y = x:**

1. Identify the reflected tile: (j', r') such that tile_x(j', r') = tile_y(j, r)
   and tile_y(j', r') = tile_x(j, r). This tile exists in the reflected
   octant (or equivalently, in our octant under the reflection map).

2. Compute delta_h for the shared face. The shared geometric edge is along
   y = x. The exact offset depends on the tile geometry.

3. Match ports using h1 equality with the computed delta_h (tile_spec S5.1).
   The reflected tile's port h1 values are measured from ITS origin corner,
   which is the reflected position. The matching must account for the
   axis swap.

**Implementation.** Since our grid extends past y = x (S4.3), the "reflected
tile" is often another tile in our own grid (one of the dead or straddling
tiles). We can match R-faces of alive tiles near y = x with O-faces (after
reflection relabeling) of their mirror counterparts within the same grid.

Detailed stitching geometry — which faces share which primes, the exact
delta_h formula under reflection — is deferred to the implementation
specification. The key correctness property is:

**Stitching Invariant:** every prime on or within COLLAR distance of the
y = x line is visible to at least one tile in each octant. Both tiles'
TileOps encode this prime's connectivity. Stitching connects the two
representations.

### S8.4 Stitching and the Spanning Check

After stitching, the union-find structure connects components across the
octant boundary. The spanning check (S9, Phase 4) operates on the full
stitched UF and detects paths that cross y = x.

---

## S9. Compositor Algorithm

### S9.1 Overview

The compositor receives:

- `tile_ops[0..N)`: array of N TileOps (128 bytes each, tile_spec v5 layout)
- `base_y[0..J)`: tower base y-coordinates
- `delta[0..J-1)`: tower delta table

It produces a boolean: **spanning** (path exists from inner to outer boundary)
or **moat** (no such path exists).

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

Dead tile:

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
    if dead(t) or overflow(t):
        group_offset[t + 1] = group_offset[t]
    else:
        group_offset[t + 1] = group_offset[t] + max_group_label(tile_ops[t])
```

Global ID:

```
global_id(t, G) = group_offset[t] + (G - 1)
```

This is the canonical compositor addressing model. The old fixed-slot or
embedded-parent schemes are not part of v2.

### S9.4 Phase 1: I/O Matching (Within Towers)

For each tower `j`, for each row `r` in `[0, 31)`:

```
a = tile_index(j, r)       // lower tile, O-face
b = tile_index(j, r + 1)   // upper tile, I-face

if dead(a) || dead(b): continue
if overflow(a) { handle_overflow(a); continue }
if overflow(b) { handle_overflow(b); continue }

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

for r in 0..32:
    a = tile_index(j, r)
    if dead(a): continue
    if overflow(a) { handle_overflow(a); continue }

    // primary neighbor
    if r + q < 32:
        b = tile_index(j + 1, r + q)
        if !dead(b) && !overflow(b):
            match_lr(a, b, -f)

    // secondary neighbor
    if f > 0 && r + q + 1 < 32:
        b = tile_index(j + 1, r + q + 1)
        if !dead(b) && !overflow(b):
            match_lr(a, b, S - f)
```

Where:

```
function match_lr(a, b, delta_h):
    a_groups = face_groups(a, R)
    a_h1     = face_h1(a, R)
    b_groups = face_groups(b, L)
    b_h1     = face_h1(b, L)
    for sa in 0..len(a_groups):
        target = a_h1[sa] - delta_h
        for sb in 0..len(b_groups):
            if b_h1[sb] == target:
                uf.union(global_id(a, a_groups[sa]),
                         global_id(b, b_groups[sb]))
                break
```

The packed sections are variable-length, so the inner loops run to the actual
face counts rather than to a fixed slot count.

### S9.6 Overflow Handling

When `tile[0] == 0xFF`, the compositor applies conservative bridging:

```
function handle_overflow(t):
    bridge = overflow_global_id(t)
    for each neighbor n of tile t:
        if dead(n): continue
        if overflow(n):
            uf.union(bridge, overflow_global_id(n))
            continue
        for g in face_groups(n, face_toward(t)):
            uf.union(bridge, global_id(n, g))
```

This adds false connectivity only and is therefore safe in the moat-search
direction.

### S9.7 Phase 3: Spanning Check

After all matching and octant stitching:

```
inner_roots = HashSet()

for each row-0 tile:
    add roots of all I-face groups
    add roots of exposed L/R boundary groups

for each row-31 tile:
    if any O-face group root is in inner_roots: return Spanning
    if any exposed L/R boundary group root is in inner_roots: return Spanning

return Moat
```

The exposure rules from S5 still apply; only the face access method changes.

### S9.8 Cost Model

The compositor remains `O(N + M * alpha(M))`, where `M` is the number of
group-level UF operations. TileOp v2 adds only three header reads and simple
count arithmetic per tile. This is negligible relative to UF traffic.

---

## S10. Memory Layout

### S10.1 Tower-Major Ordering

TileOps are stored in tower-major order:

```
tile_ops[j * 32 + r]    // 128 bytes each
```

**Spatial locality:**
- I/O matching (Phase 2): accesses tile_ops[j*32 + r] and tile_ops[j*32 + r+1]
  — consecutive in memory. Sequential scan within each tower. Stride: 128
  bytes. Excellent prefetch behavior.
- L/R matching (Phase 3): accesses tile_ops[j*32 + r] and
  tile_ops[(j+1)*32 + r'] — stride of 32 * 128 = 4096 bytes between towers.
  Still L2-friendly on modern CPUs (L2 line = 64 B, L2 size = 1-4 MB;
  two towers fit in 8 KB).

### S10.2 Tile Status Access

Tile status for tile (j, r) is derived from group data in `tile_ops[j * 32 + r]`
(tile_spec S4.3). Overflow: `bytes[0] == 0xFF`. Dead:
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
| tile_ops | 9.38 GB | 73.4M tiles * 128 B |
| group-level UF parent | 1.47 GB | ~367M groups * 4 B (5 groups/tile avg) |
| group_offset prefix-sum | 294 MB | 73.4M tiles * 4 B |
| delta table | 9 MB | 2.3M towers * 4 B |
| base_y table | 18 MB | 2.3M towers * 8 B |
| **Total** | **~11.2 GB** | |

An 80 GB A100 has ample room. A 24 GB consumer GPU can also hold this footprint
with tighter overhead margins, though sector batching remains useful for
pipeline flexibility and future larger-band experiments.

---

## S11. Grid Construction Algorithm

### S11.1 Pseudocode

```
function build_grid(R: i64, S: i64 = 256, W: i64 = 8192) -> Grid:
    assert(W == 32 * S)
    
    let mut towers: Vec<Tower> = []
    let mut err: f64 = 0.0
    
    for j in 0, 1, 2, ...:
        let x_j: i64 = j * S
        
        // Check if x_j exceeds the y-axis intercept of the inner arc
        if x_j as i128 * x_j as i128 > R as i128 * R as i128:
            break
        
        // Inner arc y-coordinate (continuous)
        let y_cont: f64 = sqrt((R as f64)^2 - (x_j as f64)^2)
        
        // Integer rounding with accumulated correction
        let y_j: i64 = round(y_cont - err)
        err += (y_j as f64) - y_cont
        
        // Termination: tower top must be above y = x
        if y_j + W <= x_j:
            break
        
        towers.push(Tower { base_x: x_j, base_y: y_j, id: j })
    
    let J = towers.len()
    
    // Precompute delta table
    let mut delta: Vec<u32> = Vec::with_capacity(J - 1)
    for j in 0..J-1:
        let d = towers[j].base_y - towers[j+1].base_y
        assert(d >= 0, "delta must be non-negative (arc descends)")
        delta.push(d as u32)
    
    // Count dead tiles
    let mut dead_count = 0
    for j in 0..J:
        for r in 0..32:
            let top_y = towers[j].base_y + (r + 1) * S
            let min_x = towers[j].base_x
            if top_y <= min_x:
                dead_count += 1
    
    let total_tiles = J * 32
    let alive_tiles = total_tiles - dead_count
    
    return Grid {
        R, S, W,
        towers,
        delta,
        total_tiles,
        alive_tiles,
        tiles_per_tower: 32,
    }
```

### S11.2 Numerical Precision

The inner-arc computation uses f64 for sqrt. At R = 870M:

- R^2 = 7.569e17, which has 60 bits. f64 has 53-bit mantissa. The
  subtraction R^2 - x_j^2 may lose precision when x_j is close to R
  (catastrophic cancellation).
- **Mitigation:** for towers near the diagonal (x_j ~ R/sqrt(2)),
  R^2 - x_j^2 ~ R^2/2 ~ 3.8e17, which has 59 bits — still exceeds f64
  mantissa. However, the loss is at most a few ULP, and the accumulated
  correction mechanism absorbs this: the absolute error in y_j is at most 1
  lattice unit, and err tracks the cumulative deviation.
- **Stronger mitigation (recommended):** compute R^2 - x_j^2 in i128, then
  apply integer sqrt (Newton's method) to get an exact integer floor. This
  eliminates all floating-point concerns. The accumulated-correction rounding
  then operates on the integer floor value.

### S11.3 Verification

The grid construction can be verified by checking:

1. **Arc tracking:** for each tower j, |base_y[j] - sqrt(R^2 - (j*S)^2)| <= 1.
2. **Delta consistency:** delta[j] = base_y[j] - base_y[j+1] for all j.
3. **Monotonicity:** delta[j] >= 0 and non-decreasing (approximately; exact
   monotonicity depends on rounding).
4. **Termination:** the last tower has at least one alive tile; the tower
   after termination would have no alive tiles.
5. **Coverage:** every lattice point in the octant annulus with y >= x is
   within S/2 lattice units of some alive tile's center (approximate; exact
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
| 2 (L/R matching) | O(P_LR * avg_face_ports) | packed L/R sections + h1 | h1 matching, two faces per pair |
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

At R = 830M: N = 73.4M tiles, G ~ 367M groups (about 5 groups/tile average).
With union-find operations dominating, and each operation being a few
nanoseconds after path compression:

```
Estimated UF work: 367M * 5 ns ~ 1.8 seconds
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
Bytes h_start .. h_start + l_cnt - 1 Face L h1
Bytes h_start + l_cnt .. 127         Face R h1
```

The payload order is **O-I-L-R** so that the most frequent I/O matching data
resides in the lowest byte offsets.

### S14.2 Cache Behavior

- Header parsing costs three byte loads plus simple arithmetic.
- I/O matching usually stays within cache line 0 because O/I groups are packed
  immediately after the header.
- L/R matching reads deeper into the TileOp, typically touching late group
  bytes and h1 bytes.
- The old split "all groups in line 0, h1 in line 1" no longer applies.

### S14.3 Octant Stitching and I/O h1

TileOp v2 still omits I/O h1 storage. Any stitching path that genuinely needs
h1 on a geometrically I/O face must either:

1. Express the stitch as an L/R match on the relevant reflected geometry, or
2. Recompute the needed h1 values for those boundary tiles.

No spare-byte escape hatch exists in TileOp v2.

---

## S15. Open Questions

### Q1. Octant Stitching Detail

The exact face-mapping and delta_h formulas for stitching across y = x are
outlined in S8.3 but not fully derived. The implementation must resolve:

- Which tiles in our grid are their own reflections (tiles centered on y = x)?
- How does the axis swap affect h1 computation (x-offset becomes y-offset)?
- Can the stitching be expressed as additional match_lr calls with a
  transformed delta_h, or does it require a dedicated stitching pass?

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

At R = 830M, the full octant has J ~ 2.3M towers and 73M tiles. If memory
permits processing the full octant at once (11.8 GB), no batching is needed.
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

4. **Row validity:** all computed neighbor rows r' are in [0, 32). (Enforced
   by the bounds checks in S6.4.)

5. **Port count agreement on aligned faces:** for tiles (j, r) and (j, r+1),
   the number of nonzero groups on (j, r)'s O-face equals the number on
   (j, r+1)'s I-face. (Guaranteed by tile_spec determinism: both kernels
   see the same primes in the shared collar.)

6. **No false moats from dead tiles:** every dead tile has the empty TileOp v2
   header state (tile_spec S4.3).
   The compositor never matches a dead tile's ports.

7. **Spanning monotonicity within a band:** not assumed. The spanning check
   examines the full UF after all matching is complete.

---

## S17. Glossary

| Term | Definition |
|------|------------|
| **Tower** | A vertical column of 32 tiles, addressed by tower index j. |
| **Row** | A tile's position within its tower, r in [0, 31]. Row 0 is innermost. |
| **Delta** | The vertical offset delta[j] = base_y[j] - base_y[j+1] between adjacent towers. |
| **Primary neighbor** | The main overlapping tile in the adjacent tower (overlap = S - f). |
| **Secondary neighbor** | The secondary overlapping tile (overlap = f). Exists only when f > 0. |
| **Alive tile** | A tile that is neither dead nor overflow — has at least one nonzero group slot. |
| **Dead tile** | A tile entirely below y = x, all group slots zero. |
| **Spanning** | A connected component of ports touches both the inner and outer boundary. |
| **Moat** | No connected component spans the annulus — a gap in Gaussian prime connectivity. |
| **Octant stitching** | Connecting ports across the y = x diagonal between the working octant and its reflection. |
