# Compositor Logic Reference

Single source of truth for the tiles-compositor. Self-contained: a worker reading
only this document can understand and audit the entire compositor pipeline.

Source files (all paths relative to `tiles-compositor/`):

| File | Role |
|------|------|
| `include/types.h` | Constants, enums, Grid/ExtendedTileSideTable/CompositorResult structs |
| `include/grid.h` | Grid computation, tile_index(), is_tile_dead() |
| `src/grid.cpp` | compute_grid (from R), compute_grid_from_coords (from binary) |
| `include/tileop_parse.h` | TileOpCounts, FaceSlice, decode helpers |
| `src/tileop_parse.cpp` | is_overflow, is_dead, parse_counts, max_group_label, face_slice |
| `include/compositor.h` | Compositor class declaration |
| `src/compositor.cpp` | All compositor logic (500 lines) |
| `src/campaign.cpp` | Binary reader + campaign runner (main) |
| `test/test_compositor.cpp` | Synthetic unit tests |

---

## 1. Coordinate System

The Gaussian integer lattice Z[i] is mapped to integer pixel coordinates:

```
  a = Re(z)   horizontal axis, increases rightward
  b = Im(z)   vertical axis, increases upward
```

The first octant of the quarter-disk is the region `a >= b >= 0` within
radius R of the origin. The compositor operates on this octant, tiled into
a grid of 256x256 pixel squares.

```
  b (imaginary)
  ^
  |     .  quarter-circle r = R
  |   .
  |  .   first octant:
  | .    a >= b >= 0
  |.________________________> a (real)
```

Tiles are indexed by `(a_lo, b_lo)` where `a_lo` and `b_lo` are the
minimum coordinates of the tile (bottom-left corner in standard orientation).

---

## 2. Grid Structure

### Towers

The first octant is divided into vertical **towers** indexed by `j`,
going rightward from the imaginary axis. Tower `j` covers the horizontal
(a-axis) strip:

```
  a in [j * S, (j+1) * S)     where S = TILE_SIDE = 256
```

### Tile stacking within a tower

Each tower contains `TILES_PER_TOWER = 32` tiles stacked vertically (upward
in b). Tile row `r` within tower `j` covers:

```
  a in [j*S, (j+1)*S)
  b in [base_y[j] + r*S,  base_y[j] + (r+1)*S)
```

- Row 0 = bottom tile (lowest b, closest to the a-axis = innermost).
- Row 31 = top tile (highest b, farthest from a-axis = outermost).

The constant `RADIAL_DEPTH = 8192 = 32 * 256` is the total b-range of a tower.

### base_y and delta

`base_y[j]` is the b-coordinate of the bottom edge of tile row 0 in tower `j`.
It is computed from the quarter-circle geometry:

```
  base_y[j] = floor(sqrt(R^2 - (j*S)^2) / S) * S
```

Reference: `grid.cpp:42-73` (compute_grid). The `floor(.../ S) * S` alignment
ensures tile edges land on S-multiples.

Because the quarter-circle curves inward as `a` increases, `base_y` decreases
with `j`:

```
  base_y[0] >= base_y[1] >= base_y[2] >= ...
```

The step-down between adjacent towers is:

```
  delta[j] = base_y[j] - base_y[j+1]       (>= 0 always)
```

Reference: `grid.cpp:25-38` (finalize_deltas).

```
  Tower j      Tower j+1
  +------+
  |  r=3 |     +------+
  |  r=2 |     |  r=3 |      <- base_y[j] > base_y[j+1]
  |  r=1 |     |  r=2 |         so tower j+1 starts lower
  |  r=0 |     |  r=1 |
  +------+     |  r=0 |
               +------+
      ^            ^
   base_y[j]   base_y[j+1]
```

`delta[j]` decomposes into whole-tile rows and a fractional pixel offset:

```
  q = delta[j] / S       (whole tile rows of shift)
  f = delta[j] % S       (fractional pixel shift, 0 <= f < S)
```

### Dead tiles

A tile is "dead" (below the diagonal `a = b`) when its b-range lies entirely
below the a-range of the tower:

```
  is_tile_dead(grid, j, r) :=
      (base_y[j] - r * S) <= (j+1) * S
```

