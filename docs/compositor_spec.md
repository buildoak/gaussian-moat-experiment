---
title: "Compositor Specification v9 — Group-Level Union-Find"
date: 2026-04-13
version: 9
status: draft
depends:
  - tile_spec.md (v5)
  - grid_spec.md (v6)
---

# Compositor Specification v9 — Group-Level Union-Find

## S1. Overview

The compositor answers one binary question: does any port on the inner
boundary ring of the annulus connect, transitively through group links and
face matches, to any port on the outer boundary ring?

If yes: **spanning**. If no: **moat**.

This spec defines the algorithm that wires TileOps (tile_spec.md v5)
together across the grid (grid_spec.md v6) and performs the spanning check.
It keeps the group-level union-find design from v2, but updates all TileOp
access patterns for **TileOp v2 dynamic packed layout**.

Key properties:

- **Group-level UF** -- operands are `(tile, group)` pairs, not individual
  ports.
- **Prefix-sum addressed UF** -- the UF lives outside the TileOp array; there is
  no embedded parent area inside TileOp v2.
- **Offset-based TileOp parsing** -- the compositor reads `off_I`, `off_L`,
  and `off_R`, derives face counts, and slices packed face sections. Supports
  both 128-byte standard and 256-byte extended TileOps.
- **Same matching predicates** -- shared-prime identity for aligned I/O faces,
  decoded h1 equality for offset L/R faces, and shared-boundary identity as the
  `delta_h = 0` special case on L/R. (updated 2026-04-09: 257x257 shared
  boundary convention)
- **Single-pass tower sweep** -- I/O matching, pre-flattening, and L/R
  matching remain fused.
- **Variable tower height** -- `tiles_per_tower[j]` ranges from 32 (at
  Y-axis) to 46 (at 45°). Extra tiles added at the TOP create an "outer
  staircase". Adjacent towers differ in height by at most 1 tile.
- **Staircase-aware spanning check** -- inner and outer boundary exposure
  accounts for the staircase geometry of the tower grid: horizontal treads
  (I/O faces) and vertical risers (exposed L/R face segments). Both inner
  staircase (base_y differences) and outer staircase (variable tower height)
  contribute boundary exposure.
- **First-octant sufficient** -- no octant stitching phase. Cross-diagonal
  connectivity is covered by COLLAR depth. Tower generation extends past
  y=x for cross-diagonal coverage (grid_spec S7-S8).
- **Burst-mode early termination** -- incremental spanning check after each
  angular burst enables skipping remaining towers once spanning is detected.
  Sound by UF monotonicity (S6.6, S10.3).

### S1.1 Module Boundary

The compositor is a separate module (`tiles-compositor/`) with a clean
interface:

**Input:**
- `Grid` struct -- tower geometry (base_y, delta tables, tower count J,
  `tiles_per_tower[j]` per-tower height metadata)
- `TileOp` array -- N tiles of 128-byte standard TileOps
- `ExtendedTileSet` -- bitset of extended tile indices + side table of
  256-byte `TileOp256` values (for tiles reprocessed by C++ fallback)

**Output:**
```cpp
struct CompositorResult {
    Verdict verdict;          // Spanning or Moat
    uint32_t total_groups;    // total UF elements allocated
    uint32_t inner_root_count; // distinct roots touching inner boundary
    uint32_t outer_root_count; // distinct roots touching outer boundary
};
```

The compositor does not modify TileOps. It reads them, builds the UF, and
returns a verdict.

### S1.2 Incremental API

The compositor exposes a tower-by-tower incremental interface with burst-mode
early termination support:

```cpp
class Compositor {
public:
    void init(const Grid& grid);

    // Ingest tower j. Towers must be ingested in order: j = 0, 1, 2, ...
    // tower_tileops: tiles_per_tower[j] contiguous 128-byte TileOps.
    // ext: optional extended tile side table for 256-byte tiles.
    void ingest_tower(int32_t j, const uint8_t* tower_tileops,
                      const ExtendedTileSideTable* ext = nullptr);

    // Best-effort spanning check. O(|inner| + |outer|) per call.
    bool has_spanning();

    // --- Burst-mode API (v6) ---

    // Incremental spanning check after a burst of towers.
    // Returns true if any UF root appears in both inner_members_ and
    // outer_members_ collected so far. Does NOT finalize — more towers
    // may follow. O(|inner| + |outer|) per call.
    // Precondition: the rightmost tower of this burst must have had its
    // R-face outer boundary collection deferred (see collect_outer_boundary).
    bool check_spanning_incremental();

    // Collect outer boundary exposure for tower j's top tile(s).
    // Called explicitly by the campaign runner. In burst mode, call ONLY
    // when tower j is confirmed as the grid's actual last tower —
    // either because j == J-1, or because check_spanning_incremental()
    // returned true and finalize() is imminent. See S6.6.
    void collect_outer_boundary(int32_t j);

    // Finalize after all towers ingested (or after early termination).
    // Definitive spanning check.
    CompositorResult finalize();
};
```

**Key design points:**

1. **Previous tower retained as raw tile buffer.** An internal buffer
   `prev_tower_tiles_[TILES_PER_TOWER_MAX * 128]` holds the previous tower's raw tile data.
   L/R matching reads the current tower's L-face ports and the previous
   tower's R-face ports from this buffer, using the same `face_slice()`
   parser on both sides. For extended tiles in the previous tower, the
   compositor checks `ext` and reads from the side table instead of the
   128-byte buffer.

2. **Boundary tracking stores member global_ids, not roots.** Sets
   `inner_members_` and `outer_members_` accumulate raw global_ids during
   `ingest_tower()`. `finalize()` re-finds all members to get current roots,
   then checks intersection. Storing roots at collection time would be
   incorrect because later L/R matching may union groups that were already
   collected, changing their roots.

3. **`has_spanning()` is best-effort.** It re-finds all boundary members on
   each call, which is correct but O(|inner| + |outer|). Useful for
   early-exit checks during incremental ingestion.

4. **Prefix-sum computed per tower.** `group_offset_` is pre-sized to
   `num_tiles + 1` in `init()`. Each `ingest_tower()` fills the offset
   entries for the `tiles_per_tower[j]` tiles in that tower and grows
   `parent_` accordingly.

5. **Deferred R-face boundary collection (v6).** In burst mode, `ingest_tower()`
   collects inner boundary exposure (I-face treads, L-face risers) and outer
   boundary O-face treads as before. However, R-face outer boundary exposure
   for the rightmost tower of a burst is NOT collected during `ingest_tower()`.
   The R-face of that tower will be matched against the next burst's first
   tower's L-face — collecting it as outer boundary would be premature.
   `collect_outer_boundary(j)` is called explicitly when tower j is confirmed
   as the actual last tower. See S6.6 for the full protocol.

### S1.3 Core Insight

The CUDA kernel computes connected components within each tile. Group labels
therefore already encode intra-tile connectivity. The compositor does not
wire same-group ports within a tile; it allocates one UF element per distinct
group label and unions those elements only when shared-face matching proves an
inter-tile connection.

### S1.4 Relationship to Other Specs

This spec consumes:

- TileOp layout, overflow semantics, and matching predicates from
  `tile_spec.md` v5.
- Tower geometry, delta table, and cross-diagonal tower extension
  (extended tower generation, S7-S8) from `grid_spec.md` v6.

This spec supersedes compositor_spec.md v8. Changes from v8:
- Closed h1 intervals `[0, S]` per 257x257 shared boundary convention (S6.5)
- Parameterized K_SQ / COLLAR (S6.4, S10.2, S16)
- Removed stale C++ diagonal infill references (S10.2, S14.2)
- `is_skip(t)` replaced with `empty(t)` (S5.2)
- grid_spec dependency updated to v6

