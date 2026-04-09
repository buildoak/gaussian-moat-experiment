# Tile Specification v5 — TileOp v2 Dynamic Packed Layout

> Status: **draft** (v5: TileOp v2 dynamic packed layout, 2026-04-09)
> Supersedes: v4 (fixed-size layout + sentinel overflow, 2026-04-08), v3 (two-tier variable-length, 2026-04-08), v2 (full fingerprint, 2026-04-07), `research/2026-03-25-tile-algorithm-spec.md` (distance-based)

## 1. Overview

The Gaussian moat search decomposes an annular band of the Gaussian integer
lattice into axis-aligned square tiles. Each tile is processed by a CUDA kernel
that emits a fixed-size **TileOp** (128 bytes) — a transfer operator describing
how the tile's boundary ports connect through its interior. TileOp v2 retains
the fixed 128-byte footprint but replaces the old fixed-16-per-face layout with
a **3-byte offset header + dynamic packed sections**. Face O and Face I group
labels are packed first for fast within-tower matching; Face L and Face R group
labels and their h1 bytes occupy the higher offsets. A Rust compositor parses
the header, derives face counts, and uses union-find to check whether any path
spans from the inner to outer boundary.

Key properties:
- **Fixed 128-byte TileOp** — exactly 2 cache lines, direct index addressing
- **Dynamic packed face storage** — no fixed per-face slot cap; capacity comes from the 125-byte data budget
- **All-integer arithmetic** — no floats in matching or composition
- **Zero-allocation composition** — stack-only, no heap in the hot loop
- **Self-contained TileOps** — raw tile data is discarded after kernel emit
- **Deterministic** — same tile always produces the same TileOp
- **Unconditionally correct** — overflow sentinel preserves search correctness

## 2. Tile Geometry

```
S = 256            // tile side length (lattice units, 256 segments)
k = 40             // step parameter (moat step = sqrt(k) ~ 6.32)
collar = 7         // perpendicular depth into tile from each face (1..7 lattice units)
halo = (S + 1) + 2*7 // CUDA kernel processes 271 x 271 lattice points per tile
                      // = 257 tile-proper points + 14 collar points
```

Tiles are axis-aligned squares of side `S = 256` lattice units, so tile proper
contains `(S + 1) x (S + 1) = 257 x 257` lattice points with tile-relative
coordinates in the closed box `[0, S] x [0, S]`. Adjacent tiles share the
boundary row/column at offset `S`; tile A at origin `a_lo` covers
`[a_lo, a_lo + S]`, while its neighbor at `a_lo + S` covers
`[a_lo + S, a_lo + 2S]`. Both CUDA kernels process the same shared-boundary
primes and the same 7-deep collar around that boundary, which is what makes
shared-prime identity matching work. (updated 2026-04-09: 257x257 shared boundary convention)

**Why S=256:** clean alignment with 256 threads per CUDA block and convenient
tile-origin stride. Under the 257x257 shared-boundary convention, the old
"h-offsets fit in u8" argument is no longer literally true for L/R faces
because offset 256 is representable geometrically but not in `u8`; that
packing consequence is now an explicit open issue rather than an implicit
assumption. (updated 2026-04-09: 257x257 shared boundary convention) Yields
~6,300 primes per tile at origin density, far fewer (~1,500) at 850M radius.

**Why collar=7:** two Gaussian primes connect iff their squared Euclidean
distance <= k=40. For primes on the same face (separated only along the face
axis), the maximum connecting offset is floor(sqrt(40))=6. The collar must
reach deep enough that every prime within connection range of the face is
included: collar = ceil(sqrt(k)) = ceil(6.32) = 7.

### 2.1 Grid Descriptor

Tiles are addressed by array index. Position is derived from the index and a
per-band grid descriptor:

```rust
struct GridDesc {
    base_x: i64,    // lattice x of tile (0,0)'s origin corner (min-x, min-y)
    base_y: i64,    // lattice y of tile (0,0)'s origin corner
    width:  u32,    // tiles in x direction
    height: u32,    // tiles in y direction (32 for 8192-unit radial depth)
}
```

**Origin corner convention:** each tile's origin is its (min-x, min-y) corner.
h1 offsets are measured from this corner along the face axis.

```
tile_x = grid.base_x + (idx % grid.width) as i64 * 256
tile_y = grid.base_y + (idx / grid.width) as i64 * 256
right_neighbor  = idx + 1           (if same row)
top_neighbor    = idx + grid.width
```

**Face convention:** the grid y-axis is radial (row 0 = innermost ring of
band). Logical faces map to geometric edges as follows:

| Face | Edge | Shared with neighbor |
|------|------|----------------------|
| I (Inner) | bottom edge (y = tile_y) | `bottom_neighbor = idx - grid.width` |
| O (Outer) | top edge (y = tile_y + S) | `top_neighbor = idx + grid.width` |
| L (Left) | left edge (x = tile_x) | `left_neighbor = idx - 1` |
| R (Right) | right edge (x = tile_x + S) | `right_neighbor = idx + 1` |

Face O of tile i and Face I of its top neighbor extract from the same shared
boundary row `y = tile_y + S`. Face R of tile i and Face L of its right
neighbor extract from the same shared boundary column `x = tile_x + S`.
(updated 2026-04-09: 257x257 shared boundary convention)

One GridDesc per search band. Passed to both the CUDA kernel (block -> lattice
region) and the Rust compositor (index -> adjacency). Tiles outside the annular
band exist in the array but are dead (all-zero TileOp — no ports on any
face, detected via S4.3 dead-tile check).

## 3. Port Definition

A **port** is a maximal connected cluster of Gaussian primes on a tile face,
where two consecutive face primes (ordered by along-face position) belong to
the same port iff their squared Euclidean distance <= k (including both
along-face and depth coordinates). (Resolved 2026-04-09: the 1D approximation
"along-face separation <= 6" is superseded by the full distance^2 <= k rule,
consistent with tile_operations.md S7.2. Face primes span up to collar=7
depth, so the perpendicular component matters.) Ports are determined by face
geometry alone (independent of tile interior).

Each port is anchored by its minimum-offset prime along the face. The anchor
position, measured as an offset in lattice units from the tile's origin corner
(see S2.1: corner at (tile_x, tile_y)), is the port's **h1** value (u8,
range 0..256; values 0 and 256 both occur on shared tile boundaries under the
257-point convention). h1 uniquely identifies a port on its face. (updated 2026-04-09: 257x257 shared boundary convention)

**Asymmetric encoding (see S4.1):** I/O ports cost 1 byte each (group label
only). L/R ports cost 2 bytes each (group label + h1). TileOp v2 packs these
dynamically: O groups first, then I groups, then L groups, then R groups, then
L h1, then R h1. I/O faces do not store h1 because adjacent vertical tiles
share the exact same boundary row, so shared-prime identity matching is exact
from the duplicated boundary prime set and positional order is only an
extraction convenience, not the semantic matching rule. (updated 2026-04-09:
257x257 shared boundary convention)

### 3.1 Parity Property

Consider a horizontal face at fixed imaginary coordinate R. A Gaussian
integer n + Ri is a Gaussian prime (up to units) iff its norm n^2 + R^2 is a
rational prime congruent to 1 mod 4, or equals 2. (Primes p = 3 mod 4 do not
split in Z[i] and contribute no Gaussian primes of this form.) For n^2 + R^2
to be odd (necessary for primality when > 2), n and R must have opposite
parity. Thus face primes occupy **exactly one parity class** of along-face
positions — at most 128 candidates out of 256. Vertical faces are analogous
with the roles of real and imaginary parts swapped.

Consequences:
- All gaps between consecutive face primes are **even**: 2, 4, 6, 8, ...
- Intra-port gaps (primes within same port): 2, 4, or 6 lattice units
- **Minimum port-splitting gap: 8** (not 7) — next even value after 6
- Upper bound on ports per face: at most 128 primes in one parity class, with
  minimum inter-port gap of 8 (= 4 parity-adjusted positions). Worst case:
  alternating single-prime ports and minimal gaps, giving floor(128 / 4) = 32.
  (Never approached in practice.)

### 3.2 TileOp v2 Data Budget

TileOp v2 removes the old hard cap of 16 ports per face. Capacity is now
governed by the packed-data budget:

```
o_cnt + i_cnt + 2*l_cnt + 2*r_cnt <= 125
```

where I/O ports cost 1 byte each and L/R ports cost 2 bytes each. This gives:

- **Uniform max:** `6*N <= 125`, so `N = 20` ports on every face fits.
- **Asymmetric headroom:** a high-port face can borrow bytes from sparse faces.
- **Empirical motivation:** the 100K-tile census at `R ~= 860M` found 33.16%
  overflow under the old fixed-16-per-face layout, with observed maxima up to
  22 face ports. TileOp v2 is designed to absorb that skew.

Observed operating-point data fits this budget for 99.5%+ of tiles. The
overflow sentinel (S4.6) remains the unconditional safety net for the residue.

## 4. TileOp Structure

### 4.1 Dynamic Packed Layout (128 bytes = 2 cache lines)

