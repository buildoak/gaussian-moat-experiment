---
title: "Compositor Specification v3 — Group-Level Union-Find"
date: 2026-04-09
version: 3
status: draft
depends:
  - tile_spec.md (v5)
  - grid_spec.md (v1)
---

# Compositor Specification v3 — Group-Level Union-Find

## S1. Overview

The compositor answers one binary question: does any port on the inner
boundary ring of the annulus connect, transitively through group links and
face matches, to any port on the outer boundary ring?

If yes: **spanning**. If no: **moat**.

This spec defines the algorithm that wires TileOps (tile_spec.md v5)
together across the grid (grid_spec.md v1) and performs the spanning check.
It keeps the group-level union-find design from v2, but updates all TileOp
access patterns for **TileOp v2 dynamic packed layout**.

Key properties:

- **Group-level UF** — operands are `(tile, group)` pairs, not individual
  ports.
- **Prefix-sum addressed UF** — the UF lives outside the TileOp array; there is
  no embedded parent area inside TileOp v2.
- **Offset-based TileOp parsing** — the compositor reads `off_I`, `off_L`,
  and `off_R`, derives face counts, and slices packed face sections.
- **Same matching predicates** — shared-prime identity for aligned I/O faces,
  h1 equality for offset L/R faces, and shared-boundary identity as the
  `delta_h = 0` special case on L/R. (updated 2026-04-09: 257x257 shared
  boundary convention)
- **Single-pass tower sweep** — I/O matching, pre-flattening, and L/R
  matching remain fused.

### S1.1 Core Insight

The CUDA kernel computes connected components within each tile. Group labels
therefore already encode intra-tile connectivity. The compositor does not
wire same-group ports within a tile; it allocates one UF element per distinct
group label and unions those elements only when shared-face matching proves an
inter-tile connection.

### S1.2 Relationship to Other Specs

This spec consumes:

- TileOp layout, overflow semantics, and matching predicates from
  `tile_spec.md` v5.
- Tower geometry and `delta_h` derivation from `grid_spec.md`.

This spec supersedes compositor_spec.md v2. The old embedded-UF design is
removed because TileOp v2 has no parent-storage area inside the 128-byte
record.

---

## S2. Constants

All constants from tile_spec.md S11 and grid_spec.md S2 apply. Additional:

| Name | Value | Derivation |
|------|-------|------------|
| MAX_GROUPS_PER_TILE | 9 | Validated max after dead-end pruning at 850M. |
| TILES_PER_TOWER | 32 | From grid_spec S2. |
| GROUP_LABEL_MIN | 1 | Group 0 is never stored. |
| GROUP_LABEL_MAX | 255 | u8 range minus zero. |

---

## S3. UF Domain and Addressing

### S3.1 Domain Reduction

The port-level compositor allocates one UF element per active port. The
group-level compositor allocates one UF element per distinct group label per
tile. At operating radii this reduces the domain from roughly billions of
ports to hundreds of millions of groups.

### S3.2 Prefix-Sum Addressing

Phase 0 scans all TileOps, extracts the maximum group label per tile, and
computes a prefix-sum:

```rust
group_offset[0] = 0
for t in 0..N {
    if is_dead(t) || is_overflow(t) {
        group_offset[t + 1] = group_offset[t]
    } else {
        max_g = max_group_label(tile_ops[t])
        group_offset[t + 1] = group_offset[t] + max_g
    }
}
total_groups = group_offset[N]
```

Global ID:

```rust
global_id(t, G) = group_offset[t] + (G - 1)
```

This handles any group count representable by the TileOp without mutating the
TileOp array.

### S3.3 TileOp v2 Parsing

For each non-overflow TileOp:

```rust
off_I = tile[0] as usize
off_L = tile[1] as usize
off_R = tile[2] as usize

o_cnt = off_I - 3
i_cnt = off_L - off_I
l_cnt = off_R - off_L
r_cnt = (125 - o_cnt - i_cnt - 2 * l_cnt) >> 1
h_start = off_R + r_cnt
```

Face sections:

```rust
O groups = tile[3 .. off_I]
I groups = tile[off_I .. off_L]
L groups = tile[off_L .. off_R]
R groups = tile[off_R .. off_R + r_cnt]
L h1     = tile[h_start .. h_start + l_cnt]
R h1     = tile[h_start + l_cnt .. h_start + l_cnt + r_cnt]
```

The optional pad at byte 127 is ignored.

---

## S4. Union-Find Operations

### S4.1 Path Halving

The UF uses path halving without rank:

```rust
fn find(mut i: u32, parent: &mut [u32]) -> u32 {
    loop {
        let p = parent[i as usize];
        if p == i { return i; }
        let pp = parent[p as usize];
        parent[i as usize] = pp;
        i = pp;
    }
}

fn union(a: u32, b: u32, parent: &mut [u32]) {
    let ra = find(a, parent);
    let rb = find(b, parent);
    if ra != rb {
        if ra < rb { parent[rb as usize] = ra; }
        else       { parent[ra as usize] = rb; }
    }
}
```

### S4.2 Initialization

```rust
fn init_uf(total_groups: usize) -> Vec<u32> {
    (0..total_groups as u32).collect()
}
```

### S4.3 Why No Embedded UF

TileOp v2 uses the full 128-byte record for header, packed face data, and the
optional terminal pad. There is no stable per-tile parent-slot area. The compositor must
therefore keep UF state in a separate array addressed by `group_offset[]`.

---

## S5. Processing Order — Single-Pass Tower Sweep

### S5.1 Fused Loop Structure

```rust
fn compose(
    tile_ops: &[u8],
    parent: &mut [u32],
    group_offset: &[u32],
    delta: &[u32],
    n_towers: usize,
) {
    for j in 0..n_towers {
        // Step 1: I/O matching within tower j
        for r in 0..31u32 {
            let a = tile_index(j, r);
            let b = tile_index(j, r + 1);
            if is_skip(a, tile_ops) || is_skip(b, tile_ops) { continue; }
            if is_overflow(a, tile_ops) { handle_overflow(a, tile_ops, parent, group_offset); continue; }
            if is_overflow(b, tile_ops) { handle_overflow(b, tile_ops, parent, group_offset); continue; }
            match_io(tile_ops, a, b, parent, group_offset);
        }

        // Step 2: pre-flatten tower j
        pre_flatten_tower(tile_ops, j, parent, group_offset);

        // Step 3: L/R matching with tower j-1
        if j > 0 {
            let d = delta[j - 1] as usize;
            let q = d / 256;
            let f = d % 256;

            for r in 0..32u32 {
                let b = tile_index(j, r);
                if is_skip(b, tile_ops) { continue; }
                if is_overflow(b, tile_ops) { handle_overflow(b, tile_ops, parent, group_offset); continue; }

                if r as usize >= q {
                    let a = tile_index(j - 1, (r as usize - q) as u32);
                    if !is_skip(a, tile_ops) && !is_overflow(a, tile_ops) {
                        match_lr(tile_ops, a, b, -(f as i16), parent, group_offset);
                    }
                }

                if f > 0 && r as usize >= q + 1 {
                    let a = tile_index(j - 1, (r as usize - q - 1) as u32);
                    if !is_skip(a, tile_ops) && !is_overflow(a, tile_ops) {
                        match_lr(tile_ops, a, b, (256 - f) as i16, parent, group_offset);
                    }
                }
            }
        }
    }
}
```

### S5.2 Pre-Flattening

```rust
fn pre_flatten_tower(
    tile_ops: &[u8],
    j: usize,
    parent: &mut [u32],
    group_offset: &[u32],
) {
    for r in 0..32u32 {
        let t = tile_index(j, r);
        if is_skip(t, tile_ops) || is_overflow(t, tile_ops) { continue; }
        let max_g = max_group_label(tile_ops, t);
        for g in 1..=max_g {
            let gid = global_id(t, g, group_offset);
            let root = find(gid, parent);
            parent[gid as usize] = root;
        }
    }
}
```

---

## S6. Compositor Phases

### S6.1 Phase 0: Initialization

1. Scan all TileOps.
2. Detect overflow via `tile[0] == 0xFF`.
3. Detect dead tiles via `tile[0] == tile[1] == tile[2] == 3` and `tile[3] == 0`.
4. For normal tiles, parse packed face sections and compute `max_group_label`.
5. Build `group_offset[]`, then allocate `parent[]`.

### S6.2 Phase 1: I/O Matching

```rust
fn match_io(
    tile_ops: &[u8],
    a: usize,
    b: usize,
    parent: &mut [u32],
    group_offset: &[u32],
) {
    let a_groups = face_groups(tile_ops, a, O);
    let b_groups = face_groups(tile_ops, b, I);
    debug_assert!(a_groups.len() == b_groups.len(), "I/O port count mismatch");

    for s in 0..a_groups.len() {
        union(
            global_id(a, a_groups[s], group_offset),
            global_id(b, b_groups[s], group_offset),
            parent,
        );
    }
}
```