Reference: `grid.h:14-16`. Dead tiles contain no primes and are skipped
throughout the compositor.

### Flat indexing

Tiles are flat-indexed as `flat_idx = j * tiles_per_tower + r`:

```
  tile_index(grid, j, r) = j * grid.tiles_per_tower + r
```

Reference: `grid.h:11-13`.

---

## 3. Face Convention

Each tile has four faces corresponding to its four edges:

```
  +------ FACE_O (top, high b) ------+
  |                                   |
  FACE_L (left, low a)    FACE_R (right, high a)
  |                                   |
  +------ FACE_I (bottom, low b) ----+
```

| Enum | Value | Edge | Direction | h1 stored? |
|------|-------|------|-----------|------------|
| FACE_I | 0 | bottom (low b) | within-tower | No |
| FACE_O | 1 | top (high b) | within-tower | No |
| FACE_L | 2 | left (low a) | between-tower | Yes |
| FACE_R | 3 | right (high a) | between-tower | Yes |

Reference: `types.h:19-24`.

### Within-tower faces (I, O)

FACE_I and FACE_O are the horizontal edges shared by vertically adjacent
tiles in the same tower. When tile[r].FACE_O meets tile[r+1].FACE_I, they
share the exact same a-range and the exact same boundary b-value. No
positional disambiguation is needed -- ports match by slot position.
Therefore no h1 is stored.

### Between-tower faces (L, R)

FACE_L and FACE_R are the vertical edges shared by horizontally adjacent
towers. When tower[j-1].FACE_R meets tower[j].FACE_L, the tiles may be
at different b-offsets (due to `delta[j-1]`). The `h1` value disambiguates
which ports share the same absolute b-coordinate. h1 is the **tile-local
b-offset from the tile's bottom edge**:

```
  h1 = b - b_lo        (for a port on L or R face)
     = tile_row         (b-offset along the vertical edge)
  range: [0, S-1] = [0, 255]
```

Within the binary format, h1 uses 9 bits total (see Section 4).

---

## 4. TileOP v2 Binary Format

Each tile's connectivity summary is encoded as a fixed-size binary record.
Two sizes exist:

| Variant | Total bytes | Payload bytes | Constant |
|---------|-------------|---------------|----------|
| Standard | 128 | 125 | `TILEOP_SIZE`, `TILEOP_PAYLOAD_BYTES` |
| Extended | 256 | 253 | `TILEOP_EXT_SIZE`, `TILEOP_EXT_PAYLOAD_BYTES` |

The extended variant is used when a tile's port counts exceed the standard
budget. Extended tiles are stored in `ExtendedTileSideTable` (a hash map from
`flat_idx` to a 256-byte array). Reference: `types.h:36-42`.

### Header (3 bytes)

```
  byte[0] = off_I     offset where I-face groups begin
  byte[1] = off_L     offset where L-face groups begin
  byte[2] = off_R     offset where R-face groups begin
```

All offsets are from the start of the record (byte 0). `off_I >= 3` always
(header occupies bytes 0-2).

### Payload layout

The payload is divided into six contiguous sections. Let `P` = payload budget
(125 or 253):

```
  byte[3 .. off_I-1]           O-face group bytes      (o_cnt = off_I - 3)
  byte[off_I .. off_L-1]       I-face group bytes      (i_cnt = off_L - off_I)
  byte[off_L .. off_R-1]       L-face group bytes      (l_cnt = off_R - off_L)
  byte[off_R .. off_R+r_cnt-1] R-face group bytes
  byte[h_start .. h_start+l_cnt-1]    L-face h1 bytes
  byte[h_start+l_cnt .. h_start+l_cnt+r_cnt-1]  R-face h1 bytes
```

where:

```
  r_cnt = (P - o_cnt - i_cnt - 2*l_cnt) / 2
  h_start = off_R + r_cnt
```

Reference: `tileop_parse.cpp:16-63` (parse_counts).

The layout is self-describing from the 3-byte header: `o_cnt`, `i_cnt`, and
`l_cnt` are derived from the offset differences. `r_cnt` is derived from the
remaining budget, which must be evenly split between R-face groups and
L+R h1 bytes.

