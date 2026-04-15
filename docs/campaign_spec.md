---
title: "Campaign Specification v6 — Burst-Mode Pipeline with Parameterized K_SQ"
date: 2026-04-13
version: 6
status: draft
depends:
  - tile_spec.md (v5)
  - grid_spec.md (v6)
  - compositor_spec.md (v9)
---

# Campaign Specification v6 — Burst-Mode Pipeline with Parameterized K_SQ

## S1. Overview

The campaign runner is a single deployable binary that sweeps radii and
determines, for each radius R, whether the annular band [R, R+W] contains a
Gaussian moat at step distance sqrt(K_SQ). It runs on a GPU server and
produces one of three output formats: verdict-only, full tile dump, or dump
with per-radius statistics.

The binary orchestrates a burst-mode pipeline. Instead of processing the
entire octant in one GPU launch, it feeds the octant to the GPU in angular
bursts — contiguous segments of B towers each. While the GPU cooks burst
N+1, the CPU compositor processes burst N. This overlaps GPU and CPU work
and enables early SPANNING termination mid-octant.

Tower j=0 (on the Y-axis) is always processed by C++ reference code before
CUDA bursts begin, due to degenerate axis geometry. Its TileOps are the
first tower ingested by the compositor.

K_SQ is a runtime parameter (not hardcoded to 40), enabling sqrt(36)
campaigns with the same binary.

**Module location:** `tiles-campaign/`

### S1.1 Pipeline Summary

```
  R_start, R_step, K_SQ (input)
    |
    v
  [Outer loop] Radial sweep: R = R_start, R_start + R_step, ...
    |
    v
  [Stage 1] Grid construction for radius R
    |
    v
  [Stage 2] C++ on-axis processing (tower j = 0)
    |
    v
  [Stage 3] Compositor ingests tower j = 0
    |
    v
  [Stage 4] Burst loop: b = 0, 1, 2, ...
    |
    +---> [Stage 4a] CUDA burst b: process towers [j_lo, j_hi)
    |       |
    |       +---> (GPU async: burst b+1 begins while CPU processes burst b)
    |       |
    |       v
    |     [Stage 4b] Overflow scan + C++ fallback for burst b
    |       |
    |       v
    |     [Stage 4c] Compositor ingests burst b towers
    |       |
    |       v
    |     [Stage 4d] Early SPANNING check (if spanning detected, skip remaining bursts)
    |
    v
  [Stage 5] Compositor finalize → verdict
    |
    v
  [Stage 6] Output (verdict_only / dump / dump_with_stats)
```

---

## S2. Parameters

### S2.1 Runtime Parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `R_start` | `i64` | yes | -- | First radius in the sweep |
| `R_step` | `i64` | no | 1 | Radial increment between sweeps |
| `R_end` | `i64` | no | `R_start` | Last radius (inclusive). Single radius if omitted |
| `--k-sq` | `u32` | no | 40 | Step distance squared. Determines COLLAR and CUDA constants |
| `--burst-size` | `u32` | no | 4096 | Towers per CUDA burst (B). Tunable for GPU occupancy |
| `--mode` | enum | no | `verdict_only` | Output mode: `verdict_only`, `dump`, `dump_with_stats` |
| `--dump-path` | path | no | `./campaign-R{R}.bin` | Output path for dump/dump_with_stats modes |
| `--log-level` | enum | no | `info` | `debug`, `info`, `warn`, `error` |
| `--tower-major` | flag | no | off | CUDA outputs flat TileOp[128] arrays in tower-major order (no interleaved metadata) |

### S2.2 Derived Constants

Constants are derived from K_SQ at startup. The CUDA kernels must be
compiled with matching values — either via recompilation per K_SQ or via
template instantiation / `#define` injection.

| Name | Derivation | K_SQ=40 | K_SQ=36 |
|------|------------|---------|---------|
| K_SQ | Input parameter | 40 | 36 |
| COLLAR | ceil(sqrt(K_SQ)) | 7 | 6 |
| S | 256 (fixed) | 256 | 256 |
| TILE_POINTS | S + 1 | 257 | 257 |
| SIDE_EXP | TILE_POINTS + 2 * COLLAR | 271 | 269 |
| W_RADIAL | TILES_PER_TOWER_MIN * S | 8192 | 8192 |
| TILES_PER_TOWER_MIN | 32 (fixed) | 32 | 32 |
| TILES_PER_TOWER_MAX | 46 (fixed) | 46 | 46 |
| TILEOP_SIZE | 128 (fixed) | 128 | 128 |
| TILEOP_EXT_SIZE | 256 (fixed) | 256 | 256 |
| TILEOP_HEADER_BYTES | 3 (fixed) | 3 | 3 |
| TILEOP_PAYLOAD_BYTES | 125 | 125 | 125 |
| TILEOP_EXT_PAYLOAD_BYTES | 253 | 253 | 253 |
| OVERFLOW_SENTINEL | 0xFF (fixed) | 0xFF | 0xFF |
| SIEVE_LIMIT | 10000 (fixed) | 10000 | 10000 |

### S2.3 CUDA Constant Consistency

The campaign binary validates at startup that the loaded CUDA kernel was
compiled with COLLAR, SIDE_EXP, and K_SQ values matching the runtime
parameters. Approaches:

1. **Template instantiation:** compile kernels for each supported K_SQ
   (40, 36) and select at runtime via function pointer.
2. **Define injection:** compile with `-DK_SQ=N -DCOLLAR=M -DSIDE_EXP=P`
   and embed a device-side constant for the host to query.
3. **Recompilation:** JIT or offline recompile on K_SQ change.

The binary must abort with a clear diagnostic if CUDA constants do not
match runtime K_SQ. Mismatched constants produce silent wrong results.

**Backward offset table:** The CUDA kernel's backward-neighbor lookup
table (which Gaussian integer offsets can reach a tile from a neighboring
tile) depends on K_SQ. This table must also be consistent with the runtime
parameter.