Aligned I/O matching still uses shared-prime identity on the duplicated
boundary row. TileOp v2 only changes where those group bytes live.

### S6.3 Phase 2: L/R Matching

```rust
fn match_lr(
    tile_ops: &[u8],
    a: usize,   // R-face tile
    b: usize,   // L-face tile
    delta_h: i16,
    parent: &mut [u32],
    group_offset: &[u32],
) {
    let a_groups = face_groups(tile_ops, a, R);
    let a_h1     = face_h1(tile_ops, a, R);
    let b_groups = face_groups(tile_ops, b, L);
    let b_h1     = face_h1(tile_ops, b, L);

    for sa in 0..a_groups.len() {
        let target_h1 = (a_h1[sa] as i16) - delta_h;
        for sb in 0..b_groups.len() {
            if (b_h1[sb] as i16) == target_h1 {
                union(
                    global_id(a, a_groups[sa], group_offset),
                    global_id(b, b_groups[sb], group_offset),
                    parent,
                );
                break;
            }
        }
    }
}
```

This consumes packed sections, not fixed byte ranges. The `delta_h = 0`
special case still reduces to shared-boundary identity on a shared column.

### S6.4 Phase 3: Octant Stitching

Octant stitching still matches reflected boundary tiles using the same
matching predicates. The stitching phase must parse the relevant face section
from TileOp v2 before applying the reflection-adjusted `delta_h`.

### S6.5 Phase 4: Spanning Check

Collect roots of all inner-boundary groups into a hash set, then test all
outer-boundary groups against it:

```rust
fn spanning_check(...) -> Verdict {
    let mut inner_roots = HashSet::new();
    // row 0 I-face groups + exposed inner L/R groups
    // row 31 O-face groups + exposed outer L/R groups
}
```

Boundary completeness rules are unchanged; only face access moves from fixed
slots to offset-derived slices.

---

## S7. Edge Cases

### S7.1 Overflow Tiles

Detected by `tile_ops[t * 128] == 0xFF`. The compositor assigns one bridge
group ID to the overflow tile and unions all neighbor groups touching it into
that bridge.

```rust
fn handle_overflow(
    t: usize,
    tile_ops: &[u8],
    parent: &mut [u32],
    group_offset: &[u32],
) {
    let bridge_gid = overflow_global_id(t);
    for (neighbor, shared_face) in adjacent_tiles(t) {
        if is_skip(neighbor, tile_ops) { continue; }
        if is_overflow(neighbor, tile_ops) {
            union(bridge_gid, overflow_global_id(neighbor), parent);
            continue;
        }
        let opp = opposite_face(shared_face);
        for &g in face_groups(tile_ops, neighbor, opp) {
            union(bridge_gid, global_id(neighbor, g, group_offset), parent);
        }
    }
}
```

### S7.2 Dead Tiles

Detected by `off_I == off_L == off_R == 3` and `tile[3] == 0`. No UF slots are
allocated. Dead tiles are skipped in all phases.

### S7.3 Exposed L/R Boundary Ports

Exposure detection is still based on the `delta` geometry from grid_spec S5.
The only update is that exposed-port tests now iterate the packed L/R face
sections and their packed h1 arrays rather than fixed-size face arrays.

---

## S8. Cache Analysis

### S8.1 TileOp Footprint Per Tower

One tower is still 32 tiles * 128 bytes = 4 KB of TileOps. TileOp v2 changes
which bytes are hot:

- O/I groups concentrate near the start of each TileOp.
- L/R groups and h1 bytes concentrate later in each TileOp.
- There is no co-located UF metadata in the TileOp.

### S8.2 Two-Tower Working Set

During L/R matching between towers `j-1` and `j`, the compositor streams:

- 8 KB of TileOps (two towers)
- The corresponding UF parent slices in the separate `parent[]` array

The TileOp working set remains L1/L2-friendly. UF traffic is a separate stream.

### S8.3 Cache-Line Layout

```
Cache line 0 (bytes 0-63):
  header + O groups + I groups + early L/R groups

Cache line 1 (bytes 64-127):
  trailing L/R groups + L h1 + R h1 + optional pad
```

| Operation | Data accessed | Cache lines touched |
|-----------|---------------|---------------------|
| I/O match | header + O/I groups | usually line 0 only |
| L/R match | header + L/R groups + L/R h1 | 1 or 2 lines depending on offsets |
| UF find/union | separate `parent[]` | not co-located with TileOp |

The old v2 claim that h1 loads auto-prefetch UF parents is no longer true.

---

## S9. Performance Estimates