TileOp v2 is a fixed-size record with a 3-byte header and 125 bytes of packed
payload:

```
TileOp v2 [128 bytes]

Header:
  Byte 0: off_I   // byte offset where Face I groups begin
  Byte 1: off_L   // byte offset where Face L groups begin
  Byte 2: off_R   // byte offset where Face R groups begin

Payload:
  Bytes 3 .. off_I - 1                 Face O groups   (o_cnt bytes)
  Bytes off_I .. off_L - 1             Face I groups   (i_cnt bytes)
  Bytes off_L .. off_R - 1             Face L groups   (l_cnt bytes)
  Bytes off_R .. off_R + r_cnt - 1     Face R groups   (r_cnt bytes)
  Bytes h_start .. h_start + l_cnt - 1 Face L h1       (l_cnt bytes)
  Bytes h_start + l_cnt .. 127         Face R h1       (r_cnt bytes, optional 1-byte pad at byte 127)

where:
  h_start = off_R + r_cnt
```

The compositor derives counts from the offsets:

```
o_cnt = off_I - 3
i_cnt = off_L - off_I
l_cnt = off_R - off_L
r_cnt = (125 - o_cnt - i_cnt - 2*l_cnt) >> 1
```

The floor shift handles the optional 1-byte pad when the residual budget is
odd. Byte 127 may therefore be zero padding and is never interpreted as data.

**Face ordering rationale (O-I-L-R):** Face O and Face I groups are packed
immediately after the header because within-tower I/O matching is the dominant
compositor read pattern. Putting O/I groups in the lowest byte offsets keeps
the most frequently matched data in cache line 0. L/R groups and h1 bytes are
less frequently accessed and occupy the higher offsets.

**Asymmetric semantics unchanged:** I/O faces store group labels only; L/R
faces store group labels plus h1. The rationale is unchanged from v4: I/O
matching is shared-prime identity on an aligned duplicated boundary row, while
L/R matching needs h1 for offset disambiguation.

**Port storage:** within each face section, ports are sorted by ascending h1
(for L/R) or ascending shared-face position (for I/O). There are no empty-slot
sentinels inside face sections; counts come from the header.

**Empty tile:** `off_I = off_L = off_R = 3`. No payload bytes are live.

**Addressing:** `tile_ops[tile_index * 128]` — direct, O(1), no indirection.

**CUDA emit:** each block writes 128 bytes at `blockIdx.x * 128`. No atomic
allocators, no prefix-sum prepass. Adjacent blocks write adjacent 128 B
regions — good L2 spatial locality and no false sharing between blocks.

### 4.2 Cache Behavior

Cache behavior is still two-cache-line friendly, but the hot bytes shift:

```
Cache line 0 (bytes 0-63):
  header (3 B) + Face O groups + Face I groups + early Face L/R groups

Cache line 1 (bytes 64-127):
  trailing Face L/R groups + Face L h1 + Face R h1 + optional 1-byte pad
```

| Operation                  | Data read                                  | Bytes          | Cache lines |
|---------------------------|---------------------------------------------|----------------|-------------|
| Aligned match I/O         | header + O groups / I groups                | typically 3-30 | usually 1   |
| Offset match L/R          | header + L/R groups + L/R h1                | variable       | 1 or 2      |
| Group scan for max label  | header + all group sections                 | `3+o+i+l+r`    | usually 1   |

The old "cache line 0 = all groups, cache line 1 = h1 + tail metadata" model no
longer applies. The important property is that O/I groups sit in the lowest
offsets, while L/R groups and h1 bytes are deeper in the TileOp.

### 4.3 Tile Status Detection

There is no dedicated status field in the TileOp. Tile status is derived
from the header:

**Overflow detection:** `bytes[0] == 0xFF`. In a valid TileOp v2 header,
`off_I` is a real byte offset and never approaches 255. `0xFF` is therefore an
unambiguous poison sentinel. Overflow still means all 128 bytes are filled with
`0xFF`.

**Dead-tile detection:** `off_I == off_L == off_R == 3` and byte 3 is zero.
This is the unique empty-layout state: all face counts are zero and no payload
bytes are live.

**Compositor 3-way check:**

| Byte 0 | Condition | Meaning | Action |
|---|---|---|---|
| `0xFF` | — | Overflow (CUDA filled all 128 bytes with 0xFF) | Conservative bridge (S4.6) |
| `3` | `bytes[1] == 3 && bytes[2] == 3 && bytes[3] == 0` | Dead tile (no ports on any face) | Skip |
| other valid offset | — | Normal alive tile | Process |

### 4.4 Group Semantics