---

## S3. Grid Construction

Stage 1 builds the grid geometry for a given radius R. This follows
grid_spec.md S4 and S11 exactly.

**Grid construction mode:** The campaign runner uses `compute_grid(R)` with
accumulated-correction rounding (grid_spec S4.2). This is the production
mode that generates deterministic tower placement. Test harnesses compositing
existing CUDA output (e.g., from `gen_coords.py`) use
`compute_grid_from_coords(R, coords, n_tiles)` instead, which extracts
`base_y` directly from tile coordinates. These two modes produce different
`base_y` values — the compositor must use the same grid that generated the
tiles.

### S3.1 Tower Geometry

Towers are indexed j = 0, 1, 2, ... from the y-axis rightward. Tower j has:

```
base_x[j] = j * S
base_y[j] = computed via accumulated-correction rounding (S3.2)
```

Tile (j, r) occupies the lattice square:

```
x in [j * S,  j * S + S]
y in [base_y[j] + r * S,  base_y[j] + (r + 1) * S]
```

with the origin corner at (j * S, base_y[j] + r * S).

### S3.2 Accumulated-Correction Rounding

```cpp
void compute_grid(int64_t R, int64_t S,
                  std::vector<int64_t>& base_y,
                  std::vector<uint32_t>& tiles_per_tower) {
    double err = 0.0;
    for (int64_t j = 0; ; ++j) {
        int64_t x_j = j * S;
        // i128 overflow check
        __int128 x2 = (__int128)x_j * x_j;
        __int128 R2 = (__int128)R * R;
        if (x2 > R2) break;

        double y_cont = std::sqrt((double)(R2 - x2));
        int64_t y_j = std::llround(y_cont - err);
        err += (double)y_j - y_cont;

        // Variable tower height: ceil(32 / cos(theta_j))
        // cos(theta_j) = base_y[j] / R, but use exact form:
        //   cos(theta_j) = y_j / sqrt(x_j^2 + y_j^2)
        double cos_theta = (double)y_j / std::sqrt((double)(x2 + (__int128)y_j * y_j));
        uint32_t tpt = (uint32_t)std::ceil(32.0 / cos_theta);
        if (tpt < 32) tpt = 32;
        if (tpt > 46) tpt = 46;

        // Termination: tower extends past y=x by MARGIN = 2*S
        if (y_j + (int64_t)tpt * S + 2 * S <= x_j) break;

        base_y.push_back(y_j);
        tiles_per_tower.push_back(tpt);
    }
}
```

**Invariant:** |err| <= 0.5 at all times. The accumulated correction
distributes rounding error evenly, preventing drift between the discrete
tower base and the continuous arc.

**Precision note:** R^2 at R = 870M is ~7.6e17, which exceeds f64's 53-bit
mantissa. The subtraction R^2 - x_j^2 is computed in i128 before conversion
to f64 for sqrt. Near the diagonal (x_j ~ R/sqrt(2)), the subtraction
result is ~R^2/2 ~ 3.8e17 (59 bits) — f64 sqrt introduces at most a few
ULP of error, absorbed by the correction mechanism. For production hardening,
integer Newton's method sqrt on the i128 value eliminates all floating-point
concerns.

### S3.3 Tower Termination

Towers are generated past the y = x diagonal until the termination
predicate is satisfied:

```
terminated(j) := base_y[j] + tiles_per_tower[j] * S + MARGIN <= j * S
```

where MARGIN = 2 * S = 512. The first tower that satisfies this predicate
is NOT included. Let J denote the total tower count.

This extends generation past the diagonal by MARGIN, ensuring cross-diagonal
coverage. Sub-diagonal tiles are processed by CUDA and provide connectivity
via standard face matching. See S4.3 and grid_spec S7-S8 for the full
rationale.

At R = 830M: J ~ 2,293,130 towers (marginally more than first-octant-only),
total tiles = sum(tiles_per_tower[j]) ~ 84-88M (approximately 15-20% more
than the fixed-32 baseline of 73.4M, due to the gradual height ramp — most
towers near the Y-axis stay at 32, only towers approaching 45° reach 46).
The extra towers from extended generation add ~0.01% to total tile count.

### S3.4 Delta Table

For each adjacent tower pair (j, j+1):

```
delta[j] = base_y[j] - base_y[j+1]
```

Properties:
- delta[j] >= 0 for all j (arc descends rightward in the first octant)
- Monotonically non-decreasing (arc slope steepens)
- Near y-axis: delta ~ 0. Near diagonal: delta ~ S = 256
- Storage: J-1 entries, u32 each (~9 MB at operating radii)

### S3.5 Tower Termination (All Tiles Live)

There is no dead-tile predicate. All tiles in generated towers are
processed by CUDA (or C++ for tower j = 0). Tower generation continues
past the y = x diagonal until the termination predicate (S3.3) is
satisfied, and every tile in every generated tower (r = 0 through
tiles_per_tower[j]-1) is sieved, compacted, and encoded.

Empty TileOps (off_I = off_L = off_R = 3, zero payload) may still occur
rarely for tiles with zero primes, but this is not tied to the diagonal —
it reflects genuine prime absence in that lattice square.

### S3.6 Grid Output

```cpp
struct Grid {
    int64_t R;                    // inner radius
    int32_t S;                    // tile side = 256
    int32_t W_RADIAL;              // guaranteed radial depth = 8192
    int32_t tiles_per_tower_min;  // = 32
    int32_t tiles_per_tower_max;  // = 46
    int32_t num_towers;           // = J
    uint32_t K_SQ;                // step distance squared
    uint32_t COLLAR;              // ceil(sqrt(K_SQ))
    std::vector<int64_t> base_y;           // J entries
    std::vector<uint32_t> delta;           // J-1 entries
    std::vector<uint32_t> tiles_per_tower; // J entries, range [32, 46]
};
```

---

## S4. Tile Source Routing

The campaign routes tiles to one of three processing paths based on tower
index and tile status.