### Group byte encoding (7-bit + h1 bit steal)

For I and O faces, the group byte is a plain 1-byte group label (1-indexed,
0 = padding/empty).

For L and R faces, the group byte encodes both the group label and the high
bit of h1:

```
  group_byte = (group_label & 0x7F) | ((h1 >> 8) << 7)
```

- Bits [6:0] = group label (7 bits, max 127)
- Bit [7] = high bit of h1 (bit 8 of the 9-bit h1)

The full 9-bit h1 is reconstructed from the group byte and the separate
h1 byte:

```
  decode_group_id(group_byte) = group_byte & 0x7F
  decode_h1(group_byte, h1_byte) = ((group_byte >> 7) << 8) | h1_byte
```

Reference: `tileop_parse.h:30-37`.

h1 range = [0, 511]. Since `S = 256`, valid h1 values are [0, 255], but the
encoding supports up to 511 for safety.

### FaceSlice accessor

`face_slice(tile, face, payload_budget)` returns a `FaceSlice{groups, h1_bytes, count}`:

- FACE_I: groups at `tile + off_I`, no h1, count = `i_cnt`
- FACE_O: groups at `tile + 3`, no h1, count = `o_cnt`
- FACE_L: groups at `tile + off_L`, h1 at `tile + h_start`, count = `l_cnt`
- FACE_R: groups at `tile + off_R`, h1 at `tile + h_start + l_cnt`, count = `r_cnt`

Reference: `tileop_parse.cpp:104-122`.

### Sentinel values

- `tile[0] == 0xFF` (OVERFLOW_SENTINEL): tile has too many ports, compositor aborts.
- `tile[0..2] == 3, 3, 3` and `tile[3] == 0` (EMPTY_OFFSET pattern): tile is dead (no groups).

Reference: `tileop_parse.cpp:6-14`.

---

## 5. Within-Tower Matching (match_io_within_tower)

Reference: `compositor.cpp:164-206`.

Within a single tower, vertically adjacent tiles share a horizontal boundary.
The top face (FACE_O) of tile at row `r` connects to the bottom face (FACE_I)
of the tile at row `r+1`.

```
  Tower j, row r+1:   +--------+
                       | FACE_I |  <-- bottom of row r+1
                       +--------+
                       | FACE_O |  <-- top of row r
  Tower j, row r:      +--------+
```

**Matching rule:** Slot-by-slot positional pairing up to `min(o_cnt, i_cnt)`.

```
  for s in 0 .. min(o_cnt, i_cnt) - 1:
      if O_groups[s] > 0 and I_groups[s] > 0:
          unite(global_id(flat_top, O_groups[s]),
                global_id(flat_bottom, I_groups[s]))
```

**Why no h1:** Both tiles share the exact same boundary row (same a-range,
same b-value at the shared edge). The port lists are ordered by position
along that shared edge, so port `s` on the O-face and port `s` on the I-face
refer to the same cluster of boundary primes. This is the "shared-prime
identity" guarantee from the TileOp encoder.

**Why min-count:** The encoder may have different counts on each side due to
pruning or dead-end elimination. Ports beyond `min(o_cnt, i_cnt)` have no
matching partner.

---

## 6. Between-Tower Matching (match_lr_with_previous)

Reference: `compositor.cpp:216-329`.

When tower `j` is ingested, it must be matched against tower `j-1` (whose
tile data was saved in `prev_tower_tiles_` during the previous ingestion).
The shared boundary is vertical: FACE_R of tower `j-1` faces FACE_L of
tower `j`.

### Row mapping derivation

Let `d = delta[j-1] = base_y[j-1] - base_y[j]`. Decompose:

```
  q = d / S       (whole tile rows)
  f = d % S       (fractional pixel offset)
```

Consider a tile at row `r` in tower `j`. Its b-range is:

```
  b in [base_y[j] + r*S,  base_y[j] + (r+1)*S)
```

A tile at row `p` in tower `j-1` has b-range:

```
  b in [base_y[j-1] + p*S,  base_y[j-1] + (p+1)*S)
```

For overlap in b, we need:

```
  base_y[j] + r*S  <  base_y[j-1] + (p+1)*S
  base_y[j-1] + p*S  <  base_y[j] + (r+1)*S
```