The **group** label (u8, values 1-255) encodes interior connectivity: ports
with the same group value are connected through the tile's internal prime
graph. Ports with different group values are disconnected inside this tile.
Group 0 is not stored in the packed sections.

Groups can span multiple faces — a single connected component may reach all 4
faces. After dead-end pruning, validated max at 850M: 9 surviving groups.

**Group label assignment (deterministic):** labels assigned by order of first
appearance when scanning ports face-by-face (I, O, L, R), ascending h1 within
each face. First new component gets label 1, next gets 2, etc. Same tile
always emits the same TileOp.

**Design alternatives rejected:**
- Group-start bit (1 bit): fails when groups span faces
- Connectivity bitmask (N bits): redundant for equivalence classes
- u4 group labels: nibble packing complexity; overflow at >15 groups
- Per-component storage: composition becomes O(N_A x N_B) instead of O(P)

### 4.5 Dead-End Pruning

Before emitting the TileOp, the CUDA kernel prunes **dead-end groups**:
groups that can never contribute to any spanning path or port bridging.

**Pruning rule:** omit a group if it touches exactly ONE face AND has exactly
ONE port on that face.

**Why this is exact (not a heuristic):**

*Claim:* a port belonging to a group that touches exactly one face and has
exactly one port on that face cannot participate in any cross-tile path or
same-face bridge.

*Proof sketch:* Let p be such a port on face F. The group's connected
component in the tile's interior prime graph reaches no other face (single-
face condition). The group has no other port on F (single-port condition).
Therefore p connects to no other port in the TileOp. During composition,
p's global-id can only be unioned with a matched port on the adjacent tile's
corresponding face -- but that matched port gains no additional connectivity
through this tile. Since the adjacent tile already knows about p's physical
primes (via the shared collar), the matched port's own TileOp already
encodes any connectivity derivable from those primes. Removing p from this
tile's TileOp loses no reachability information. QED.