### S4.1 Routing Table

| Condition | Processor | Reason |
|-----------|-----------|--------|
| j = 0 | C++ (Stage 2) | On-axis sieve correction required |
| j > 0 | CUDA (Stage 4a) | Bulk GPU processing in bursts |
| CUDA overflow (bytes[0] == 0xFF) | C++ fallback (Stage 4b) | Poisoned tile reprocessing |

### S4.2 Tower j=0 Exclusion from CUDA

The CUDA coordinate generator MUST skip tower j = 0. This is a hard
invariant, not an optimization.

**Rationale:** Tower j = 0 tiles have base_x = 0, so their left face sits
on the imaginary axis (a = 0 column). The standard inert-prime sieve
incorrectly classifies certain on-axis Gaussian primes as composite:

For a Gaussian integer bi where b is an inert rational prime (b = 3 mod 4),
the sieve marks lattice point (0, b) as composite because b divides both
coordinates: b | 0 and b | b. But bi is a Gaussian prime (it is an
associate of b, which remains prime in Z[i] because b is inert). The
standard sieve has no special-case logic for the a = 0 column, so these
primes are silently dropped.

The C++ tower-0 processor applies a post-sieve correction (S6.2) that
restores these points. This correction is trivial (~32 affected tiles at
operating radii) and executes in negligible time.

### S4.3 Extended Tower Generation for Cross-Diagonal Coverage

Tower generation extends past the y = x diagonal by MARGIN = 2 * S = 512
(S3.3). Sub-diagonal tiles are processed by CUDA through the standard
burst pipeline — no separate C++ infill band is needed.

Cross-diagonal connectivity is provided by standard face matching between
adjacent towers in the sub-diagonal region. The COLLAR overlap captures
one-step cross-diagonal connections (sqrt(40) ~ 6.32 < 7, sqrt(36) = 6
<= 6), and the extended tower generation ensures that multi-step
cross-diagonal paths through sub-diagonal tiles are discovered by the
compositor's incremental union-find.

See grid_spec S7-S8 for the full rationale on cross-diagonal coverage
geometry and the MARGIN derivation.

---

## S5. CUDA Burst Processing

Stage 4a processes tiles in angular bursts. Each burst covers a contiguous
range of towers.

### S5.1 Burst Geometry

The octant towers j = 1 through J-1 are partitioned into bursts of B
towers each (B = `--burst-size`, default 4096):

```
Burst 0: towers [1,          min(1+B, J))
Burst 1: towers [1+B,        min(1+2B, J))
Burst 2: towers [1+2B,       min(1+3B, J))
...
Burst k: towers [1+k*B,      min(1+(k+1)*B, J))
```

Total burst count: ceil((J - 1) / B).

At R = 830M with B = 4096: ~560 bursts, each covering
sum(tiles_per_tower[j] for j in burst) tiles (variable per burst due to
tower height ramp; ranges from ~131K near Y-axis to ~189K near 45°).

### S5.2 GPU/CPU Overlap

The burst loop overlaps GPU and CPU work:

```
                 time --->
GPU:  [burst 0 compute] [burst 1 compute] [burst 2 compute] ...
CPU:              [tower 0] [burst 0 compose] [burst 1 compose] ...
```

Implementation:

```cpp
// Launch burst 0 on GPU (async)
cuda_launch_burst(0, stream);

for (int b = 0; b < num_bursts; ++b) {
    // Wait for burst b to finish on GPU
    cudaStreamSynchronize(stream);

    // Launch burst b+1 on GPU (async), if not last
    if (b + 1 < num_bursts) {
        cuda_launch_burst(b + 1, stream);
    }

    // CPU: overflow scan + fallback for burst b
    scan_overflow_burst(b);
    cpp_fallback_burst(b);

    // CPU: compositor ingests burst b towers
    for (int j = burst_j_lo(b); j < burst_j_hi(b); ++j) {
        compositor.ingest_tower(j, tower_tileops(j), &extended);
    }

    // Early spanning check — incremental, does not finalize
    if (compositor.check_spanning_incremental()) {
        // SPANNING confirmed — finalize and skip remaining bursts
        cancel_gpu_if_running(stream);
        compositor.finalize();
        result.verdict = SPANNING;
        break;
    }

    // NOTE: The rightmost tower of this burst does NOT get
    // collect_outer_boundary() yet — it is only called once the
    // next burst confirms this tower is not the final tower, or
    // after all bursts complete (see post-loop finalization below).
}

// Post-loop: if no early SPANNING, finalize the last tower's outer boundary
if (result.verdict != SPANNING) {
    compositor.collect_outer_boundary(J - 1);
    compositor.finalize();
}
```

### S5.3 Tower-Major TileOp Output

When `--tower-major` is set (the default for campaign mode), the CUDA
kernel writes flat `TileOp[128]` arrays in tower-major order with no
interleaved metadata. Each burst outputs:

```
burst_tile_ops: uint8[sum(tiles_per_tower[j] for j in burst) * 128]
```

Indexed via cumulative offset: for tower j, tile r starts at
`burst_tile_ops[tile_offset(j) * 128]` where `tile_offset(j)` is the
prefix sum of `tiles_per_tower` within the burst. No `a_lo`, `b_lo`,
`prime_count`, or other metadata between tiles. This is the format the
compositor consumes directly via `ingest_tower()`.

Buffer sizing uses `TILES_PER_TOWER_MAX = 46` for per-tower allocations
to guarantee sufficient space regardless of tower height.

The campaign binary uses double-buffering: two burst output buffers
alternate between GPU write target and CPU read source.

### S5.4 Coordinate Generation

Within each burst, the CUDA kernel generates tile coordinates from the Grid:

```cpp
// One CUDA block per tile. Tiles are laid out contiguously in tower-major
// order; the burst launcher computes a prefix sum of tiles_per_tower[j]
// for towers in [j_lo, j_hi) to map flat_idx -> (j, r).
int flat_idx = blockIdx.x;

// Binary search or lookup table maps flat_idx to tower offset within burst
int j_offset = tower_of_flat_idx(flat_idx, burst_prefix_sum);
int j = j_lo + j_offset;
int r = flat_idx - burst_prefix_sum[j_offset]; // row within tower

// r must be < tiles_per_tower[j]. Tower termination (S3.3) ensures only
// valid towers are in the burst; variable height ensures only valid rows.

// Tile origin
TileCoord coord;
coord.a_lo = j * S;                     // real axis
coord.b_lo = base_y[j] + r * S;         // imaginary axis
```

### S5.5 Per-Tile Pipeline

Each CUDA block executes the 5-kernel pipeline (tile_spec.md):

1. **Sieve** — mark composite Gaussian integers in the SIDE_EXP x SIDE_EXP bitmap
2. **Compact** — extract surviving primes into coordinate arrays
3. **Union-find** — connect primes within sqrt(K_SQ) distance
4. **Face-extract** — identify boundary ports on all four faces
5. **Prune + Encode** — dead-end pruning, pack into TileOp v2 (128 bytes)

If the tile overflows the 125-byte payload after pruning, the kernel writes
OVERFLOW_SENTINEL (0xFF) to all 128 bytes.

### S5.6 Burst Memory Budget

Each burst occupies a bounded GPU memory footprint:

| Structure | Size (B=4096) | Notes |
|-----------|---------------|-------|
| Burst output buffer | 23 MB | 4096 * 46 * 128 B (worst-case, TILES_PER_TOWER_MAX) |
| Double buffer (x2) | 46 MB | Two alternating output buffers |
| Grid base_y slice | 32 KB | B * 8 B (or full grid, 18 MB) |
| Sieve working memory | ~32 MB | Per-block shared memory |
| **Total GPU per burst** | **~78 MB** | Plus full grid if cached |

This is dramatically smaller than the v2 monolithic allocation (9.4 GB).
The full grid geometry (base_y, delta) can be uploaded once and retained on
GPU for the entire radius.

### S5.7 Performance

Reference throughput on RTX 4090: ~155K tiles/s. At B = 4096 towers:
131K-189K tiles per burst (varying with tower height), each burst completes
in ~0.85-1.2s on GPU. CPU compositor time per burst is expected to be
comparable, achieving good overlap.

At R = 830M: ~560 bursts * ~0.85s = ~8 minutes GPU time. CPU compositor
runs in parallel, so total wall time is approximately max(GPU, CPU) rather
than sum.

---

## S6. On-Axis C++ Processing

Stage 2 processes the tiles_per_tower[0] tiles in tower j = 0 (always 32,
since tower j = 0 sits on the Y-axis where cos(theta) = 1) using the C++
reference tile processor with a sieve correction pass. Tower j = 0 is
processed before CUDA bursts begin and is the first tower ingested by the
compositor.

### S6.1 Standard Processing

Tower j = 0 tiles are processed through the same 5-phase pipeline as CUDA
tiles (sieve, compact, union-find, face-extract, prune-encode), producing
standard 128-byte TileOp v2 records. The C++ implementation is
byte-identical to CUDA output for non-axis tiles (validated by
tile-compare).

### S6.2 Sieve Correction for a = 0 Column

After the standard sieve phase, the C++ processor applies a correction to
the imaginary axis column within each tower-0 tile:

```cpp
void correct_axis_sieve(uint32_t* bitmap, TileCoord coord, uint32_t collar) {
    // Only applies when a_lo = 0 (tower j = 0)
    if (coord.a_lo != 0) return;

    // Column a = 0 in tile-local coordinates is at col_offset = COLLAR
    int col = collar;

    for (int row = 0; row < SIDE_EXP; ++row) {
        int64_t b = coord.b_lo - collar + row;
        if (b <= 0) continue;

        // Check if |b| is an inert rational prime (b ≡ 3 mod 4)
        if (b % 4 != 3) continue;
        if (!is_rational_prime(b)) continue;

        // The point (0, b) is a Gaussian prime — restore it as candidate
        int bit_idx = row * SIDE_EXP + col;
        bitmap[bit_idx / 32] |= (1u << (bit_idx % 32));
    }
}
```

**Which points are affected:** lattice points (0, b) where b is a positive
rational prime with b = 3 mod 4. These are the inert primes in Z[i] — they
do not split, and bi is a Gaussian prime (associate of b). The standard
sieve marks (0, b) as composite because b divides both the real part (0)
and the imaginary part (b). The correction restores these points.

**Scope:** at operating radii (R ~ 830M), tower j = 0 spans b values from
~R to ~R + W = R + 8192. By the prime number theorem, there are roughly
8192 / ln(830e6) ~ 400 primes in this range, of which roughly half are
= 3 mod 4. So ~200 corrections across all 32 tiles. Negligible compute
cost.

### S6.3 Output Format

```
tile_ops_axis: TileOp[tiles_per_tower[0]]    // always 32 * 128 = 4096 bytes (tower 0 on Y-axis)
```

Standard TileOp v2 format. These are fed to the compositor as tower j = 0
before the burst loop begins.

### S6.4 Performance

The C++ processor runs at ~1,000 tiles/s on a single core. 32 tiles
complete in ~32ms. This stage is negligible in the pipeline.

---

## S7. Overflow Handling

After each CUDA burst completes, overflow tiles are detected and
reprocessed by C++ before the burst's towers are ingested by the compositor.

### S7.1 Detection

Scan the burst output buffer for overflow sentinels:

```cpp
void scan_overflow_burst(int burst_idx,
                         const uint8_t* burst_ops,
                         const Grid& grid,
                         std::vector<OverflowEntry>& overflow) {
    int j_lo = burst_j_lo(burst_idx);
    int j_hi = burst_j_hi(burst_idx);
    int flat = 0;
    for (int j = j_lo; j < j_hi; ++j) {
        for (uint32_t r = 0; r < grid.tiles_per_tower[j]; ++r, ++flat) {
            if (burst_ops[flat * 128] == OVERFLOW_SENTINEL) {
                overflow.push_back({j, (int)r, flat});
            }
        }
    }
}
```