Changes from v7 (retained from v8):
- Variable tower height: `tiles_per_tower[j]` ranges 32..46 (S2, S3, S5)
- Outer staircase boundary from variable tower height (S6.5, S7.3)
- L/R matching iterates only shared rows: `0..min(H_j, H_prev)-1` (S6.5)
- Invariant 9: adjacent tower height difference <= 1 (S15)
- Invariant 10: bump tower L and R face exposure (S15)

Changes from v5 (retained from v7):
- Incremental burst-mode spanning check: `check_spanning_incremental()` (S1.2, S6.6)
- Deferred R-face boundary collection for burst mode (S6.6)
- UF monotonicity argument for early termination soundness (S6.6, S10.3)
- Invariant 8: UF monotonicity (S15)

Changes from v4 (retained from v7):
- Incremental tower-by-tower API (S1.2)
- Zero-padded R-face slots: explicit `group_id == 0` skip guards in all
  L/R matching and boundary collection (S3.3, S6.3, S6.5)
- No early-break in L/R matching loops (dual-neighbor matching, S6.3)
- Overflow handling: compositor asserts `!is_overflow()`, no conservative
  bridge path — `ExtendedTileSideTable` is the only mechanism (S3.2, S7.1)
- Multi-row staircase boundary exposure (S6.5, S7.3)
- Boundary tracking stores member global_ids, not roots (S1.2)

Changes from v3 (retained from v4):
- Module boundary defined (S1.1)
- 256-byte extended TileOp support (S3.3)
- Octant stitching removed (first-octant sufficiency argument in S10)
- Staircase-aware boundary exposure and spanning check (S7.3, S6.5)
- All open questions resolved (S14)

**Superseded implementation: `tile-probe/crates/moat-kernel/src/compose.rs`.**
The old Rust compositor in compose.rs operates on `FacePort { a, b, component }`
structs -- port-level, O(n^2) matching via Euclidean distance. It predates
TileOp v2's packed 128-byte records and group-level union-find. Do not
reference compose.rs for the production compositor; it is retained only as a
historical reference for the port-level algorithm.

---

## S2. Constants

All constants from tile_spec.md S11 and grid_spec.md S2 apply. Additional:

| Name | Value | Derivation |
|------|-------|------------|
| MAX_GROUPS_PER_TILE | 9 | Validated max after dead-end pruning at 850M. |
| TILES_PER_TOWER_MIN | 32 | Minimum tower height (at Y-axis). From grid_spec. |
| TILES_PER_TOWER_MAX | 46 | Maximum tower height (at 45°). From grid_spec. |
| `tiles_per_tower[j]` | 32..46 | Per-tower metadata from the grid. Extra tiles added at TOP. |
| GROUP_LABEL_MIN | 1 | Group 0 is never stored. |
| GROUP_LABEL_MAX | 127 | Global per-tile cap imposed by group-bit steal on L/R faces. |

### S2.1 Encoding and Kernel Limits

Multiple capacity limits interact. They apply at different stages and have
different consequences:

| Limit | Value | Stage | Consequence when exceeded |
|-------|-------|-------|--------------------------|
| **Encoding limit** (group count) | 127 groups | K5 encode / C++ encode | 7-bit group-id field in L/R group byte saturates. TileOp poisoned (`0xFF`). |
| **GPU per-face port limit** | MAX_FACE_PORTS_GPU = 32 | K5 face extraction | Per-face raw port array overflows. `scratch->overflow` set, TileOp poisoned. |
| **GPU total port limit** | MAX_TOTAL_PORTS_GPU = 128 | K5 face extraction | Sum of all four face port counts overflows. `scratch->overflow` set, TileOp poisoned. |
| **GPU group table limit** | MAX_GROUPS_GPU = 127 | K5 pruning / group assignment | Hash table for group entries is full. TileOp poisoned. |
| **Payload capacity (128B)** | TILEOP_PAYLOAD_BYTES = 125 | K5 encode / C++ encode | `o_cnt + i_cnt + 2*l_cnt + 2*r_cnt > 125`. TileOp poisoned. |
| **Payload capacity (256B)** | TILEOP256_PAYLOAD_BYTES = 253 | C++ encode only | `o_cnt + i_cnt + 2*l_cnt + 2*r_cnt > 253`. Should never occur. |
| **Observed maximum** | ~9 groups/tile | Empirical at R = 860M | Well below all limits. Overflow at operating radii indicates a bug. |

The encoding limit (127) and the GPU group table limit (127) are numerically
equal but arise from different constraints: the encoding limit comes from the
7-bit field width in `encode_group_byte()`, while the GPU limit comes from the
`MAX_GROUPS_GPU` constant sizing the hash table in `FaceScratchGPU`. Both
trigger the same poison path.

---

## S3. UF Domain and Addressing

### S3.1 Domain Reduction

The port-level compositor allocates one UF element per active port. The
group-level compositor allocates one UF element per distinct group label per
tile. At operating radii this reduces the domain from roughly billions of
ports to hundreds of millions of groups.

### S3.2 Prefix-Sum Addressing

Phase 0 scans all TileOps, extracts the maximum group label per tile, and
computes a prefix-sum. The total tile count N = sum of `tiles_per_tower[j]`
across all towers (variable per tower, ranging from 32 to 46):

```cpp
group_offset[0] = 0;
for (int t = 0; t < N; t++) {
    assert(!is_overflow(t));  // Caller must pre-process all overflow tiles
    if (is_dead(t)) {
        group_offset[t + 1] = group_offset[t];
    } else {
        // Normal tile (128B) or extended tile (256B from side table)
        const uint8_t* data = tile_data(t);
        int budget = is_extended(t) ? 253 : 125;
        uint8_t max_g = max_group_label(data, budget);
        group_offset[t + 1] = group_offset[t] + max_g;
    }
}
uint32_t total_groups = group_offset[N];
```

**Design invariant: NO overflow tiles reach the compositor.** The caller
(campaign runner or test harness) detects overflow tiles (`bytes[0] == 0xFF`)
and reprocesses them via C++ into 256-byte extended TileOps BEFORE calling
the compositor. The compositor asserts `!is_overflow()` on every tile it
processes. Extended tiles (reprocessed by C++ fallback) are parsed normally
via the side table with `payload_budget = 253`. The `ExtendedTileSideTable`
is the only mechanism for formerly-overflow tiles.

Global ID:

```cpp
uint32_t global_id(int t, uint8_t G) {
    return group_offset[t] + (G - 1);
}
```

This handles any group count representable by the TileOp without mutating the
TileOp array.

### S3.3 TileOp Parsing

The compositor supports two TileOp sizes: the standard 128-byte record and the
256-byte extended record for tiles reprocessed by the C++ fallback.

**Standard 128-byte TileOp:**

For each non-overflow TileOp:

```cpp
uint8_t off_I = tile[0];
uint8_t off_L = tile[1];
uint8_t off_R = tile[2];

int o_cnt = off_I - 3;
int i_cnt = off_L - off_I;
int l_cnt = off_R - off_L;
int payload = 125;
int r_cnt = (payload - o_cnt - i_cnt - 2 * l_cnt) >> 1;
int h_start = off_R + r_cnt;
```

Face sections:

```cpp
// O groups = tile[3 .. off_I)
// I groups = tile[off_I .. off_L)
// L groups = tile[off_L .. off_R)
// R groups = tile[off_R .. off_R + r_cnt)
// L h1     = tile[h_start .. h_start + l_cnt)
// R h1     = tile[h_start + l_cnt .. h_start + l_cnt + r_cnt)
```

The optional pad at byte 127 is ignored.

**256-byte Extended TileOp:**

Same 3-byte header layout. The only difference is the payload budget:

```cpp
int payload = is_extended(t) ? 253 : 125;
int r_cnt = (payload - o_cnt - i_cnt - 2 * l_cnt) >> 1;
```

The extended TileOp is identified via a bitset of tile indices provided by
the campaign runner. At operating radii (R ~ 850M), the census shows 0
overflow tiles, so this set is expected to be empty. The 256-byte path exists
as a safety net for edge cases at extreme radii.