### S9.1 Per-Tile Cost

TileOp v2 changes parsing cost from fixed offsets to three header reads, three
subtractions, and one shift. This is negligible relative to face-matching and
UF traffic.

### S9.2 Wall-Clock Estimate

The compositor remains close to the prior prefix-sum estimate: roughly the same
number of UF operations, with slightly lower TileOp bandwidth for sparse tiles
and a small parsing overhead per tile.

### S9.3 Bandwidth Model

TileOp v2 improves data locality for the dominant I/O path because O/I groups
sit in the lowest offsets. L/R matching still touches the deeper bytes.

---

## S10. Correctness Argument

The v3 compositor is semantically identical to the v2 compositor after replacing
"fixed-slot face access" with "offset-derived face access":

1. Every inter-tile match still triggers the same group-level union.
2. Every group label still denotes the same intra-tile connectivity class.
3. Header parsing changes addressing only, not the matching predicate.
4. Conservative bridging for overflow tiles is unchanged.

Therefore the spanning verdict is unchanged relative to any faithful TileOp v2
producer.

---

## S11. Memory Budget

| Structure | Size | Notes |
|-----------|------|-------|
| tile_ops | 9.38 GB | 73.4M * 128 B. Read-only. |
| group_offset | 294 MB | (73.4M + 1) * 4 B. |
| UF parent | ~1.47 GB | ~367M groups * 4 B at 5 groups/tile average. |
| delta table | 9 MB | 2.3M * 4 B. |
| base_y table | 18 MB | 2.3M * 8 B. |
| inner_roots HashSet | ~28 MB | Boundary probe set. |
| **Total** | **~11.2 GB** | |

TileOp v2 does not change the 128-byte record size, so the dominant memory
figures remain the same as the old prefix-sum path.

---

## S12. Streaming Relevance

The compositor still only needs a sliding two-tower TileOp window for matching,
but the current design materializes the full TileOp array for simplicity. TileOp
v2 is compatible with future streaming because its records are still fixed-size
and directly addressable.

---

## S13. Design Decisions

| Decision | Chosen | Rejected | Reason |
|----------|--------|----------|--------|
| UF operands | `(tile, group)` pairs | `(tile, face, slot)` tuples | Eliminates explicit intra-tile wiring |
| UF storage | Prefix-sum + separate parent array | Embedded UF in TileOp | TileOp v2 has no in-record parent area |
| TileOp parsing | Header-derived offsets | Fixed byte ranges | Required by TileOp v2 |
| Face ordering | O-I-L-R payload | I-O-L-R payload or fixed-slot order | Keeps I/O match data in low offsets |
| Group numbering | First appearance in I-O-L-R scan | Payload-order numbering | Preserves deterministic labels from tile_spec |
| Overflow handling | Single bridge group | Per-face bridge groups | Same correctness guarantee, simpler |

---

## S14. Open Questions

1. **Exposed L/R boundary ports.** The exact h1 thresholds still depend on the
   per-tower `delta` geometry.
2. **Octant stitching detail.** Reflection-adjusted face mapping and `delta_h`
   remain delegated to grid_spec.
3. **h1 = 256 edge case.** TileOp v2 layout does not resolve the pre-existing
   storage-width issue for the shared-boundary endpoint on L/R faces.

---

## S15. Invariants

1. **Overflow sentinel:** `tile[0] == 0xFF` iff the entire TileOp is poisoned.
2. **Empty tile header:** `tile[0] == tile[1] == tile[2] == 3` and `tile[3] == 0`
   iff the tile has no ports on any face.
3. **Offset monotonicity:** for every normal tile, `3 <= off_I <= off_L <= off_R <= 127`.
4. **Group label range:** all stored group labels lie in `[1, 255]`.
5. **Aligned I/O count agreement:** adjacent tower tiles have equal O/I face counts.
6. **L/R section agreement:** `len(face_groups(L)) == len(face_h1(L))` and
   `len(face_groups(R)) == len(face_h1(R))`.
7. **UF domain:** `global_id(t, G) < total_groups` for every valid normal-tile
   pair `(t, G)`.

---

## S16. Constants Summary

```
TILE_SIDE              = 256
COLLAR_DEPTH           = 7
STEP_K                 = 40
TILEOP_SIZE            = 128
TILEOP_HEADER_BYTES    = 3
TILEOP_DATA_BYTES      = 125
OVERFLOW_SENTINEL      = 0xFF
EMPTY_OFFSET           = 3
TILES_PER_TOWER        = 32
MAX_GROUPS_PER_TILE    = 9
```