Substituting `base_y[j-1] = base_y[j] + d = base_y[j] + q*S + f`:

```
  From first:   r*S < (q*S + f) + (p+1)*S   =>   r < q + p + 1 + f/S
  From second:  (q*S + f) + p*S < (r+1)*S   =>   q + p + f/S < r + 1
```

So: `q + p - 1 + f/S < r < q + p + 1 + f/S`, i.e., `p` ranges over
`{r - q, r - q - 1}` when `f > 0`, or just `{r - q}` when `f = 0`.

**Primary match row:** `primary_prev_row = r - q`

Since `base_y[j-1] >= base_y[j]`, the previous tower starts HIGHER (larger
base_y). At the same absolute b-coordinate, the previous tower has a LOWER
row index. So current row `r` maps to previous row `r - q`.

Rows `0 .. q-1` in the current tower have `primary_prev_row < 0` and thus
have **no matching tile** in the previous tower -- their L-face is exposed
(inner boundary).

**Secondary match row:** `secondary_prev_row = r - q - 1` (only when `f > 0`)

When the fractional offset `f` is nonzero, the current tile also overlaps
with one row below the primary in the previous tower.

### h1 match predicates

#### Primary predicate: `hl == hr + f`

Consider a Gaussian prime at absolute coordinate `b_abs` on the shared
vertical boundary (the line `a = j * S`). Its h1 in each tile:

```
  In current tile (tower j, row r):
      hl = b_abs - (base_y[j] + r*S)
         = b_abs - base_y[j] - r*S

  In previous tile (tower j-1, row r-q):
      hr = b_abs - (base_y[j-1] + (r-q)*S)
         = b_abs - (base_y[j] + d) - (r-q)*S
         = b_abs - base_y[j] - d - r*S + q*S
         = (b_abs - base_y[j] - r*S) - d + q*S
         = hl - (q*S + f) + q*S
         = hl - f
```

Therefore: **`hl = hr + f`**

Reference: `compositor.cpp:280-281`.

#### Secondary predicate: `hl + (S - f) == hr`

For the secondary match (previous row `r - q - 1`):

```
  In previous tile (tower j-1, row r-q-1):
      hr = b_abs - (base_y[j-1] + (r-q-1)*S)
         = b_abs - (base_y[j] + d) - (r-q-1)*S
         = (b_abs - base_y[j] - r*S) - d + q*S + S
         = hl - f + S
```

Therefore: **`hl + (S - f) = hr`**, equivalently `hr - hl = S - f`.

Reference: `compositor.cpp:319`.

### Matching loop structure

For each row `r` in current tower `j`:

1. Get L-face slice of current tile (tower j, row r).
2. **Primary:** If `r - q` is a valid row in tower `j-1`:
   - Get R-face slice of previous tile (tower j-1, row r-q).
   - For all `(li, ri)` pairs: if `decode_h1(L[li]) == decode_h1(R[ri]) + f`, unite.
3. **Secondary** (only if `f > 0`): If `r - q - 1` is a valid row in tower `j-1`:
   - Get R-face slice of previous tile (tower j-1, row r-q-1).
   - For all `(li, ri)` pairs: if `decode_h1(L[li]) + (S - f) == decode_h1(R[ri])`, unite.

The O(L * R) nested loop is acceptable because port counts are small (typically
< 10 per face, encoded in 125-byte budget).

### ASCII diagram of between-tower geometry

```
  Previous tower (j-1)          Current tower (j)
  base_y[j-1] = base_y[j] + d

         +------+
    r=3  |      |                    +------+
         +------+ R-face        r=3  |      | L-face
    r=2  |      |  |                 +------+  |
         +------+  |            r=2  |      |  |
    r=1  |      |  |   matched       +------+  |
         +------+  v   <----->  r=1  |      |  v
    r=0  |      |                    +------+
         +------+               r=0  |      |   <- rows 0..q-1 have
                                     +------+      no match (exposed)

  With q=1, f=0: current row r maps to previous row r-1.
  Current row 0 has no match in previous tower.
```

---

## 7. Boundary Collection

Reference: `compositor.cpp:332-498`.