**Extended tile storage:** A `std::unordered_set<uint32_t>` of tile indices
marks which tiles are extended. A parallel side table
`std::unordered_map<uint32_t, TileOp256>` holds their 256-byte records. The
compositor checks membership before parsing:

```cpp
bool is_extended(uint32_t t) {
    return extended_set.contains(t);
}

const uint8_t* tile_data(uint32_t t) {
    if (is_extended(t)) return extended_table[t].data();
    return &tile_ops[t * 128];
}
```

**R-face zero-padding.** The parser derives `r_cnt` from the residual budget
formula `(payload - o_cnt - i_cnt - 2 * l_cnt) >> 1`, not from the actual
number of R-face ports. When the actual R-face port count is less than
`r_cnt`, the encoder (K5 / C++ reference) writes actual R group bytes
starting at `off_R` and jumps to `h_start` for h1 bytes, leaving the
intervening R group slots as zero bytes. **Zero-padded R slots are NOT safe
to process blindly.** `decode_group_id(0) == 0`, and group labels are
1-based (`GROUP_LABEL_MIN = 1`). Calling `global_id(t, 0)` computes
`group_offset[t] + (0 - 1)` -- unsigned underflow, corrupting the UF.
**All L/R face iteration MUST skip entries where
`decode_group_id(group_byte) == 0`.** This applies to L/R matching (S6.3),
boundary collection (S6.5), and `max_group_label()` scanning. I/O faces use
raw byte values as group labels and their counts are exact (no padding) --
only L/R faces have derived counts that may include zero-padded trailing
entries.

---

## S4. Union-Find Operations

### S4.1 Path Halving

The UF uses path halving without rank:

```cpp
uint32_t find(uint32_t i, uint32_t* parent) {
    while (true) {
        uint32_t p = parent[i];
        if (p == i) return i;
        uint32_t pp = parent[p];
        parent[i] = pp;
        i = pp;
    }
}

void union_groups(uint32_t a, uint32_t b, uint32_t* parent) {
    uint32_t ra = find(a, parent);
    uint32_t rb = find(b, parent);
    if (ra != rb) {
        if (ra < rb) parent[rb] = ra;
        else         parent[ra] = rb;
    }
}
```

### S4.2 Initialization

```cpp
void init_uf(uint32_t* parent, uint32_t total_groups) {
    for (uint32_t i = 0; i < total_groups; i++) {
        parent[i] = i;
    }
}
```

### S4.3 Why No Embedded UF

TileOp v2 uses the full 128-byte record for header, packed face data, and the
optional terminal pad. There is no stable per-tile parent-slot area. The compositor must
therefore keep UF state in a separate array addressed by `group_offset[]`.

---

## S5. Processing Order -- Single-Pass Tower Sweep

### S5.1 Fused Loop Structure

The main loop uses two predicates to classify tiles:

```cpp
// Tile has zero collar-zone primes (empty tile, see S7.2)
bool is_dead(int t);

// Returns pointer to tile data (128 or 256 bytes, from flat array or side table)
const uint8_t* tile_data(int t) {
    if (is_extended(t)) return extended_table[t].data();
    return &tile_ops[t * 128];
}
```

**No overflow tiles reach the compositor.** All overflow tiles are
pre-processed into 256-byte extended TileOps by the caller. Extended tiles
are parsed normally via `tile_data()`, which reads from the side table.
The compositor asserts `!is_overflow()` for every tile — there is no
conservative bridge path.

```cpp
void compose(
    const uint8_t* tile_ops,
    uint32_t* parent,
    const uint32_t* group_offset,
    const uint32_t* delta,
    int n_towers
) {
    for (int j = 0; j < n_towers; j++) {
        int H_j = tiles_per_tower[j];

        // Step 1: I/O matching within tower j
        for (int r = 0; r < H_j - 1; r++) {
            int a = tile_index(j, r);
            int b = tile_index(j, r + 1);
            if (is_dead(a) || is_dead(b)) continue;
            match_io(tile_data(a), tile_data(b), parent, group_offset);
        }

        // Step 2: pre-flatten tower j
        pre_flatten_tower(j, parent, group_offset);

        // Step 3: L/R matching with tower j-1
        // Only iterate rows that exist in BOTH towers.
        if (j > 0) {
            int H_prev = tiles_per_tower[j - 1];
            int d = delta[j - 1];
            int q = d / 256;
            int f = d % 256;
            int shared_rows = std::min(H_j, H_prev);

            for (int r = 0; r < shared_rows; r++) {
                int b = tile_index(j, r);
                if (is_dead(b)) continue;

                // Primary L-neighbor
                if (r >= q) {
                    int a = tile_index(j - 1, r - q);
                    if (!is_dead(a)) {
                        match_lr(tile_data(a), tile_data(b), -(int16_t)f, parent, group_offset);
                    }
                }

                // Secondary L-neighbor
                if (f > 0 && r >= q + 1) {
                    int a = tile_index(j - 1, r - q - 1);
                    if (!is_dead(a)) {
                        match_lr(tile_data(a), tile_data(b), (int16_t)(256 - f), parent, group_offset);
                    }
                }
            }
            // Rows beyond min(H_j, H_prev) in the taller tower have no
            // match -- their L/R faces are outer boundary (see S6.5).
        }
    }
}
```

### S5.2 Pre-Flattening

```cpp
void pre_flatten_tower(
    const uint8_t* tile_ops,
    int j,
    uint32_t* parent,
    const uint32_t* group_offset
) {
    int H_j = tiles_per_tower[j];
    for (int r = 0; r < H_j; r++) {
        int t = tile_index(j, r);
        if (empty(t) || is_overflow(t)) continue;
        uint8_t max_g = max_group_label(tile_ops, t);
        for (uint8_t g = 1; g <= max_g; g++) {
            uint32_t gid = global_id(t, g, group_offset);
            parent[gid] = find(gid, parent);
        }
    }
}
```

---

## S6. Compositor Phases

### S6.1 Phase 0: Initialization

1. Scan all TileOps (including any 256-byte extended tiles from the side table).
2. Detect overflow via `tile[0] == 0xFF`.
3. Detect empty tiles via `tile[0] == tile[1] == tile[2] == 3` and `tile[3] == 0`.
4. For normal tiles, parse packed face sections and compute `max_group_label`.
5. Build `group_offset[]`, then allocate `parent[]`.

### S6.2 Phase 1: I/O Matching

```cpp
void match_io(
    const uint8_t* tile_ops,
    int a,
    int b,
    uint32_t* parent,
    const uint32_t* group_offset
) {
    auto a_groups = face_groups(tile_ops, a, FACE_O);
    auto b_groups = face_groups(tile_ops, b, FACE_I);
    assert(a_groups.size() == b_groups.size());  // I/O port count mismatch

    for (int s = 0; s < (int)a_groups.size(); s++) {
        union_groups(
            global_id(a, a_groups[s], group_offset),
            global_id(b, b_groups[s], group_offset),
            parent
        );
    }
}
```

**Proof: `a_groups.size() == b_groups.size()` for aligned I/O faces.**

Tiles (j, r) and (j, r+1) share the 257-column boundary row at
y = base_y[j] + (r+1) * S. Both tiles include this row in their respective
collar zones (O-face collar of tile r, I-face collar of tile r+1). The
primes on the shared boundary are discovered by deterministic integer sieving
-- no floating-point arithmetic, no rounding variance -- so both tiles see
identical primes on this row. Port clustering applies the same K_SQ
distance threshold and the same sort order (h, depth) to both tiles' collar
primes on the shared face. Therefore identical primes produce identical ports
produce identical group counts. The `assert` above guards against
encoder bugs, not against a legitimate geometric mismatch.

Aligned I/O matching still uses shared-prime identity on the duplicated
boundary row. TileOp v2 only changes where those group bytes live.