### S7.2 C++ Reprocessing

Each overflow tile is reprocessed by the C++ tile processor using a 256-byte
TileOp format. The processing pipeline is identical (sieve, compact,
union-find, face-extract, prune-encode); only the encode phase differs,
targeting the larger payload.

**No sub-tiles. No 128x128 splitting.** The tile is reprocessed at the same
256x256 granularity. The extended format simply provides more payload space.

### S7.3 256-Byte Extended TileOp Format

See S11 for the full layout specification. Summary: same 3-byte offset
header, 253-byte payload. Same encoding logic (O-I-L-R face packing, group
byte + h1 byte for L/R faces). The extended budget is:

```
o_cnt + i_cnt + 2*l_cnt + 2*r_cnt <= 253
```

This accommodates all observed tile configurations at operating radii. The
validated maximum is ~9 groups per tile after pruning, far below the 253-byte
budget.

### S7.4 Abort on Double Overflow

If any tile overflows even the 256-byte extended format, the campaign binary
aborts with a diagnostic message:

```
ABORT: tile (j={j}, r={r}) overflows 256-byte TileOp.
  prime_count={n}, group_count={g}, ports: O={o} I={i} L={l} R={r}
  payload_needed={p} > 253
This indicates a bug — census data shows max ~9 groups/tile at operating radii.
```

At operating radii, this condition should never trigger. If it does, it
indicates a bug in the sieve, union-find, or pruning stages — not a
legitimate density spike.

### S7.5 Integration with Burst Loop

Overflow handling is per-burst, not deferred to end-of-octant. After each
burst's GPU output is available:

1. Scan burst output for overflow sentinels
2. Reprocess overflow tiles with C++ (256-byte TileOps)
3. Update the extended side table with new entries
4. The compositor ingests the burst towers, reading extended TileOps from
   the side table for overflow tiles

This ensures the compositor always has resolved data for every tile it
ingests. No unresolved overflow tiles reach the compositor.

### S7.6 Expected Volume

The census at R = 860M shows 0 poisoned tiles across 490K+ sampled tiles.
At operating radii, overflow is not expected. The fallback exists as a
safety net for extreme conditions or unforeseen density spikes. If any
poisoned tiles do occur, reprocessing at ~1,000 tiles/s is negligible
and does not block the burst pipeline.

---

## S8. Early SPANNING Termination

Union-find is monotonic — adding towers can only merge groups, never split
them. This enables early termination for SPANNING verdicts.

### S8.1 Monotonicity Principle

After each burst, the compositor's UF state represents all connectivity
discovered so far. Adding more towers (from subsequent bursts) can only
perform additional union operations, never undo existing ones. Therefore:

- If any UF root appears in both the inner boundary set and the outer
  boundary set after burst b, SPANNING is confirmed. No subsequent burst
  can disconnect these groups.
- If no root appears in both sets, SPANNING may still emerge from later
  bursts. Processing must continue.

### S8.2 Boundary Tracking

The compositor maintains two sets during incremental ingestion
(compositor_spec S1.2):

- `inner_members_`: global_ids of UF elements that touch the inner
  boundary (r = 0 tiles, face I)
- `outer_members_`: global_ids of UF elements that touch the outer
  boundary (r = tiles_per_tower[j]-1 tiles, face O)

Plus staircase boundary exposure (horizontal treads and vertical risers)
as defined in compositor_spec.

### S8.3 Check Protocol

After each burst's towers are ingested:

```cpp
if (compositor.check_spanning_incremental()) {
    // SPANNING confirmed at burst b
    // Record burst index for statistics
    spanning_burst = b;
    cancel_gpu_if_running(stream);
    compositor.finalize();
    result.verdict = SPANNING;
    break;
}
```

`check_spanning_incremental()` returns bool without finalizing. It
re-finds all boundary members to get current roots, then checks for
intersection. Cost is O(|inner| + |outer|) per call. At operating radii,
boundary sets are bounded by the number of tiles on the inner/outer edges
(~J tiles each), so each check is O(J).

On early SPANNING detection, `finalize()` is called once to seal the
compositor state. If the loop completes without early exit,
`collect_outer_boundary(J-1)` is called for the final tower, then
`finalize()` is called once.

### S8.4 MOAT Requires Full Octant

Early termination applies ONLY to SPANNING. A MOAT verdict requires
processing the entire octant — every tower, every burst — because the
absence of spanning can only be confirmed when all connectivity has been
discovered. There is no shortcut for MOAT.

### S8.5 Expected Impact

At radii where SPANNING holds (which is expected for all tested radii so
far), early termination may skip a significant fraction of bursts. The
spanning component often emerges well before the diagonal, particularly
for smaller step distances where prime density provides rich connectivity.

---

## S9. Compositor Invocation

The compositor is invoked incrementally, tower by tower, throughout the
burst loop.

### S9.1 Ingestion Order

```
1. compositor.init(grid)
2. compositor.ingest_tower(0, tile_ops_axis, &extended)     // tower j=0
3. for each burst b:
     for j in [burst_j_lo(b), burst_j_hi(b)):
       compositor.ingest_tower(j, tower_tileops(j), &extended)
     if compositor.check_spanning_incremental():
       compositor.finalize()  →  SPANNING (early exit)
4. compositor.collect_outer_boundary(J-1)   // final tower's outer boundary
5. compositor.finalize()  →  CompositorResult
```