**No further pruning is safe.** Every surviving port carries load-bearing
cross-tile information. Same-face same-group ports bridge positions the
adjacent tile cannot derive from its own halo. Removing any surviving port
risks false disconnection — the unsafe direction for moat search (could
hallucinate moats that don't exist).

**What survives:**

| Group type                | Ports | Keep? | Reason                              |
|---------------------------|-------|-------|-------------------------------------|
| Multi-face (2+ faces)     | any   | YES   | Carries cross-tile connectivity     |
| Single-face, multi-port   | 2+    | YES   | Bridges non-contiguous ports        |
| Single-face, single-port  | 1     | **NO**| Provable dead end                   |

**Validated impact at 850M:**

| Metric          | Before pruning | After pruning |
|-----------------|----------------|---------------|
| Groups per tile | 39-43          | 3-9           |
| Ports per tile  | 60-71          | 30-36         |
| Max ports/face  | 17-21          | 9-11          |

### 4.6 Overflow Sentinel

When a tile exceeds the TileOp v2 packed-data budget after pruning, the CUDA
kernel:

1. Writes 0xFF to all 128 bytes of the TileOp
2. The compositor detects overflow by checking `bytes[0] == 0xFF` —
   structurally impossible for valid tiles (see S4.3)

The compositor treats the tile as a **conservative bridge**:
all ports on all adjacent tile faces touching this tile are unioned into one
equivalence class. No h1 matching, no group logic — everything connects
through this tile.

**Why this is safe for moat search:**

Conservative bridging adds **false connectivity** -- groups that may not
actually be connected get unioned. The only possible error is:

- **False spanning:** the compositor reports "path exists" when no true path
  does. This can cause a true moat to be overlooked (false negative for moat
  detection), but can never cause a non-existent moat to be reported (no
  false positives for moat detection).

The moat search seeks the absence of spanning paths. Conservative bridging
can mask a moat but cannot fabricate one. Correctness is preserved in the
direction that matters: every reported moat is real.

**Expected occurrence:** TileOp v2 is expected to absorb 99.5%+ of operating-
point tiles. The remaining poisoned tiles are handled safely by conservative
bridging and, if needed, Tier 2 reprocessing.

### 4.7 Compositor Escalation

If overflow tiles accumulate enough to degrade search sensitivity, the
compositor escalates from conservative bridging to on-demand recalculation.

**Tier 0 — Normal**
```
Overflow count: 0
Action: standard composition
```

**Tier 1 — Isolated poison**
```
Overflow count: 1 to ESCALATION_THRESHOLD   // default threshold: 100 tiles/band
Action: conservative bridging for poisoned tiles, log indices
Correctness: intact (safe direction — false connectivity only)
Optional: if spanning verdict AND path passes through a poisoned tile
          -> flag for re-verification with Tier 2
```

Tier 1 is the expected operating mode for the rare v2 poison tiles at
`R >= 830M`.

**Tier 2 — Escalation**
```
Overflow count: > ESCALATION_THRESHOLD
Action:
  1. Compositor halts composition for the band
  2. Re-launches CUDA kernel for ONLY the poisoned tiles
  3. Emit to TileOp_wide (256 bytes, 32 ports/face):

     TileOp_wide [256 bytes = 4 cache lines, symmetric layout]
     +-- face_I: { groups: [u8; 32], h1: [u8; 32] }   64 B
     +-- face_O: { groups: [u8; 32], h1: [u8; 32] }   64 B
     +-- face_L: { groups: [u8; 32], h1: [u8; 32] }   64 B
     +-- face_R: { groups: [u8; 32], h1: [u8; 32] }   64 B

     Note: TileOp_wide uses symmetric layout (h1 on all faces)
     because overflow tiles may need h1 matching on any face
     for maximum compatibility with mixed-width reads.

     Under the 257x257 shared-boundary convention, any wide-tile redesign
     must also resolve the `h1 = 256` storage-width issue explicitly.
     (updated 2026-04-09: 257x257 shared boundary convention)

  4. 32 ports/face exceeds Brun-Titchmarsh raw prime cap (~20).
     Unconditionally sufficient.
  5. Recalculated tiles stored in small overflow buffer
  6. Compositor resumes with mixed-width reads
```

Escalation buffer size: negligible (e.g., 100 tiles x 256 B = 25.6 KB).
Hot path is untouched — overflow branch is perfectly predicted (never taken).

## 5. Matching

One matching rule, one optimization. Fully deterministic.

### 5.1 Matching Predicate

Let tiles A and B share a face. Define **delta_h** as the signed offset
between their origin corners along the shared face axis, in lattice units:

```
delta_h = (B.origin - A.origin) projected onto the shared face axis
```

For a regular grid (all tiles axis-aligned, same size), delta_h = 0 for
every shared face. For staggered or multi-resolution grids, delta_h != 0
(see S5.3).

Two ports on a shared face represent the same physical port iff they anchor
on the same Gaussian prime:

```
match(A_port, B_port) := A.h1 == B.h1 + delta_h
```

**Correctness:** both tiles include all primes within collar=7 lattice units
of the shared face. Any prime on the face is visible to both tiles. Both
tiles use deterministic integer arithmetic to compute h1 as the offset of
the port's minimum-position prime from their respective origins. If A.h1 and
B.h1 + delta_h are equal, they reference the same lattice point. No
approximation; no false positives; no false negatives.

### 5.2 Aligned Tiles — I/O Faces (Shared-Prime Identity Matching)

I/O faces are always aligned within towers (`delta_h = 0`). Under the
257x257 shared-boundary convention, Face O of tile A and Face I of tile B
contain the same shared boundary row of lattice points. Both CUDA kernels
therefore see the same face-prime set on that row, plus their own adjacent
collar rows. Since port construction is a deterministic function of the
shared boundary prime set (sort by position, cluster by distance^2 <= k,
take minimum-position anchor), both tiles produce port lists whose shared-face
entries correspond to the same physical Gaussian primes. (updated 2026-04-09: 257x257 shared boundary convention)

**Semantic rule:** composition is by shared-prime identity, not merely by slot
position. For aligned I/O faces, deterministic extraction means `port[i]` on
A's O-face corresponds to `port[i]` on B's I-face because both entries refer
to the same shared boundary primes. (updated 2026-04-09: 257x257 shared boundary convention)

**Optimization:** the compositor may still read **groups only** (the O/I packed
sections identified by the header) and pair equal slots, but that optimization
is justified by shared-prime identity on the duplicated boundary row.
No h1 values are stored for I/O faces — they are unnecessary because the tower
architecture guarantees alignment and the boundary row itself is shared.
(updated 2026-04-09: 257x257 shared boundary convention)

```
match:   port[i] on A.O <-> port[i] on B.I           // same shared boundary primes
access:  header + packed O/I group sections
verify:  port count equality (derived from offsets)
```

This is shared-prime identity matching with `delta_h = 0`, where equality is
guaranteed by the shared boundary row and deterministic extraction rather than
checked with stored h1 at runtime. (updated 2026-04-09: 257x257 shared boundary convention)

### 5.3 Offset Tiles — L/R Faces

L/R faces connect tiles in adjacent towers, which may be shifted by
`delta_h != 0` along the shared face axis due to arc curvature
(requires 0 < |delta_h| < S):

```
(assuming delta_h > 0; symmetric for delta_h < 0)

              +--- Tile B face ---+
 +--- Tile A face ---+
 |  A only  |  overlap  |  B only  |
 0      delta_h         S      S+delta_h
```

Compositor reads the packed L/R group sections and the packed L/R h1 sections
applies the S5.1 predicate (A.h1 == B.h1 + delta_h). When `delta_h = 0`,
the shared boundary column appears in both tiles and the match is again a
shared-prime identity match on duplicated lattice points; when `delta_h != 0`,
h1 resolves which overlap points coincide. Ports in the non-overlap region
have no match on the other tile -- correctly disconnected. (updated 2026-04-09: 257x257 shared boundary convention)

**Why positional matching fails with offset faces:**

Non-overlap primes disrupt the port list in three ways:
1. **Extra port:** non-overlap prime creates a port B doesn't have. Indices
   shift — every subsequent positional pair is cross-wired.
2. **Extended port:** non-overlap prime chains into an overlap port, moving
   its h1 anchor. Positional pairing hits the wrong physical port.
3. **Merged ports:** non-overlap prime bridges two overlap-zone clusters.
   One tile sees one port, the other sees two. Counts differ.

All three silently corrupt connectivity under positional matching. h1 matching
handles all three correctly. This does not conflict with aligned-face shared-prime
identity matching in S5.2; the failure mode here is specific to offset faces.
(updated 2026-04-09: 257x257 shared boundary convention)

### 5.4 Diagnostic Verification

A full-fingerprint verification mode exists for testing. Matches on all
geometric fields (h1, h2, d1, d2) to confirm that h1-only matching produces
identical composition results. Runs in the Python tile-validator, not in the
production compositor.

## 6. Composition Algorithm

### 6.1 Phase 1: Wire Internal Groups

For each tile, union all ports sharing the same group label. Reads the header
and all packed group sections. No h1 access needed.

```rust
let mut leaders: [Option<usize>; 256] = [None; 256]; // stack, u8 range
for face in [O, I, L, R] {
    let section = tile.face_groups(face);    // span derived from header
    for (slot, &group) in section.iter().enumerate() {
        let gid = global_id(tile_idx, face, slot);
        match leaders[group as usize] {
            Some(lead) => uf.union(lead, gid),
            None       => { leaders[group as usize] = Some(gid); }
        }
    }
}
```

O(P) per tile where P = number of active ports. Zero heap allocation
(leaders array on stack, 256 x 8 = 2 KB; index 0 unused since group 0 is
the empty sentinel).

### 6.2 Phase 2: Match Shared Faces

For each pair of adjacent tiles, match ports on the shared face.

**Aligned I/O tiles (shared-prime identity shortcut — groups only, packed O/I sections):**

```rust
// Face O of tile a matches Face I of tile b (vertical neighbors within tower)
let a_groups = tile_ops[a_idx].face_groups(O);
let b_groups = tile_ops[b_idx].face_groups(I);
debug_assert!(a_groups.len() == b_groups.len(), "aligned port count mismatch on shared I/O face");
for slot in 0..a_groups.len() {
    uf.union(
        global_id(a_idx, O, slot),
        global_id(b_idx, I, slot),
    );
}
```

Port count mismatch on shared I/O faces (one side derives fewer packed entries
than the other) is a hard error during validation — it indicates non-deterministic
collar classification. This check runs unconditionally (not debug-only) at
least during validation/testing mode.

**Offset L/R tiles (h1 matching — packed L/R group sections plus packed h1 sections):**

```rust
// Face R of tile a matches Face L of tile b (horizontal neighbors across towers)
let a_groups = tile_ops[a_idx].face_groups(R);
let b_groups = tile_ops[b_idx].face_groups(L);
let a_h1 = tile_ops[a_idx].face_h1(R);
let b_h1 = tile_ops[b_idx].face_h1(L);
let delta_h = grid.delta_h(a_idx, b_idx);
for sa in 0..a_groups.len() {
    for sb in 0..b_groups.len() {
        if a_h1[sa] == b_h1[sb].wrapping_add(delta_h) {
            uf.union(
                global_id(a_idx, R, sa),
                global_id(b_idx, L, sb),
            );
        }
    }
}
```

**Conservative bridge (overflow sentinel, S4.6):**

```rust
if tile_ops[tile_idx].bytes[0] == 0xFF {
    // Poison tile: union ALL adjacent ports through it (S4.3 overflow check)
    let mut first = None;
    for (neighbor_idx, shared_face) in adjacent_tiles(tile_idx) {
        let opp = opposite(shared_face);
        for slot in 0..tile_ops[neighbor_idx].face_groups(opp).len() {
            let gid = global_id(neighbor_idx, opp, slot);
            match first {
                None    => first = Some(gid),
                Some(f) => uf.union(f, gid),
            }
        }
    }
    continue; // skip normal composition for this tile
}
```

### 6.3 Phase 3: Check Spanning

```rust
for pi in inner_boundary_ports {
    for po in outer_boundary_ports {
        if uf.find(pi) == uf.find(po) {
            return Spanning;
        }
    }
}
return Moat;
```

### 6.4 Complexity

For a band of W x H tiles:

| Phase           | Cost                                          |
|-----------------|-----------------------------------------------|
| Wire groups     | O(total_ports)                                |
| Match faces     | O(total_ports) — bounded by actual packed face counts |
| Spanning check  | O(boundary_ports)                             |
| **Total**       | **O(total_ports x alpha(total_ports))** where alpha is the inverse Ackermann function (effectively constant) |

## 7. CUDA Kernel Pipeline

One block per tile, 256 threads, 8-phase pipeline:

1. **Sieve** — identify Gaussian prime candidates in the 271 x 271 lattice point grid
2. **Miller-Rabin** — confirm that norm(a + bi) = a^2 + b^2 is a rational prime
3. **Compact** — collect confirmed primes into shared memory
4. **UF init** — initialize union-find over tile primes
5. **Neighbor union** — connect primes whose squared Euclidean distance <= k=40
6. **Flatten** — path compression on UF
   (see S7.1 for synchronization details on phases 4-6)
7. **Classify** — identify face ports, assign group labels, prune dead-ends
8. **Emit** — write 128-byte TileOp to `tile_ops[blockIdx.x * 128]`:
   - Header bytes 0-2 = `off_I`, `off_L`, `off_R`
   - Packed payload order: O groups, I groups, L groups, R groups, L h1, R h1
   - Optional pad: byte 127 may be zero when the residual budget is odd
   If `o + i + 2l + 2r > 125`: fill all 128 bytes with 0xFF.

After emit, raw tile data is discarded. The TileOp is the sole output.

Shared memory budget: 271 x 271 = 73,441 lattice points. Prime bitmap:
73,441 bits = 9,181 bytes ~ 9 KB. Fits in 48 KB shared memory with room
for UF arrays (at 850M density: ~1,500 primes x 2 B parent = 3 KB).

### 7.1 Union-Find Synchronization (Phases 4-6)

Parent array lives in shared memory: one `u16` per prime in the tile (max
~6,300 at origin density, ~1,500 at 850M radius).

**Phase 4 (UF init):** each thread initializes `parent[i] = i` for its
assigned primes. `__syncthreads()` after init.

**Phase 5 (Neighbor union):** threads process neighbor pairs in parallel.
Union uses `atomicCAS` on parent entries with path splitting during find
(Jayanti-Tarjan concurrent union-find pattern). No barrier within Phase 5 —
atomic operations provide the necessary linearizability.

**Phase 6 (Flatten):** `__syncthreads()` barrier between Phase 5 and Phase 6.
After the barrier, all unions are complete — no concurrent modifications.
Each thread performs iterative root-finding on its assigned primes (chase
parent pointers to fixpoint). No atomics needed post-barrier.

## 8. Ring Expansion

The search proceeds outward from the ISE-identified blockage radius:

1. Construct annular band of radial depth D=8192 lattice units (= 32 tiles radially)
2. Enumerate tiles intersecting the band
3. CUDA: compute TileOp for each tile (batched kernel launch)
4. Rust: scan byte 0 of each TileOp -- count overflow tiles
   (`bytes[0] == 0xFF`), decide escalation tier (S4.7: Compositor Escalation)
5. Rust: compose TileOps via Phase 1-3
6. If spanning: advance outer boundary, repeat from step 1
7. If not spanning: moat candidate — verify with finer resolution

Ring expansion (marching outward) is used instead of binary search because
spanning is **not monotone in radius** in the 826M-835M transition zone:
a band at radius R may span while a band at R+D does not, then R+2D spans
again. Binary search assumes "spanning at R implies spanning at all R' < R,"
which fails here.

## 9. Design Decisions

| Decision | Chosen | Rejected | Reason |
|----------|--------|----------|--------|
| TileOp size | Fixed 128 B (2 cache lines) | Variable-length | Direct indexing; no offset table; trivial CUDA writes |
| Layout | Dynamic packed O-I-L-R with 3-byte header | Fixed symmetric or fixed-slot layouts | Same 128-byte footprint, but bytes are allocated where the tile needs them. O/I remain group-only; L/R retain h1. |
| Group width | u8 (1 byte) | u4 (nibble) | No nibble packing; eliminates group overflow; simpler code |
| Empty encoding | `off_I = off_L = off_R = 3` | face_counts field | Unique empty-layout header; zero payload |
| Tile status | Derived from header | Dedicated face_mask byte | Structural impossibility of `bytes[0] == 0xFF`; no extra metadata |
| Face capacity | Dynamic, budgeted by `o+i+2l+2r <= 125` | v1-style fixed face caps | High-port faces borrow bytes from sparse faces |
| Matching | Shared-prime identity for aligned faces; h1 equality for offset faces | Three separate modes | delta_h=0 on shared boundaries gives identity matching; h1 handles offset overlap (updated 2026-04-09: 257x257 shared boundary convention) |
| Matching (vs v1) | Equality (h1) | Distance formula | Same primes -> same offsets; exact, not approximate |
| Overflow | 0xFF sentinel + escalation | Full-fingerprint TileOp variant | Conservative bridge safe for moat search; no extra encoding |
| Tile identity | Implicit (index + GridDesc) | Stored coordinates | Regular grid -> trivial derivation; saves 8 B/tile |
| Collar depth | 7 = ceil(sqrt(40)) | 6 = floor(sqrt(40)) | ceil ensures all connecting primes are captured; free in fixed layout |
| Tile size | S=256 | S=512 | CUDA block alignment; note that the old "h1 fits in u8" rationale is no longer valid under shared-boundary ownership (updated 2026-04-09: 257x257 shared boundary convention) |
| Port model | Port-transfer | Per-component | Composition O(P) not O(N_A x N_B) |
| Search | Ring expansion | Binary search | Transition zone non-monotonicity |
| Tile shape | Square only | Rectangular | Non-square tiles further complicate the already-open h1 width issue under 257-point faces (updated 2026-04-09: 257x257 shared boundary convention) |
| Halo model | 257-point tile proper + 7-deep collar | Exclusive boundary convention | Shared boundary plus collar enables exact shared-prime identity matching (updated 2026-04-09: 257x257 shared boundary convention) |
| Dead-end pruning | Single-face single-port omitted | Keep all / prune more | 70-80% reduction; further pruning risks false disconnection |

## 10. Data Layout Summary

```
N = GridDesc.width * GridDesc.height         -- tiles per sector batch (~70M typical)

Per search band (sector batch):
  GridDesc:      32 bytes                     -- 1 instance
  tile_ops:      [TileOp; N]                 -- ~8.96 GB at N=70M (128 B each)
  overflow_buf:  [TileOp_wide; overflow_N]    -- negligible (Tier 2 only)
```

Tile status (dead/alive/overflow) is derived from group data — no separate
metadata fields. Total per-tile cost remains 128 bytes.

**Sector batching:** the ~70M tile figure assumes angular sector processing
(e.g., 45-degree octants). Full-annulus tile count at R=830M: circumference
~5.2B lattice units, at S=256 -> ~20.4M tiles tangentially x 32 radially ->
~652M tiles total. Sector batching keeps the working set under GPU memory
limits. The TileOp format and composition logic are sector-agnostic — sectors
compose independently and results merge at sector boundaries.

## 11. Limits and Constants

```
TILE_SIDE            = 256    // S = 256 lattice units = 257 tile-proper points per axis (updated 2026-04-09: 257x257 shared boundary convention)
COLLAR_DEPTH         = 7      // perpendicular depth from face (1..7 lattice units)
STEP_K               = 40     // moat step parameter
HALO_SIDE            = 271    // (S + 1) + 2*collar = 257 + 14 = 271 lattice points (updated 2026-04-09: 257x257 shared boundary convention)
TILEOP_SIZE          = 128    // bytes, exactly 2 cache lines
TILEOP_HEADER_BYTES  = 3      // off_I, off_L, off_R
TILEOP_DATA_BYTES    = 125    // packed payload budget
NUM_FACES            = 4      // I, O, L, R
GROUP_LABEL_MIN      = 1
GROUP_LABEL_MAX      = 255
OVERFLOW_SENTINEL    = 0xFF   // all 128 bytes filled; detected via bytes[0] == 0xFF
EMPTY_OFFSET         = 3      // off_I = off_L = off_R = 3 for the empty tile
ESCALATION_THRESHOLD = 100    // overflow tile count triggering Tier 2 recalc (tunable)
```

Validated empirically by Python tile-validator at radii 141, 14K, 849M, 854M
(2026-04-07). See `docs/supportive/2026-04-07-tile-validator-report.md` and
`docs/supportive/2026-04-07-pruning-analysis-report.md`.