### S6.3 Phase 2: L/R Matching

```cpp
void match_lr(
    const uint8_t* tile_ops,
    int a,       // R-face tile (previous tower)
    int b,       // L-face tile (current tower)
    int16_t delta_h,
    uint32_t* parent,
    const uint32_t* group_offset
) {
    auto a_groups = face_groups(tile_ops, a, FACE_R);
    auto a_h1     = face_h1(tile_ops, a, FACE_R);
    auto b_groups = face_groups(tile_ops, b, FACE_L);
    auto b_h1     = face_h1(tile_ops, b, FACE_L);

    for (int sa = 0; sa < (int)a_groups.size(); sa++) {
        uint8_t gid_a = decode_group_id(a_groups[sa]);
        if (gid_a == 0) continue;  // zero-padded R slot, not a real group
        int16_t target_h1 = (int16_t)a_h1[sa] - delta_h;
        for (int sb = 0; sb < (int)b_groups.size(); sb++) {
            uint8_t gid_b = decode_group_id(b_groups[sb]);
            if (gid_b == 0) continue;  // zero-padded L slot (rare but possible)
            if ((int16_t)b_h1[sb] == target_h1) {
                union_groups(
                    global_id(a, gid_a, group_offset),
                    global_id(b, gid_b, group_offset),
                    parent
                );
                // NOTE: no break — a port at h1_l == f can match both
                // primary (h1_r=0) and secondary (h1_r=256) neighbors.
                // The redundant union is harmless.
            }
        }
    }
}
```

**Zero-padding guard.** The derived `r_cnt` (and potentially `l_cnt` for
extended tiles) may include zero-padded trailing entries where
`decode_group_id(group_byte) == 0`. Calling `global_id(t, 0)` computes
`group_offset[t] + (0 - 1)` — unsigned underflow, catastrophic UF
corruption. All L/R matching loops MUST skip entries with `group_id == 0`.
I/O faces are not affected (their counts are exact, no padding).

**No early-break.** A port at `h1_l == f` can match both the primary
neighbor (where `h1_r = 0`) and the secondary neighbor (where `h1_r = 256`).
This represents the same physical boundary prime visible from both adjacent
tiles. The resulting redundant union is harmless, but an early `break` would
miss the second match.

**h1 decode discipline.** All matching code decodes raw h1 by combining
group-byte bit 7 with the `h1` byte:

```cpp
uint8_t group_id = group_byte & 0x7F;
uint16_t h1 = ((group_byte >> 7) << 8) | h1_byte;
```

This is baked into the `face_h1()` accessor. Comparing stored bytes directly
is invalid because the group byte also carries the group ID in its low 7 bits.

This consumes packed sections, not fixed byte ranges. The `delta_h = 0`
special case still reduces to shared-boundary identity on a shared column.

### S6.4 Phase 3: First-Octant Sufficiency

First-octant composition is sufficient. No octant stitching phase exists.

Cross-diagonal connectivity within tiles is captured by the
COLLAR = ceil(sqrt(K_SQ)) sieve domain (e.g., ceil(sqrt(40)) = 7 for
K_SQ=40; one-step cross-diagonal paths are covered by intra-tile
union-find). Multi-step cross-diagonal excursions require primes in a
narrow uncovered strip -- negligible at operating radii. The
45-degree boundary is handled by grid termination (towers extend past the
diagonal per grid_spec S7-S8; sub-diagonal tiles are processed normally).

See S10 for the full correctness argument.

### S6.5 Phase 4: Spanning Check (Staircase-Aware)

The spanning check accounts for the staircase geometry of the tower grid.
Towers follow the inner arc: `base_y` decreases as tower index j increases
(towers step down to the right). This creates a staircase pattern at the
inner and outer boundaries of the annulus, with horizontal treads (I/O faces)
and vertical risers (exposed L/R face segments).

**The inner staircase.** `delta[j] = base_y[j] - base_y[j+1] > 0`. As j
increases, tower j+1 starts `delta[j]` units lower than tower j. For tile
(j, 0) in tower j, the L-face (at x = j*S) is partially or fully exposed to
the inner boundary depending on how far tower j drops below tower j-1.

**Inner boundary exposure:**

The inner boundary is formed by:
- **Horizontal treads:** I-faces of row-0 tiles (`base_y[j]` to
  `base_y[j] + S` at x in `[j*S, (j+1)*S]`)
- **Vertical risers:** Exposed L-face portions of row-0 tiles at staircase
  steps

For tile (j, 0)'s L-face (at x = j*S):
- `d_prev = delta[j-1]`, `q_prev = d_prev / S`, `f_prev = d_prev % S`
- L-face spans `y` in `[base_y[j], base_y[j] + S]` which maps to
  `h1` in `[0, S]` (closed -- h1 ranges to S=256, not S-1, per the 257x257
  shared boundary convention; 9-bit h1 encoding via the L/R group-byte
  bit-steal)
- Primary L-neighbor: `(j-1, -q_prev)` -- valid only if `q_prev == 0`
  (row index must be >= 0)
- If `q_prev == 0`: overlap = `S - f_prev`. Matched portion:
  `h1` in `[f_prev, S]`. Unmatched: `h1` in `[0, f_prev)`.
- If `q_prev > 0`: NO L-neighbor exists for row 0. Entire L-face unmatched.
- The unmatched portion is at the bottom of the tile, facing the inner
  boundary riser.
- **Inner-exposed L-face ports:** ports with `h1 < f_prev` (when
  `q_prev == 0`) or ALL ports (when `q_prev > 0`).

Special case `j == 0`: L-face is at the y-axis (grid boundary), NOT the
annulus boundary. Tower j=0's L-face ports are NOT inner-boundary exposed.

**Outer boundary exposure:**

The outer boundary has TWO staircase sources:

**Inner staircase R-face risers (from base_y differences):** Symmetric to the
inner staircase, at the top of the annulus. For tile (j, H_j-1)'s R-face
(at x = (j+1)*S), where `H_j = tiles_per_tower[j]`:
- `d = delta[j]`, `q = d / S`, `f = d % S`
- Primary R-neighbor: `(j+1, H_j-1 + q)` -- valid only if
  `H_j-1 + q < tiles_per_tower[j+1]`
- If valid: overlap = `S - f`. Matched: `h1` in `[0, S-f)`.
  Unmatched: `h1` in `[S-f, S]`.
- If invalid (neighbor row out of range): entire R-face unmatched.
- **Outer-exposed R-face ports:** ports with `h1 >= S - f` (when partially
  matched) or ALL ports (when fully unmatched).

**Outer staircase (NEW — from variable tower height):** Adjacent towers may
differ in height by at most 1 tile. When tower j is taller than a neighbor,
the topmost tile(s) of tower j have exposed faces with no matching tile on
the shorter neighbor. Let `H_j = tiles_per_tower[j]`:

- **Horizontal treads:** O-face of the top row (tile at row `H_j - 1`)
- **Exposed L-face (left risers):** Let `H_prev = tiles_per_tower[j-1]`.
  If `H_j > H_prev`: tiles at rows `H_prev..H_j-1` have L-faces with no
  match on tower j-1. ALL L-face ports of these tiles are outer-boundary
  exposed.
- **Exposed R-face (right risers):** Let `H_next = tiles_per_tower[j+1]`.
  If `H_j > H_next`: tiles at rows `H_next..H_j-1` have R-faces with no
  match on tower j+1. ALL R-face ports of these tiles are outer-boundary
  exposed.
- **Bump tower:** When `H_j > H_prev` AND `H_j > H_next`, the topmost
  tile's L-face AND R-face are both exposed -- both feed into outer boundary.