Towers MUST be ingested in order j = 0, 1, 2, ..., J-1. The compositor
relies on sequential tower ordering for L/R face matching (it retains
the previous tower's tile data for matching). The compositor receives
`tiles_per_tower[j]` as part of grid metadata and uses it to determine
the tile count per tower during ingestion.

### S9.2 Compositor Behavior with Extended Tiles

The compositor follows compositor_spec.md v9. **No overflow tiles reach the
compositor.** Stage 7 (S7.5) reprocesses ALL overflow tiles into 256-byte
extended TileOps before compositor ingestion. The compositor asserts
`!is_overflow(tile)` for every tile.

Extended tiles (formerly-overflow) appear in the `ExtendedTileSideTable`.
The compositor reads them via `tile_data(t)`, which checks the side table
and returns the 256-byte data. Parsing uses `payload_budget = 253` instead
of 125. Group labels, h1 encoding, and matching predicates are identical
to standard tiles.

There is no conservative bridging path. If an overflow tile is not
reprocessed (which should never happen), the compositor asserts and aborts.

### S9.3 Compositor Output

```cpp
struct CompositorResult {
    enum Verdict { SPANNING, MOAT };
    Verdict verdict;

    // Diagnostics
    uint64_t total_tiles;
    uint64_t overflow_tiles;
    uint64_t extended_tiles;         // overflow tiles with extended TileOps (all resolved)
    uint64_t total_groups;           // UF elements allocated
    uint64_t total_unions;           // UF union operations performed
    double   compositor_seconds;     // wall-clock time for composition
};
```

---

## S10. Radial Sweep

The campaign sweeps radii sequentially from R_start outward.

### S10.1 Sweep Loop

```cpp
for (int64_t R = R_start; R <= R_end; R += R_step) {
    Grid grid = compute_grid(R, K_SQ);
    Compositor compositor;
    compositor.init(grid);

    // Stage 2: tower j=0
    auto axis_ops = cpp_process_tower0(grid);
    compositor.ingest_tower(0, axis_ops.data(), &extended);

    // Stage 4: burst loop
    RadiusResult result = run_burst_loop(grid, compositor);

    // Stage 5: finalize
    if (result.verdict == UNKNOWN) {
        result = compositor.finalize();
    }

    // Stage 6: output
    emit_output(R, result, mode);
}
```

### S10.2 Sequential Scan

Radii are scanned sequentially. Binary search is NOT possible because
spanning is non-monotone in R: a radius may be spanning while a larger
radius is a moat, or vice versa. Each radius is an independent computation.

### S10.3 Independence

Each radius R produces an independent Grid, independent CUDA bursts, and
an independent compositor instance. No state carries across radii. The
compositor is destroyed and re-initialized for each radius.

---

## S11. Output Modes

Stage 6 produces output according to the `--mode` flag. Three modes are
supported.

### S11.1 verdict_only (default)

Prints a structured result to stdout per radius:

```
=== Campaign Result: R = 830000000, K_SQ = 40 ===
Verdict:         MOAT
Band:            [830000000, 830008192]

--- Grid ---
Towers:          2293130
Total tiles:     ~85000000 (variable tower heights, 32-46 tiles/tower)

--- Processing ---
Burst size:      4096 towers
Total bursts:    560
Bursts processed: 560 (full octant)
Axis tiles:      32 (tower 0, C++, always 32)
Overflow tiles:  0

--- Timing ---
Grid build:      0.42s
C++ axis:        0.03s
GPU bursts:      473.81s
CPU compositor:  468.12s (overlapped)
Overflow reproc: 0.00s
Total wall:      478.50s
```

For SPANNING verdicts, the output includes the burst at which spanning
was detected:

```
Verdict:         SPANNING
Spanning burst:  142 of 560 (25.4% of octant)
Bursts skipped:  418
```

Exit code: 0 for MOAT, 1 for SPANNING, 2 for error.

### S11.2 dump

Writes the full merged tile data to a binary file for archival/debugging:

```
File layout:
  [Header]
    magic:         uint32 = 0x474D4F41   ("GMOA")
    version:       uint32 = 2
    R:             int64
    K_SQ:          uint32
    J:             uint32  (num_towers)
    total_tiles:   uint32  (sum of tiles_per_tower[j])
    extended_count: uint32

  [base_y table]
    base_y:        int64[J]

  [delta table]
    delta:         uint32[J-1]

  [tiles_per_tower table]
    tiles_per_tower: uint32[J]

  [TileOp array]
    tile_ops:      uint8[total_tiles * 128]

  [Extended tile index]
    flat_indices:  uint32[extended_count]

  [Extended TileOps]
    extended_ops:  uint8[extended_count * 256]
```

The dump file is self-contained: it includes the grid geometry, all
standard TileOps, and all extended TileOps. A separate verification tool
can load the dump, re-run the compositor, and confirm the verdict.

Note: In burst mode, the full TileOp array is assembled incrementally
from burst output buffers. For dump mode, each burst's output is appended
to the output file (or accumulated in host memory if it fits). Dump mode
disables early SPANNING termination — the full octant must be processed
to produce a complete dump.

### S11.3 dump_with_stats

Same binary dump as S11.2, plus a companion stats file
(`{dump-path}.stats.json`) with per-radius statistics:

```json
{
  "R": 830000000,
  "K_SQ": 40,
  "verdict": "MOAT",
  "tile_count": 85000000,
  "overflow_count": 0,
  "total_groups": 366895412,
  "total_unions": 289441007,
  "wall_time_s": 478.50,
  "gpu_time_s": 473.81,
  "compositor_time_s": 468.12,
  "burst_size": 4096,
  "total_bursts": 560,
  "spanning_burst": null,
  "bursts_processed": 560
}
```

If SPANNING was detected early, `spanning_burst` is the 0-indexed burst
number at which it was confirmed, and `bursts_processed` is less than
`total_bursts`.

---

## S12. 256-Byte Extended TileOp Format

### S12.1 Layout

The extended TileOp uses the same encoding as the standard 128-byte TileOp
v2 (tile_spec S4.1) with a larger payload:

```
TileOp256 [256 bytes = 4 cache lines]

Header:
  Byte 0: off_I   // byte offset where Face I groups begin
  Byte 1: off_L   // byte offset where Face L groups begin
  Byte 2: off_R   // byte offset where Face R groups begin

Payload:
  Bytes 3 .. off_I - 1                 Face O groups   (o_cnt bytes)
  Bytes off_I .. off_L - 1             Face I groups   (i_cnt bytes)
  Bytes off_L .. off_R - 1             Face L group bytes (l_cnt bytes)
  Bytes off_R .. off_R + r_cnt - 1     Face R group bytes (r_cnt bytes)
  Bytes h_start .. h_start + l_cnt - 1 Face L h1 bytes    (l_cnt bytes)
  Bytes h_start + l_cnt .. 255         Face R h1 bytes    (r_cnt bytes, optional pad)

where:
  h_start = off_R + r_cnt
```

### S12.2 Count Derivation

The compositor derives counts from the offsets using the extended payload
size:

```
o_cnt = off_I - 3
i_cnt = off_L - off_I
l_cnt = off_R - off_L
r_cnt = (253 - o_cnt - i_cnt - 2*l_cnt) >> 1
```

The floor shift handles the optional pad byte at byte 255, identical to
the standard format's treatment of byte 127.

### S12.3 Data Budget

```
o_cnt + i_cnt + 2*l_cnt + 2*r_cnt <= 253
```

Capacity at uniform distribution: 6*N <= 253, so N = 42 ports per face.
This exceeds the Brun-Titchmarsh raw prime cap (~20 per face at operating
radii) and the validated post-pruning maximum (~11 per face).

### S12.4 Encoding Differences from Standard TileOp

None. The only difference is the size of the backing array (256 vs 128
bytes). The header format, face ordering (O-I-L-R), group byte encoding,
h1 byte encoding, group-bit steal for L/R 9th bit, dead-end pruning rule,
and overflow sentinel are all identical.

### S12.5 Identifying Extended TileOps

Extended TileOps are NOT stored in the flat tile_ops array. They live in
the side table (S8 of the merge logic). The compositor identifies a tile as
extended solely via the `is_extended[flat_index]` bitset. Extended tiles
have valid headers (not 0xFF) — identification by sentinel is incorrect.
The extended TileOp is then read from the side table at 256-byte width.

**The compositor MUST use the payload-size parameter (128 or 256) when
deriving r_cnt.** The formula uses the total payload (125 or 253) as the
budget. All other parsing logic is width-agnostic.

---

## S13. Memory Budget

### S13.1 GPU Memory (Burst Mode)

| Structure | Size | Notes |
|-----------|------|-------|
| Burst output buffers (x2) | 46 MB | 2 * 4096 * 46 * 128 B (double-buffered, worst-case TILES_PER_TOWER_MAX) |
| Grid base_y | 18 MB | 2.3M * 8 B (full grid, uploaded once) |
| Grid delta | 9 MB | 2.3M * 4 B |
| Grid tiles_per_tower | 9 MB | 2.3M * 4 B |
| Sieve working memory | ~32 MB | Per-block shared memory |
| **Total GPU** | **~114 MB** | |

Burst mode reduces GPU memory from 9.4 GB (v2 monolithic) to ~114 MB.
This fits on any CUDA-capable GPU with at least 256 MB of memory.

### S13.2 Host Memory (Compositor + Burst Buffers)

| Structure | Size | Notes |
|-----------|------|-------|
| Burst output buffer (host) | 23 MB | Current burst being composited (worst-case 46 tiles/tower) |
| Tower j=0 TileOps | 4 KB | 32 * 128 B (tower 0 always 32 tiles) |
| Extended side table | ~213 KB | ~847 tiles * (4 + 256) B (worst case) |
| group_offset prefix-sum | 352 MB | ~88M * 4 B (variable tower heights, ~15-20% increase) |
| UF parent array | 1.76 GB | ~440M groups * 4 B (path halving, no rank) |
| base_y + delta + tiles_per_tower | 36 MB | Grid geometry (3 per-tower arrays) |
| **Total host** | **~2.2 GB** | |

Burst mode eliminates the 9.38 GB host-side tile_ops copy. The compositor
ingests each burst's tiles and discards the raw data (retaining only the
previous tower's tiles_per_tower[j] tiles for L/R matching). Host memory
is dominated by the UF parent array (path halving, no rank array).

### S13.3 Host Memory (Dump Mode)

In dump mode, the full TileOp array must be accumulated for output:

| Structure | Size | Notes |
|-----------|------|-------|
| Full TileOp array | ~11.2 GB | sum(tiles_per_tower[j]) * 128 B |
| Compositor state | ~2.2 GB | As above |
| **Total host (dump)** | **~13.4 GB** | |

Alternatively, dump mode can stream burst outputs directly to disk,
avoiding the full in-memory accumulation:

| Structure | Size | Notes |
|-----------|------|-------|
| Burst buffer | 23 MB | Written to disk per burst (worst-case 46 tiles/tower) |
| Compositor state | ~2.2 GB | As above |
| **Total host (streaming dump)** | **~2.2 GB** | |

### S13.4 Disk (Dump Mode)

| Structure | Size | Notes |
|-----------|------|-------|
| Header + geometry | ~27 MB | base_y + delta + header |
| TileOp array | ~11.2 GB | Bulk of the dump (variable tower heights) |
| Extended tiles | ~213 KB | Side table |
| **Total dump** | **~11.2 GB** | |

---

## S14. Invariants

The following invariants are maintained by the campaign pipeline and
verified at each stage boundary.

### S14.1 Grid Invariants

1. **Arc tracking:** for all j, |base_y[j] - sqrt(R^2 - (j*S)^2)| <= 1
2. **Delta consistency:** delta[j] = base_y[j] - base_y[j+1] for all j in [0, J-2]
3. **Delta non-negative:** delta[j] >= 0 for all j
4. **Termination:** last tower satisfies `base_y[J-1] + tiles_per_tower[J-1] * S + MARGIN > (J-1) * S`; tower J would not
5. **Tower count:** J = len(base_y) = len(delta) + 1 = len(tiles_per_tower)
6. **Tower height range:** `32 <= tiles_per_tower[j] <= 46` for all j in [0, J)
7. **Tower height smoothness:** `|tiles_per_tower[j] - tiles_per_tower[j+1]| <= 1` for all j in [0, J-2)

### S14.2 Tile Source Invariants

8. **Tower 0 exclusion:** CUDA never processes tiles with j = 0
9. **Complete coverage:** every tile (j, r) with j in [0, J), r in [0, tiles_per_tower[j]) is processed by exactly one source (all tiles are live)
10. **Axis correction applied:** tower j = 0 tiles have inert-prime correction
11. **Tower 0 first:** tower j = 0 is ingested by the compositor before any CUDA burst towers

### S14.3 TileOp Invariants

12. **Valid encoding:** every non-overflow tile has off_I >= 3, off_L >= off_I, off_R >= off_L
13. **Empty TileOp valid:** tiles with zero primes have off_I = off_L = off_R = 3, byte 3 = 0 (rare; not tied to diagonal)
14. **Overflow sentinel:** overflow tiles have all 128 bytes = 0xFF
15. **Extended coverage:** every overflow tile in a burst has a corresponding extended TileOp before compositor ingestion

### S14.4 Burst Invariants

16. **Burst contiguity:** each burst covers a contiguous range of towers [j_lo, j_hi)
17. **Burst ordering:** bursts are processed in ascending tower order
18. **Burst completeness:** the union of all bursts covers towers [1, J) exactly (no gaps, no overlaps)
19. **Overflow resolved per burst:** all overflow tiles in burst b are reprocessed before burst b's towers are ingested by the compositor
20. **Double-buffer safety:** GPU writes to buffer A while CPU reads buffer B; they never access the same buffer simultaneously

### S14.5 Compositor Invariants

21. **No false moats:** if the compositor reports MOAT, no connected path exists through the annulus at step sqrt(K_SQ). All overflow tiles are resolved via extended TileOps — no conservative bridging
22. **Completeness:** every tile's ports are wired. No boundary port is left unmatched unless it has no geometric neighbor
23. **Group-level correctness:** UF operations use global_id(tile, group), not port-level IDs

### S14.6 Early Termination Invariants

24. **UF monotonicity:** union-find operations can only merge groups, never split. A SPANNING verdict at burst b remains valid for all subsequent bursts
25. **SPANNING early-exit safe:** if check_spanning_incremental() returns true after burst b, the SPANNING verdict is definitive; finalize() is called and no further processing is needed
26. **MOAT requires full octant:** a MOAT verdict is only emitted after all J towers have been ingested and finalize() is called

### S14.7 CUDA Constant Invariants

27. **K_SQ consistency:** the CUDA kernel's K_SQ matches the runtime parameter
28. **COLLAR consistency:** the CUDA kernel's COLLAR equals ceil(sqrt(K_SQ))
29. **SIDE_EXP consistency:** the CUDA kernel's SIDE_EXP equals TILE_POINTS + 2 * COLLAR
30. **Backward offset consistency:** the CUDA kernel's backward-neighbor table is derived from K_SQ

---

## S15. Module Dependencies

The campaign binary links against three libraries and the compositor.

### S15.1 Dependency Graph

```
tiles-campaign (binary)
  |
  +-- tile-cuda/        CUDA tile processor (GPU kernels)
  |     5-kernel pipeline: sieve, compact, UF, face-extract, prune+encode
  |     Burst-mode launch: processes tower ranges, not full octant
  |     Tower-major output: flat TileOp[128] per tile, no metadata
  |     K_SQ-parameterized: COLLAR, SIDE_EXP, backward offsets
  |
  +-- tile-cpp/          C++ reference tile processor
  |     Same 5-phase pipeline, single-threaded
  |     Used for: tower j=0 (axis correction), overflow fallback (256-byte)
  |     K_SQ-parameterized: matching constants
  |
  +-- compositor/        Compositor library
  |     Group-level UF, I/O matching, L/R matching, spanning check
  |     Incremental API: ingest_tower(), check_spanning_incremental(), collect_outer_boundary(), finalize()
  |     Consumes Grid + TileOp[] + optional extended side table
  |
  +-- grid/              Grid construction (shared between all modules)
        compute_base_y, compute_delta, tower termination predicate
        K_SQ-parameterized: COLLAR affects tower termination check
```

### S15.2 Build

```
tiles-campaign/
  CMakeLists.txt
  src/
    main.cpp             // CLI parsing, radial sweep loop, output dispatch
    grid.cpp             // Grid construction (S3)
    burst_driver.cpp     // Burst loop orchestration, GPU/CPU overlap (S5)
    cuda_launch.cpp      // Per-burst CUDA launch and synchronization
    axis_processor.cpp   // Tower j=0 C++ processing (S6)
    overflow.cpp         // Per-burst overflow scan and C++ fallback (S7)
    early_exit.cpp       // SPANNING detection and burst cancellation (S8)
    compositor_bridge.cpp // Compositor init, per-tower ingestion, finalize (S9)
    output.cpp           // verdict_only, dump, dump_with_stats formatting (S11)
  include/
    campaign.h           // Shared types: Grid, ExtendedTileSideTable, BurstRange
    constants.h          // K_SQ-derived constants, CUDA consistency check
```

The campaign binary is self-contained: it does not phone home, does not
require a network connection, and does not depend on any external services.
It reads parameters from the command line, processes, and exits.

### S15.3 External Dependencies

| Dependency | Version | Purpose |
|------------|---------|---------|
| CUDA toolkit | >= 12.0 | GPU kernel compilation and runtime |
| C++ compiler | C++17 | Host code compilation |
| fmt (optional) | >= 9.0 | Structured output formatting |
| nlohmann/json (optional) | >= 3.11 | dump_with_stats JSON output |