The compositor determines SPANNING vs MOAT by checking whether any
**inner boundary** group shares a Union-Find root with any **outer boundary**
group.

### Inner boundary

"Inner" = closest to the origin (low b, low a diagonal). Collected per tower
in `collect_inner_boundary`:

1. **FACE_I on row 0** of every tower (bottom edge of bottom tile).
   All non-zero groups on this face are inner members.
   Reference: `compositor.cpp:334-348`.

2. **FACE_L on exposed rows** of current tower -- rows that have no matching
   tile in the previous tower because the previous tower starts higher.

   Let `d_prev = delta[j-1]`, `q_prev = d_prev / S`, `f_prev = d_prev % S`.

   - **Whole exposed rows:** rows `0 .. q_prev - 1`. ALL L-face ports on these
     rows are inner boundary (their entire L-face is exposed).
     Reference: `compositor.cpp:358-386`.

   - **Fractional exposed row:** row `q_prev` when `f_prev > 0`. Only L-face
     ports with `h1 < f_prev` are inner boundary (the bottom `f_prev` pixels
     of the L-face are below the previous tower's bottom edge).
     Reference: `compositor.cpp:388-411`.

### Outer boundary

"Outer" = farthest from the origin (high b, or high a past the diagonal).
Collected per tower in `collect_outer_boundary`:

1. **FACE_O on row 31** of every tower (top edge of top tile).
   All non-zero groups on this face are outer members.
   Reference: `compositor.cpp:416-431`.

2. **FACE_R on exposed rows** -- rows of the current tower whose R-face
   has no matching tile in the NEXT tower (tower `j+1`), because the next
   tower starts lower.

   Let `d = delta[j]`, `q = d / S`, `f = d % S`.

   - **Whole exposed rows:** rows `T-q .. T-1` (where `T = TILES_PER_TOWER = 32`).
     ALL R-face ports on these rows are outer boundary.
     Reference: `compositor.cpp:441-472`.

   - **Fractional exposed row:** row `T-1-q` when `f > 0`. Only R-face ports
     with `h1 >= S - f` are outer boundary (the top `f` pixels of the R-face
     are above the next tower's top edge).
     Reference: `compositor.cpp:474-498`.

### Symmetry of inner/outer boundary logic

The inner boundary collects from the LEFT face (facing the previous, taller
tower) at the BOTTOM of the current tower. The outer boundary collects from
the RIGHT face (facing the next, shorter tower) at the TOP of the current
tower. The fractional predicates are symmetric:

```
  Inner fractional: h1 < f_prev        (below previous tower's reach)
  Outer fractional: h1 >= S - f        (above next tower's reach)
```

---

## 8. Union-Find and Verdict

### Global group indexing

Each tile's groups are 1-indexed labels (1 through `max_label`). The
compositor maps them to a global ID space via `group_offset_`:

```
  group_offset_[flat_idx]     = first global ID for tile flat_idx
  group_offset_[flat_idx + 1] = first global ID for next tile
  global_id(flat_idx, label)  = group_offset_[flat_idx] + label - 1
```

Reference: `compositor.h:35-37`, `compositor.cpp:134-162`.

The `parent_` vector is grown as towers are ingested. Each group starts as
its own root: `parent_[id] = id`.

### Union-Find operations

**find** uses path-halving (not full path compression):

```
  find(x):
      while parent_[x] != x:
          parent_[x] = parent_[parent_[x]]    // path halving
          x = parent_[x]
      return x
```

Reference: `compositor.cpp:96-102`.

**unite** is deterministic -- always points higher ID toward lower ID:

```
  unite(a, b):
      ra = find(a), rb = find(b)
      if ra == rb: return
      if ra > rb: swap(ra, rb)
      parent_[rb] = ra
```

Reference: `compositor.cpp:104-116`.

This is a deliberate design choice: deterministic (not rank/size-balanced)
ensures reproducible results across runs. The unite-toward-lower-ID rule
means earlier-ingested groups tend to become roots.

### Pre-flattening

After within-tower matching, `pre_flatten_tower` calls `find()` on every
group ID in the tower to partially flatten the UF tree. This improves
locality for the subsequent between-tower matching step.

Reference: `compositor.cpp:208-214`.

### OOM cap

If `parent_.size()` exceeds 2 billion entries, the compositor aborts with
a diagnostic message. Reference: `compositor.cpp:154-161`.

### Verdict

**has_spanning():** Collects all UF roots of inner members into a set, then
checks if any outer member's root is in that set. Returns true on first match.
Reference: `compositor.cpp:51-65`.

**finalize():** Same logic but also counts distinct inner roots and outer roots
for the result struct. Reference: `compositor.cpp:67-94`.

```
  SPANNING = exists (inner_member, outer_member) such that
             find(inner_member) == find(outer_member)

  MOAT     = no such pair exists
```

---

## 9. Campaign Runner

Reference: `src/campaign.cpp`.

The campaign runner is the `main()` entry point that reads a binary tile dump
and feeds towers sequentially into the compositor.

### Binary file format

```
  Offset 0:                 uint32_le  n_tiles (total tiles in file)
  Offset 4 + tower*T_bytes: T_bytes    tower data (32 records)
```

Each tile record is `kRecordBytes = 148` bytes:

```
  bytes [0..7]:    int64_le  a_lo (tile origin x)
  bytes [8..15]:   int64_le  b_lo (tile origin y)
  bytes [16..19]:  (unused/metadata)
  bytes [20..147]: 128 bytes = TileOp (TILEOP_SIZE)
```

Tower size: `kTowerBytes = 32 * 148 = 4736` bytes.

### Processing pipeline

1. **Read header:** Extract `n_tiles`. Verify it's a multiple of 32.

2. **First pass -- coordinate extraction:** Read the first record of each
   tower to extract `(a_lo, b_lo)`. Build `base_y[]` via
   `compute_grid_from_coords`.

3. **Sequential tower ingestion:** For each tower 0 through `towers_to_process - 1`:
   - Read all 32 records (4736 bytes).
   - Extract 128-byte TileOp from each record (offset 20).
   - Count groups per tower (for progress reporting).
   - Call `compositor.ingest_tower(j, tower_tileops)`.

4. **Finalize:** Call `compositor.finalize()`, print verdict + statistics.

### CLI interface

```
  ./campaign <binary_path> <R_value> [--max-towers N] [--progress-interval N]
```

- `--max-towers N`: Process only the first N towers (for incremental testing).
- `--progress-interval N`: Print progress every N towers (default: 1000).

### Progress reporting

Every `progress_interval` towers (and at the final tower), prints:

```
  tower T/Total (P%): groups=G, total_groups=TG, avg=A/tile
```

Final output includes verdict, total_groups, inner/outer root counts, wall
time, peak RSS, and average groups per tile.

---

## 10. Bug History

### Bug 1: The face swap bug

**Commit:** `44cd16c` ("Fix compositor face matching semantics")

**What happened:** The initial compositor implementation (before it was
extracted into `tiles-compositor/`) used FACE_R and FACE_L for within-tower
matching and FACE_O and FACE_I for between-tower matching -- the exact
reverse of the correct assignment.

**Root cause:** The `constants.h` file in `tiles-maxxing/tile-cpp/` had
misleading comments:

```
  // tiles-maxxing/tile-cpp/include/constants.h (before fix)
  constexpr int FACE_I = 0;  // inner (top)      <- WRONG: I = bottom
  constexpr int FACE_O = 1;  // outer (bottom)   <- WRONG: O = top
```

The comments labeled FACE_I as "top" and FACE_O as "bottom", inverting the
actual convention. The label "inner" was interpreted as "inner edge of the
tile" (top, toward the interior of the octant) rather than the correct
meaning "inner boundary of the annulus" (bottom, toward the origin).

This was discovered during a validator alignment audit. Reference:
`tiles-maxxing/docs/supportive/2026-04-09-validator-alignment-plan.md:103-108`.
The dispatch note at `tiles-maxxing/.dispatch-cpp-257.md:30` explicitly calls
out the fix.

**Impact:** Within-tower matching was comparing the wrong faces (L/R instead
of O/I), which would produce incorrect unions. Between-tower matching was
comparing O/I instead of L/R, missing the h1-based positional alignment
entirely. The compositor would produce spurious SPANNING or MOAT verdicts.

**Fix:** The compositor was rewritten with correct face assignments:
- Within-tower: FACE_O (top of row r) matches FACE_I (bottom of row r+1)
- Between-tower: FACE_R (right of tower j-1) matches FACE_L (left of tower j)

The `constants.h` comments were also corrected:

```
  constexpr int FACE_I = 0;  // inner (bottom)
  constexpr int FACE_O = 1;  // outer (top)
```

### Bug 2: The row index bug

**Commit:** `0e9d9a7` ("fix: correct between-tower row index in match_lr_with_previous")

**What happened:** The between-tower matching used `row + q` instead of
`row - q` for the primary previous-tower row, and `row + q + 1` instead of
`row - q - 1` for the secondary.

**The erroneous code:**

```cpp
  // WRONG (before fix):
  const int primary_prev_row = row + q;
  // ...
  const int secondary_prev_row = primary_prev_row + 1;
```

**The correct code:**

```cpp
  // CORRECT (after fix):
  const int primary_prev_row = row - q;
  // ...
  const int secondary_prev_row = primary_prev_row - 1;
```

**Root cause:** Sign confusion in the row correspondence derivation. The
pre-fix comment stated:

```
  // Current row 'row' overlaps previous row 'row + q' (primary) and
  // 'row + q + 1' (secondary, when f > 0) in b-range.
```

This would be correct if `delta` measured how much LOWER the previous tower
starts (i.e., `base_y[j] - base_y[j-1]`). But `delta[j-1] = base_y[j-1] - base_y[j]`,
meaning the previous tower starts HIGHER. Since tiles stack upward (row 0 =
bottom), the same absolute b-value corresponds to a LOWER row index in the
taller previous tower: `prev_row = row - q`, not `row + q`.

**Impact:** The matcher was looking at completely wrong rows in the previous
tower. Rows near the bottom of the current tower (low `r`) would try to
access `r + q` which could be a very different region. Rows near the top
could exceed `TILES_PER_TOWER` and be skipped entirely. The h1 predicates
were correct, but applied to the wrong tiles, so matches were essentially
random.

**How found:** Campaign run on a 75M-tile octant (R=850M) produced clearly
wrong results. After the fix: SPANNING, 733M groups, 10.2M inner/outer roots,
56s wall time, 5019 MB peak RSS.

**Note:** The boundary collectors (`collect_inner_boundary`,
`collect_outer_boundary`) were already using the correct `r - q` logic --
they pre-dated the compositor's matching code and were written from the
correct geometric derivation. This inconsistency (collectors correct,
matcher wrong) was a strong signal that the matcher had a sign error.

---

## 11. Known Issues and Invariants

### decode_group_id on I/O faces (latent bug, unfixed as of 2026-04-12)

`decode_group_id(group_byte)` applies `& 0x7F`, masking off bit 7. This is
correct for L/R face groups where bit 7 is stolen for h1 encoding (the
9-bit h1 scheme: `h1 = ((group_byte >> 7) << 8) | h1_byte`).

**But I/O face groups do NOT use h1 bit-steal.** Their group bytes are plain
8-bit labels (values 0-255). Applying `decode_group_id` to I/O face groups
silently truncates any label >= 128 to its low 7 bits.

**Where it matters:**
- `collect_inner_boundary` (compositor.cpp:342) applies `decode_group_id` to FACE_I groups
- `collect_outer_boundary` (compositor.cpp:425) applies `decode_group_id` to FACE_O groups
- `match_io_within_tower` (compositor.cpp:199-200) correctly uses the raw byte directly

**Current impact:** None at operating scale. Group counts per tile are below
128 at R=850M (census max: 21 groups). But at larger radii or denser k^2,
group counts can exceed 127, and the bug will silently corrupt boundary
collection — registering the wrong group as a boundary member.

**Fix:** Replace `decode_group_id(slice.groups[i])` with `slice.groups[i]`
for FACE_I and FACE_O in the boundary collectors. Or introduce a separate
`io_group_label()` helper that returns the raw byte.

**Rule: `decode_group_id` is ONLY for L/R faces. Never apply it to I/O faces.**
This must be enforced in code review for any future compositor changes.