Special case: last tower (`j == J-1`). R-face is at the computed region
boundary — the edge of the generated tower grid, which may extend past the
y=x diagonal (grid_spec S7-S8). The last tower's R-face ports that are
unmatched due to being the last tower (no tower j+1 exists) are at the
computed region boundary, NOT the outer boundary. However, outer-staircase
L-face exposure and inner-staircase R-face exposure at the top of the tile
still apply as outer-boundary exposed.

**Spanning check pseudocode:**

```cpp
Verdict spanning_check(
    const uint8_t* tile_ops,
    uint32_t* parent,
    const uint32_t* group_offset,
    const uint32_t* delta,
    const int* tiles_per_tower,
    int J,   // tower count
    int S    // tile side
) {
    std::unordered_set<uint32_t> inner_roots;

    // (a) Row-0 I-face groups -> inner set (horizontal treads)
    for (int j = 0; j < J; j++) {
        int t = tile_index(j, 0);  // row 0
        if (is_dead(t)) continue;
        for (auto G : face_groups(tile_ops, t, FACE_I)) {
            inner_roots.insert(find(global_id(t, G, group_offset), parent));
        }
    }

    // (b) Exposed L-face ports at inner boundary risers (vertical risers)
    for (int j = 1; j < J; j++) {  // skip j=0 (y-axis = grid boundary)
        int H_j = tiles_per_tower[j];
        int d_prev = delta[j - 1];
        int q_prev = d_prev / S;
        int f_prev = d_prev % S;

        // When q_prev > 0: ALL L-face ports of rows 0..q_prev-1
        if (q_prev > 0) {
            for (int r = 0; r < q_prev && r < H_j; r++) {
                int t = tile_index(j, r);
                if (is_dead(t)) continue;
                auto groups = face_groups(tile_ops, t, FACE_L);
                for (int k = 0; k < (int)groups.size(); k++) {
                    uint8_t gid = decode_group_id(groups[k]);
                    if (gid == 0) continue;  // zero-padded L slot
                    inner_roots.insert(find(global_id(t, gid, group_offset), parent));
                }
            }
        }

        // When f_prev > 0: L-face ports of row q_prev with h1 < f_prev
        // (Both conditions apply simultaneously when q_prev > 0 AND f_prev > 0)
        if (f_prev > 0 && q_prev < H_j) {
            int t = tile_index(j, q_prev);
            if (!is_dead(t)) {
                auto groups = face_groups(tile_ops, t, FACE_L);
                auto h1s    = face_h1(tile_ops, t, FACE_L);
                for (int k = 0; k < (int)groups.size(); k++) {
                    uint8_t gid = decode_group_id(groups[k]);
                    if (gid == 0) continue;  // zero-padded L slot
                    if ((int)h1s[k] < f_prev) {
                        inner_roots.insert(find(global_id(t, gid, group_offset), parent));
                    }
                }
            }
        }
    }

    // (c) Check outer boundary: O-face of top-row tiles (horizontal treads)
    for (int j = 0; j < J; j++) {
        int H_j = tiles_per_tower[j];
        int t = tile_index(j, H_j - 1);  // top row
        if (is_dead(t)) continue;

        // O-face groups
        for (auto G : face_groups(tile_ops, t, FACE_O)) {
            if (inner_roots.count(find(global_id(t, G, group_offset), parent))) {
                return Spanning;
            }
        }

        // (d) Inner-staircase R-face risers at outer boundary (from base_y differences)
        if (j < J - 1) {  // skip last tower (computed region boundary)
            int d = delta[j];
            int q = d / S;
            int f = d % S;

            // When q > 0: ALL R-face ports of rows (H_j-q)..H_j-1
            if (q > 0) {
                for (int r = H_j - q; r < H_j; r++) {
                    if (r < 0) continue;
                    int tr = tile_index(j, r);
                    if (is_dead(tr)) continue;
                    auto groups = face_groups(tile_ops, tr, FACE_R);
                    for (int k = 0; k < (int)groups.size(); k++) {
                        uint8_t gid = decode_group_id(groups[k]);
                        if (gid == 0) continue;  // zero-padded R slot
                        if (inner_roots.count(find(global_id(tr, gid, group_offset), parent))) {
                            return Spanning;
                        }
                    }
                }
            }

            // When f > 0: R-face ports of row (H_j-1-q) with h1 >= S - f
            // (Both conditions apply simultaneously when q > 0 AND f > 0)
            if (f > 0 && H_j - 1 - q >= 0) {
                int tr = tile_index(j, H_j - 1 - q);
                if (!is_dead(tr)) {
                    auto groups = face_groups(tile_ops, tr, FACE_R);
                    auto h1s    = face_h1(tile_ops, tr, FACE_R);
                    for (int k = 0; k < (int)groups.size(); k++) {
                        uint8_t gid = decode_group_id(groups[k]);
                        if (gid == 0) continue;  // zero-padded R slot
                        if ((int)h1s[k] >= S - f) {
                            if (inner_roots.count(find(global_id(tr, gid, group_offset), parent))) {
                                return Spanning;
                            }
                        }
                    }
                }
            }
        }

        // (e) Outer-staircase L-face risers (from variable tower height)
        if (j > 0) {
            int H_prev = tiles_per_tower[j - 1];
            if (H_j > H_prev) {
                // Tiles at rows H_prev..H_j-1 have L-faces with no match
                for (int r = H_prev; r < H_j; r++) {
                    int tr = tile_index(j, r);
                    if (is_dead(tr)) continue;
                    auto groups = face_groups(tile_ops, tr, FACE_L);
                    for (int k = 0; k < (int)groups.size(); k++) {
                        uint8_t gid = decode_group_id(groups[k]);
                        if (gid == 0) continue;
                        if (inner_roots.count(find(global_id(tr, gid, group_offset), parent))) {
                            return Spanning;
                        }
                    }
                }
            }
        }

        // (f) Outer-staircase R-face risers (from variable tower height)
        if (j < J - 1) {
            int H_next = tiles_per_tower[j + 1];
            if (H_j > H_next) {
                // Tiles at rows H_next..H_j-1 have R-faces with no match
                for (int r = H_next; r < H_j; r++) {
                    int tr = tile_index(j, r);
                    if (is_dead(tr)) continue;
                    auto groups = face_groups(tile_ops, tr, FACE_R);
                    for (int k = 0; k < (int)groups.size(); k++) {
                        uint8_t gid = decode_group_id(groups[k]);
                        if (gid == 0) continue;
                        if (inner_roots.count(find(global_id(tr, gid, group_offset), parent))) {
                            return Spanning;
                        }
                    }
                }
            }
        }
    }

    return Moat;
}
```

### S6.6 Phase 5: Incremental Spanning Check for Burst Mode (v6)

The campaign runner processes the grid in angular bursts — contiguous
segments of B towers. After each burst, the compositor checks if SPANNING has
been achieved so far, enabling early termination: if spanning is detected
after burst k, the campaign skips all remaining towers for this radius.

#### S6.6.1 Burst Protocol

The campaign runner drives the compositor in this sequence:

```
for each burst b = 0, 1, 2, ...:
    for each tower j in burst b (in order):
        compositor.ingest_tower(j, ...)

    if b is NOT the last burst:
        // Do NOT call collect_outer_boundary for rightmost tower of burst.
        // Its R-face will be matched against the next burst's leftmost tower.

        if compositor.check_spanning_incremental():
            // Early termination. Collect deferred R-face of rightmost tower,
            // then finalize.
            compositor.collect_outer_boundary(j_rightmost)
            result = compositor.finalize()
            return result

    else:  // last burst (j reaches J-1)
        compositor.collect_outer_boundary(J - 1)
        result = compositor.finalize()
        return result
```

#### S6.6.2 Deferred R-Face Boundary Collection

In the standard (non-burst) path, `ingest_tower(j)` collects ALL boundary
exposure for tower j: inner I-face treads, inner L-face risers, outer O-face
treads, AND outer R-face risers. In burst mode, the R-face outer boundary
collection for the rightmost tower of each burst is deferred.

**Why.** The R-face of tower j at x = (j+1)*S borders tower j+1's L-face.
If tower j+1 will be ingested in the next burst, L/R matching will cover
this inter-tower boundary. Collecting the R-face exposure as outer boundary
would incorrectly mark ports as boundary-exposed when they are actually
interior connections waiting to be matched. This would not cause a false
negative (spanning would still be detected eventually), but it would pollute
`outer_members_` with global_ids that may never participate in the actual
outer boundary — wasting memory and `check_spanning_incremental()` time.

More critically, for the staircase riser logic: R-face ports of top-row
tiles with `h1 >= S - f` are outer-exposed only if no higher-row neighbor
exists in tower j+1. Similarly, outer-staircase R-face exposure (from
variable tower height) depends on knowing `tiles_per_tower[j+1]`. That
determination requires knowing whether tower j+1 will be ingested. In burst
mode, the answer depends on whether the current burst is the last one.

**Deferred collection rule:**

| Tower position | R-face outer boundary collected? |
|----------------|----------------------------------|
| Interior tower (not rightmost of burst) | Yes, during `ingest_tower()` — tower j+1 is in the same burst, so `delta[j]` is known and riser exposure can be computed. |
| Rightmost tower of burst, not last tower of grid | No. Deferred until either (a) it becomes an interior tower after the next burst begins, or (b) `collect_outer_boundary(j)` is called on early termination. |
| Last tower of grid (j == J-1) | Yes, via explicit `collect_outer_boundary(J-1)` call. Note: per S6.5, the last tower's R-face unmatched ports are at the computed region boundary, NOT the outer boundary — so only the O-face tread and staircase-riser exposure from `delta[j-1]` apply. |

**`collect_outer_boundary()` pseudocode:**

```cpp
void Compositor::collect_outer_boundary(int32_t j) {
    // O-face of top-row tile (horizontal tread) — already collected during
    // ingest_tower. This method handles R-face riser exposure (both
    // inner-staircase and outer-staircase sources).

    int H_j = tiles_per_tower_[j];

    // --- Inner-staircase R-face risers (from base_y differences) ---
    if (j < J_ - 1) {  // last tower: R-face is computed region boundary
        int d = delta_[j];
        int q = d / S;
        int f = d % S;

        // R-face ports of rows (H_j-q)..H_j-1 when q > 0
        if (q > 0) {
            for (int r = H_j - q; r < H_j; r++) {
                if (r < 0) continue;
                int t = tile_index(j, r);
                if (is_dead(t)) continue;
                auto groups = face_groups(t, FACE_R);
                for (int k = 0; k < (int)groups.size(); k++) {
                    uint8_t gid = decode_group_id(groups[k]);
                    if (gid == 0) continue;
                    outer_members_.insert(global_id(t, gid));
                }
            }
        }

        // R-face ports of row (H_j-1-q) with h1 >= S - f when f > 0
        if (f > 0 && H_j - 1 - q >= 0) {
            int t = tile_index(j, H_j - 1 - q);
            if (!is_dead(t)) {
                auto groups = face_groups(t, FACE_R);
                auto h1s    = face_h1(t, FACE_R);
                for (int k = 0; k < (int)groups.size(); k++) {
                    uint8_t gid = decode_group_id(groups[k]);
                    if (gid == 0) continue;
                    if ((int)h1s[k] >= S - f) {
                        outer_members_.insert(global_id(t, gid));
                    }
                }
            }
        }
    }

    // --- Outer-staircase R-face risers (from variable tower height) ---
    if (j < J_ - 1) {
        int H_next = tiles_per_tower_[j + 1];
        if (H_j > H_next) {
            for (int r = H_next; r < H_j; r++) {
                int t = tile_index(j, r);
                if (is_dead(t)) continue;
                auto groups = face_groups(t, FACE_R);
                for (int k = 0; k < (int)groups.size(); k++) {
                    uint8_t gid = decode_group_id(groups[k]);
                    if (gid == 0) continue;
                    outer_members_.insert(global_id(t, gid));
                }
            }
        }
    }

    // --- Outer-staircase L-face risers (from variable tower height) ---
    if (j > 0) {
        int H_prev = tiles_per_tower_[j - 1];
        if (H_j > H_prev) {
            for (int r = H_prev; r < H_j; r++) {
                int t = tile_index(j, r);
                if (is_dead(t)) continue;
                auto groups = face_groups(t, FACE_L);
                for (int k = 0; k < (int)groups.size(); k++) {
                    uint8_t gid = decode_group_id(groups[k]);
                    if (gid == 0) continue;
                    outer_members_.insert(global_id(t, gid));
                }
            }
        }
    }
}
```

#### S6.6.3 `check_spanning_incremental()` Implementation

After a burst is ingested (with R-face deferred for rightmost tower), check
if spanning has already been achieved:

```cpp
bool Compositor::check_spanning_incremental() {
    // Re-find all collected boundary members to get current roots.
    std::unordered_set<uint32_t> inner_roots;
    for (uint32_t gid : inner_members_) {
        inner_roots.insert(find(gid, parent_));
    }
    for (uint32_t gid : outer_members_) {
        if (inner_roots.count(find(gid, parent_))) {
            return true;
        }
    }
    return false;
}
```

This is identical to the logic in `has_spanning()` and in `finalize()`'s
spanning check. The difference is purely in when it is called and what
boundary members have been collected at that point.

**Cost:** O(|inner_members_| + |outer_members_|) per call, dominated by
`find()` path compression. In practice, inner/outer member sets grow
linearly with the number of ingested towers, so the cost per incremental
check grows as more bursts are ingested.

#### S6.6.4 Mathematical Justification: UF Monotonicity

Union-find is monotonic: `unite(a, b)` only merges equivalence classes.
Once two elements share a root, no subsequent operation can separate them.

**Theorem.** If `check_spanning_incremental()` returns true after burst k,
then `finalize()` after all J towers would also return Spanning.

**Proof.** At burst k, there exist global_ids `g_inner` in `inner_members_`
and `g_outer` in `outer_members_` such that `find(g_inner) == find(g_outer)`.
Ingesting towers k+1..J-1 adds more `unite()` operations to the UF, which
can only merge equivalence classes — never split them. Therefore `g_inner`
and `g_outer` remain in the same equivalence class after all towers are
ingested. `finalize()` re-finds all members, discovers they share a root,
and returns Spanning. QED.

**Corollary.** `check_spanning_incremental()` returning true is a sound early
termination signal. No false positives: if it returns true, spanning exists
in the partial annulus and persists in the full annulus. False negatives are
possible: spanning may exist but require towers from a later burst to
establish the transitive connection. This is expected and handled by
continuing to the next burst.

**Note on deferred R-face.** The deferred R-face does not affect soundness.
When `check_spanning_incremental()` returns true, the spanning path was
established using only the boundary members collected so far (which exclude
the rightmost tower's R-face). The deferred R-face, once collected, can only
ADD outer boundary members — it cannot remove them or break existing
connections.

---

## S7. Edge Cases

### S7.1 Overflow Tiles

**Design invariant: NO overflow tiles reach the compositor.** The caller
(campaign runner or test harness) detects overflow tiles (`tile[0] == 0xFF`)
and reprocesses them via C++ into 256-byte extended TileOps BEFORE calling
the compositor. The compositor only sees normal (128B) and extended (256B)
tiles.

The compositor asserts `!is_overflow(tile)` for every tile it processes.
If this assertion fires, it indicates a bug in the caller — the overflow
tile was not reprocessed.

**Extended tile handling.** Formerly-overflow tiles appear in the
`ExtendedTileSideTable` as 256-byte TileOps with `payload_budget = 253`.
The compositor reads them via `tile_data(t)` which checks the side table,
and parses them with the same offset-header logic as standard tiles (S3.3).
Group labels, h1 encoding, and matching predicates are identical.

K5 sets `bytes[0] = 0xFF` (and fills the entire 128-byte record with 0xFF)
when a tile overflows: either `group_count > 127` (encoding limit) or
`total_ports > TILEOP_PAYLOAD_BYTES` (capacity limit). At operating radii
(R ~ 850M), the census shows 0 overflow tiles. The 256-byte extended
payload (253 bytes) accommodates all observed tile configurations. Overflow
at operating radii would indicate a bug.

### S7.2 Empty Tiles (Zero-Prime)

Detected by `off_I == off_L == off_R == 3` and `tile[3] == 0`. This header
state signals a tile with zero primes in any collar zone. Since tower
generation now extends past y=x (grid_spec S7-S8), sub-diagonal tiles are
processed normally and may contain primes. Empty TileOps are therefore
simply zero-prime tiles -- rare at operating radii but possible for any
tile regardless of position relative to the diagonal.

**Compositor behavior for empty tiles:**

- **UF allocation:** zero UF slots. `max_group_label` returns 0, so the
  prefix-sum contribution is 0: `group_offset[t+1] = group_offset[t]`.
- **I/O matching:** skipped. Both O-face and I-face have zero groups
  (`o_cnt = i_cnt = 0`), so the matching loop body executes zero iterations.
  The adjacent tile's corresponding face also has zero groups on the shared
  boundary (by the I/O count equality proof in S6.2).
- **L/R matching:** skipped. `l_cnt = r_cnt = 0`.
- **Spanning check:** the tile contributes no groups to the inner or outer
  boundary root sets. It is invisible to the spanning verdict.
- **Connectivity impact:** an empty tile is a connectivity gap -- no group
  transits through it. This is correct: if no primes exist in the collar
  zones, no Gaussian-prime path passes through this tile's boundary faces.

### S7.3 Staircase Boundary Geometry

The inner and outer boundaries of the annulus are not smooth arcs at the tile
level -- they form a staircase due to the discrete tower placement along the
inner arc. This staircase creates two types of boundary exposure:

**Horizontal treads:** The I-faces of row-0 tiles (inner boundary) and
O-faces of top-row tiles (row `H_j-1`, outer boundary). These are the
standard boundary faces.

**Vertical risers:** Two staircase sources produce vertical risers:

*Inner staircase (from base_y differences):* At each staircase step (where
`delta[j] > 0`), the L-face or R-face of a boundary tile is partially or
fully exposed to the annulus boundary. These exposed segments are NOT covered
by inter-tower L/R matching because the neighbor tile's corresponding rows
do not exist.

*Outer staircase (from variable tower height):* When adjacent towers differ
in height, the topmost tile(s) of the taller tower have exposed L and/or R
faces with no matching tile on the shorter neighbor.

The full derivation of which ports are boundary-exposed is given in S6.5
(spanning check). The key formulas (`H_j = tiles_per_tower[j]`):

| Source | Boundary | Face | Exposed condition | Ports exposed |
|--------|----------|------|-------------------|---------------|
| — | Inner | I-face of row 0 | Always (horizontal tread) | All I-face groups |
| Inner | Inner | L-face of rows 0..q_prev-1, j > 0 | `q_prev > 0` | ALL L-face ports of those rows |
| Inner | Inner | L-face of row q_prev, j > 0 | `f_prev > 0` | L-face ports with `h1 < f_prev` |
| — | Outer | O-face of row H_j-1 | Always (horizontal tread) | All O-face groups |
| Inner | Outer | R-face of rows (H_j-q)..H_j-1, j < J-1 | `q > 0` | ALL R-face ports of those rows |
| Inner | Outer | R-face of row (H_j-1-q), j < J-1 | `f > 0` | R-face ports with `h1 >= S-f` |
| Outer | Outer | L-face of rows H_prev..H_j-1 | `H_j > H_prev` | ALL L-face ports of those rows |
| Outer | Outer | R-face of rows H_next..H_j-1 | `H_j > H_next` | ALL R-face ports of those rows |

Where `d_prev = delta[j-1]`, `q_prev = d_prev / S`, `f_prev = d_prev % S`
for inner staircase exposure, `d = delta[j]`, `q = d / S`, `f = d % S` for
inner staircase outer exposure, `H_prev = tiles_per_tower[j-1]` and
`H_next = tiles_per_tower[j+1]` for outer staircase exposure. **Both
inner-staircase conditions apply simultaneously when `q > 0 AND f > 0`.**
Empty tiles (S7.2) must be skipped in all boundary collection. L/R face
iteration must skip entries where `decode_group_id(group_byte) == 0`
(zero-padding).

**Region boundary exclusions:**
- j=0, row 0, L-face: y-axis boundary, not annulus boundary
- j=J-1, top row, R-face: computed region boundary, not annulus boundary

---

## S8. Cache Analysis

### S8.1 TileOp Footprint Per Tower

One tower is 32..46 tiles * 128 bytes = 4..5.75 KB of TileOps (variable per
tower). TileOp v2 changes which bytes are hot:

- O/I groups concentrate near the start of each TileOp.
- L/R group bytes and packed `h1` bytes concentrate later in each TileOp.
- There is no co-located UF metadata in the TileOp.

### S8.2 Two-Tower Working Set

During L/R matching between towers `j-1` and `j`, the compositor streams:

- Up to ~11.5 KB of TileOps (two towers at up to 46 tiles each)
- The corresponding UF parent slices in the separate `parent[]` array

The TileOp working set remains L1/L2-friendly. UF traffic is a separate stream.

### S8.3 Cache-Line Layout

```
Cache line 0 (bytes 0-63):
  header + O groups + I groups + early L/R groups

Cache line 1 (bytes 64-127):
  trailing L/R group bytes + L h1 bytes + R h1 bytes + optional pad
```

| Operation | Data accessed | Cache lines touched |
|-----------|---------------|---------------------|
| I/O match | header + O/I groups | usually line 0 only |
| L/R match | header + L/R groups + decoded L/R h1 | 1 or 2 lines depending on offsets |
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

### S10.1 Semantic Equivalence

The v4 compositor is semantically identical to the v3 compositor for all
operations within the first octant:

1. Every inter-tile match still triggers the same group-level union.
2. Every group label still denotes the same intra-tile connectivity class.
3. Header parsing changes addressing only, not the matching predicate.
4. Conservative bridging for overflow tiles is unchanged.
5. 256-byte extended TileOps use the same encoding scheme; only the payload
   budget differs.

Therefore the spanning verdict is unchanged relative to any faithful TileOp v2
producer.

### S10.2 First-Octant Sufficiency

The compositor operates on the first octant {y >= x >= 0} only. No octant
stitching phase is required. This is correct because:

**COLLAR covers one-step crossings.** COLLAR = ceil(sqrt(K_SQ)) >
sqrt(K_SQ) by construction (e.g., COLLAR=7 > sqrt(40) ~ 6.32 for K_SQ=40;
COLLAR=6 = sqrt(36) for K_SQ=36). The sieve domain extends COLLAR lattice
units past each tile face into the collar zone. A Gaussian prime step of
distance <= sqrt(K_SQ) that crosses the y = x diagonal has both endpoints
within sqrt(K_SQ) units of the diagonal. Any tile straddling the diagonal
includes collar zones that extend COLLAR units past its face -- covering the
full one-step crossing distance. The intra-tile union-find captures these
connections.

**Multi-step crossings are negligible.** A multi-step cross-diagonal
excursion would require a chain of primes that: (a) exits a tile's collar
zone into the second octant, (b) traverses through the uncovered strip in the
second octant, and (c) re-enters another tile's collar zone. The uncovered
strip has width approximately `COLLAR - sqrt(K_SQ)` units on each side
(e.g., ~0.68 for K_SQ=40). At operating radii (R ~ 850M), the probability
of such primes forming the ONLY spanning connection (all other paths through
the first octant being disconnected) is negligible.

**Grid termination past the diagonal.** Towers extend past the y = x
diagonal (grid_spec S7-S8). Sub-diagonal tiles are processed normally and
may contain primes. This ensures complete coverage across the diagonal
boundary without relying on a dead-tile predicate.

**Extended tower generation (grid_spec S7-S8).** Tower generation extends
past y=x, so sub-diagonal tiles are processed normally by the compositor.
This provides complete coverage of cross-diagonal paths without any special
stitching logic.

### S10.3 Burst-Mode Early Termination Soundness (v6)

The burst-mode incremental check (S6.6) relies on two properties:

1. **UF monotonicity.** `unite()` only merges equivalence classes. A
   spanning connection detected after burst k survives all subsequent
   `unite()` operations from bursts k+1, k+2, etc. Formally: if
   `find(g_inner) == find(g_outer)` after burst k, then
   `find(g_inner) == find(g_outer)` after any number of additional
   `unite()` calls. This is a structural property of union-find, independent
   of the domain.

2. **Boundary set monotonicity.** `inner_members_` and `outer_members_` are
   append-only sets. Ingesting more towers can only ADD members, never remove
   them. A spanning intersection detected in a subset of the final member
   sets persists in the final member sets.

Together these guarantee: `check_spanning_incremental() == true` implies
`finalize() == Spanning`. The converse does not hold — spanning may require
towers from later bursts to establish the transitive connection.

---

## S11. Memory Budget

| Structure | Size | Notes |
|-----------|------|-------|
| tile_ops | ~11.3 GB | ~88.5M tiles * 128 B (variable towers, avg ~38.5 tiles/tower). Read-only. |
| group_offset | ~354 MB | (~88.5M + 1) * 4 B. |
| UF parent | ~1.77 GB | ~442M groups * 4 B at 5 groups/tile average. |
| delta table | 9 MB | 2.3M * 4 B. |
| base_y table | 18 MB | 2.3M * 8 B. |
| tiles_per_tower | 9 MB | 2.3M * 4 B. Per-tower height metadata. |
| inner_roots HashSet | ~28 MB | Boundary probe set. |
| extended tiles | ~0 B | N_ext * 256 B. Expected 0 at operating radii. |
| **Total** | **~13.5 GB** | |

Variable tower height (32..46 tiles/tower) increases total tile count by
~20% compared to the fixed-32 baseline. The 128-byte record size is
unchanged.

**Extended tile impact.** If N_ext extended tiles exist (reprocessed by C++
fallback), they add N_ext * 256 bytes to the base budget plus the hash table
overhead for the extended tile set and side table. At operating radii, the
census shows 0 overflow tiles, so N_ext = 0 and the memory impact is zero.
Even at extreme radii where a handful of tiles overflow, the additional
memory is negligible (e.g., 1000 extended tiles = 256 KB).

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
| Overflow handling | 256-byte C++ fallback (mandatory pre-processing) | Sub-tile re-tiling, conservative bridge, abort-only | C++ fallback preserves exact connectivity; compositor asserts no overflow |
| Diagonal boundary | First-octant sufficient + extended tower generation past y=x (grid_spec S7-S8) | Octant stitching phase | COLLAR covers one-step crossings; tower generation past y=x handles diagonal |
| Spanning check | Staircase-aware (I/O treads + L/R risers) | I/O faces only | Staircase risers are real boundary exposure that must be checked |
| Burst-mode early termination (v6) | Incremental check + deferred R-face | Check only after all towers | UF monotonicity guarantees soundness; early exit saves campaign time |
| R-face collection timing (v6) | Deferred for rightmost tower of burst | Collect eagerly during ingest | Rightmost R-face borders next burst's L-face; premature collection pollutes outer set |

---

## S14. Resolved Questions

All three open questions from v3 are now closed.

### S14.1 Exposed L/R Boundary Ports -- CLOSED

Staircase-aware derivation in S6.5 and S7.3. Inner exposure: L-face ports
of rows 0..q_prev-1 (ALL ports when `q_prev > 0`) AND L-face ports of
row q_prev with `h1 < f_prev` (when `f_prev > 0`), excluding j=0 (y-axis
boundary). Outer exposure has two sources: (1) inner-staircase R-face risers
from base_y differences — R-face ports of rows (H_j-q)..H_j-1 (ALL ports
when `q > 0`) AND R-face ports of row (H_j-1-q) with `h1 >= S - f` (when
`f > 0`), excluding last tower (computed region boundary); (2) outer-staircase
risers from variable tower height — ALL L-face ports of rows H_prev..H_j-1
when `H_j > H_prev`, ALL R-face ports of rows H_next..H_j-1 when
`H_j > H_next`. Both inner-staircase conditions apply simultaneously when
q > 0 AND f > 0. Dead tiles skipped. L/R face zero-padding
(`group_id == 0`) skipped.

### S14.2 Octant Stitching -- CLOSED

Removed. First-octant composition is sufficient. COLLAR = ceil(sqrt(K_SQ))
covers one-step cross-diagonal paths via intra-tile union-find (e.g.,
COLLAR=7 for K_SQ=40). Extended tower generation past y=x (grid_spec S7-S8)
ensures complete tile coverage across the diagonal. See S10.2 for the full
argument.

### S14.3 L/R h1 Decode Discipline -- CLOSED

Explicit in matching code (S6.3): `group_id = group_byte & 0x7F`,
`h1 = ((group_byte >> 7) << 8) | h1_byte`. Baked into the `face_h1()`
accessor. Comparing stored bytes directly without this decode is invalid
because the group byte carries both the group ID (low 7 bits) and the h1
high bit (bit 7).

---

## S15. Invariants

1. **Overflow sentinel:** `tile[0] == 0xFF` iff the entire TileOp is poisoned.
2. **Empty tile header:** `tile[0] == tile[1] == tile[2] == 3` and `tile[3] == 0`
   iff the tile has no ports on any face.
3. **Offset monotonicity:** for every normal tile, `3 <= off_I <= off_L <= off_R <= 127`
   (standard) or `<= 255` (extended).
4. **Group label range:** all stored group labels lie in `[1, 127]`.
5. **Aligned I/O count agreement:** adjacent tower tiles have equal O/I face counts.
6. **L/R section agreement:** `len(face_groups(L)) == len(face_h1(L))` and
   `len(face_groups(R)) == len(face_h1(R))`.
7. **UF domain:** `global_id(t, G) < total_groups` for every valid normal-tile
   pair `(t, G)`.
8. **UF monotonicity (v6):** once `find(a) == find(b)`, no subsequent
   `unite()` operation can make `find(a) != find(b)`. Spanning detected
   incrementally is permanent.
9. **Adjacent tower height constraint (v8):**
   `|tiles_per_tower[j] - tiles_per_tower[j+1]| <= 1` for all adjacent
   tower pairs. Tower heights change by at most 1 tile per step.
10. **Bump tower dual exposure (v8):** when
    `tiles_per_tower[j] > tiles_per_tower[j-1]` AND
    `tiles_per_tower[j] > tiles_per_tower[j+1]`, the topmost tile of tower j
    has BOTH its L-face and R-face exposed as outer boundary.

---

## S16. Constants Summary

```
TILE_SIDE              = 256
K_SQ                   = <param>   # step distance squared (e.g., 40)
COLLAR                 = ceil(sqrt(K_SQ))  # e.g., 7 for K_SQ=40, 6 for K_SQ=36
TILEOP_SIZE            = 128
TILEOP256_SIZE         = 256
TILEOP_HEADER_BYTES    = 3
TILEOP_DATA_BYTES      = 125
TILEOP256_DATA_BYTES   = 253
OVERFLOW_SENTINEL      = 0xFF
EMPTY_OFFSET           = 3
TILES_PER_TOWER_MIN    = 32
TILES_PER_TOWER_MAX    = 46
MAX_GROUPS_PER_TILE    = 9
```
