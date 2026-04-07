---
date: 2026-03-22
engine: coordinator
status: complete
topic: CUDA ISE Runner — Batched Multi-Tile Kernel Implementation Plan
method: Full source audit of tile_main.cu, tile_kernel.cuh, row_sieve.cuh, orchestrator.rs, scanline.rs, algorithm research
---

# CUDA ISE Runner — Batched Multi-Tile Kernel

## 0. Problem Statement

The Rust ISE (`tile-probe/crates/ise`) processes campaigns of shells x stripes. Each
shell covers a radial band [a_lo, a_hi]; each stripe places a tile at a different
b-offset. The key metric per shell is f(r) = count(io_count > 0) / M, where M is
the number of stripes. A moat candidate is a shell where f(r) = 0: no stripe tile
has inner-to-outer connectivity.

Currently, the CUDA `tile_cuda` binary processes ONE tile per process invocation:
~330ms on Jetson Orin Nano, of which ~100-200ms is CUDA context initialization.
A campaign of 100 shells x 32 stripes = 3,200 tiles = 3,200 process spawns =
~17 minutes, dominated by context init overhead.

**Goal:** A single `ise_cuda` binary that initializes CUDA once, then processes an
entire campaign (all shells, all stripes) with batched kernel launches. One CUDA
context amortized across the full campaign.

---

## 1. Architecture Overview

```
                              ise_cuda binary
                    ┌──────────────────────────────────┐
                    │         Host Campaign Loop        │
                    │                                   │
                    │  for shell_i in 0..num_shells:    │
                    │    compute tile params for M tiles │
                    │    upload TileParams[M] to GPU    │
                    │    ┌─────────────────────────┐    │
                    │    │  Kernel 1: Primality     │    │
                    │    │  M thread blocks          │    │
                    │    │  each block: 1 row/iter   │    │
                    │    │  row-sieve + MR → bitmap  │    │
                    │    └─────────┬───────────────┘    │
                    │              │ (implicit sync)     │
                    │    ┌─────────▼───────────────┐    │
                    │    │  Host: UF + Classify     │    │
                    │    │  D2H transfer M bitmaps  │    │
                    │    │  CPU: UF + face classify  │    │
                    │    │  per tile (sequential)    │    │
                    │    └─────────┬───────────────┘    │
                    │              │                     │
                    │    aggregate f(r), emit trace     │
                    │    if f(r)==0: candidate shell     │
                    └──────────────┴───────────────────┘
```

**Key design decision: CPU union-find, GPU primality.**

The current `tile_main.cu` already does this: GPU computes the primality bitmap,
then CPU runs UF + face classification. The algorithm research document confirms
this is correct: primality is compute-bound (embarrassingly parallel), UF is
memory-bound (pointer chasing, data-dependent). Fusing them would force the worst
register/memory profile on both phases.

For the ISE runner, we keep this split. The primality kernel is batched (M blocks
per launch). The UF phase runs on CPU for each tile sequentially (or with host
threads if warranted).

**Why not GPU union-find?** At 2000x2000 tiles, the UF phase takes ~40ms CPU
(from tile_main.cu timings: cpu_cc_ms). The primality kernel takes ~200ms GPU.
Moving UF to GPU would save ~40ms but add complexity (hook-to-min kernel,
compaction, face classification on GPU). The ROI is poor for the ISE use case
where we process M=32 tiles per shell: total UF time per shell = 32 x 40ms =
1.28s CPU. With 6 ARM cores on Jetson we can parallelize to ~215ms. This is
comparable to the GPU primality time. GPU UF is a Phase 3 optimization, not
Phase 1.

---

## 2. Memory Budget

### 2.1 Per-Tile Memory Requirements

For a tile with `side = 2000`, `collar = ceil(sqrt(k_sq))`:

| k_sq | collar | side_exp | total_points | bitmap (bytes) | parent (bytes) | rank (bytes) | total/tile |
|------|--------|----------|-------------|----------------|----------------|-------------|------------|
| 2    | 2      | 2005     | 4,020,025   | 502,504        | 16,080,100     | 4,020,025   | **19.6 MB** |
| 26   | 6      | 2013     | 4,052,169   | 506,522        | 16,208,676     | 4,052,169   | **19.8 MB** |
| 36   | 6      | 2013     | 4,052,169   | 506,522        | 16,208,676     | 4,052,169   | **19.8 MB** |
| 40   | 7      | 2015     | 4,060,225   | 507,529        | 16,240,900     | 4,060,225   | **19.8 MB** |

Formula:
- `side_exp = side + 1 + 2 * collar`
- `total_points = side_exp * side_exp`
- bitmap: `ceil(total_points / 32) * 4` bytes
- parent: `total_points * 4` bytes (uint32_t per point)
- rank: `total_points * 1` byte (uint8_t per point)

### 2.2 GPU Memory: Primality Bitmaps Only

The GPU only needs the bitmaps — UF runs on CPU. With M=32 tiles:

| Component | Per tile | M=32 tiles | Notes |
|-----------|----------|-----------|-------|
| Bitmap (device) | 507 KB | **15.8 MB** | Persistent across kernel launches |
| Row sieve shared mem | ~252 words = ~1 KB | per-block | Reused each row |
| Sieve tables (constant) | ~5 KB | ~5 KB | Shared across all blocks |
| Tile params (constant/global) | ~64 bytes | ~2 KB | Uploaded once per shell |
| **Total GPU** | | **~16 MB** | |

**Jetson Orin Nano (8 GB unified):** 16 MB is trivial. Could run M=256 tiles
(128 MB) without pressure. Memory is not the GPU constraint.

**A100 (40 GB HBM2e):** Could run M=1024+ tiles simultaneously.

**4090 (24 GB GDDR6X):** Same — no GPU memory concern.

### 2.3 CPU Memory: UF Arrays

UF runs on CPU. For M=32 tiles processed sequentially (reuse buffers):

| Component | Size | Notes |
|-----------|------|-------|
| parent array | 16 MB | Reused per tile |
| rank array | 4 MB | Reused per tile |
| bitmap (host copy) | 507 KB | Per tile, from D2H |
| Backward offsets | ~900 bytes | Precomputed once |
| component_faces + root_map | ~1 MB | Worst case |
| **Total CPU per tile** | **~22 MB** | Reusable |

If parallelizing UF across P host threads: P x 22 MB. On Jetson with 6 threads:
132 MB. Fits comfortably in 8 GB.

**Alternative: sequential UF with buffer reuse.** Process tiles one at a time,
reuse the same 22 MB buffer. Total CPU memory: 22 MB + 16 MB (bitmap array for
all M tiles, needed to batch D2H) = 38 MB. Minimal.

### 2.4 Memory Strategy Summary

| Platform | GPU memory | CPU memory | Strategy |
|----------|-----------|-----------|----------|
| Jetson Orin Nano | 16 MB (M=32 bitmaps) | 38 MB (sequential UF) | One shell at a time, M=32 blocks/kernel |
| RTX 4090 | 16 MB | 132 MB (6-thread UF) | Same, more CPU parallelism |
| A100 | 16 MB | 132 MB | Same |

No memory pooling or wave scheduling needed. All M bitmaps fit simultaneously on
every target GPU.

---

## 3. Kernel Design

### 3.1 Batched Primality Kernel

The existing `tile_sieved_primality_bitmap_kernel` in `row_sieve.cuh` processes
one tile. Each thread block handles one row of the expanded tile. The grid size
equals `side_exp` (number of rows).

For the ISE runner, we batch M tiles into a single kernel launch:

```
Grid:  M * side_exp blocks (one block per row per tile)
Block: 256 threads (kTileRowSieveBlockSize)
```

Each block identifies its tile index and row index from `blockIdx.x`:

```cuda
uint32_t tile_idx = blockIdx.x / side_exp;   // which tile (0..M-1)
uint32_t row      = blockIdx.x % side_exp;   // which row in that tile
```

The block reads its tile parameters from a device array `TileParams[M]`:

```cuda
struct TileParams {
    int64_t  a_lo;       // nominal a_lo for this tile
    int64_t  b_lo;       // nominal b_lo for this tile (stripe offset)
    int64_t  collar;     // ceil(sqrt(k_sq))
    uint64_t side_exp;   // side + 1 + 2*collar
    uint32_t bitmap_offset_words;  // offset into the batched bitmap array
};
```

The bitmap for tile `t` starts at `d_bitmap + params[t].bitmap_offset_words`.

**Shared memory:** Same as current kernel — `row_sieve_word_count(side_exp)`
words for the per-row sieve bitmap. At side_exp=2013: 63 words = 252 bytes.
This is tiny; shared memory is not a constraint.

**Constant memory:** Sieve tables (GM_SIEVE_TABLE, GM_MOD3_PRIMES) and k_sq
are uploaded once before the campaign loop. They do not change between shells
(k_sq is fixed for the campaign).

### 3.2 Grid Sizing

For M=32, side_exp=2013:

- Blocks per kernel: 32 x 2013 = **64,416 blocks**
- Jetson (8 SMs): 64,416 / 8 = 8,052 waves. Each wave executes in ~microseconds
  (one row of sieve + MR for 2013 columns). Total expected: ~200ms for all 32
  tiles (primality phase only).
- A100 (108 SMs): 64,416 / 108 = 597 waves. Expected: ~15ms.
- 4090 (128 SMs): 64,416 / 128 = 503 waves. Expected: ~10ms.

This is well within CUDA launch limits (max grid dim x = 2^31 - 1).

### 3.3 Kernel Code Modification

The existing `tile_sieved_primality_bitmap_kernel` needs minimal changes:

1. Add `const TileParams* __restrict__ params` parameter
2. Add `uint32_t num_tiles` parameter
3. Replace hardcoded `a_lo, b_lo, collar, side_exp` with per-tile lookup:
   ```cuda
   uint32_t tile_idx = blockIdx.x / params[0].side_exp;
   uint32_t row      = blockIdx.x % params[0].side_exp;
   // All tiles in a shell share side_exp, so params[0].side_exp is safe
   const TileParams& tp = params[tile_idx];
   ```
4. Write to `bitmap + tp.bitmap_offset_words` instead of bare `bitmap`

**Critical: all tiles in a shell share the same side_exp** (same k_sq, same
tile_size, same collar). This means the division `blockIdx.x / side_exp` is
uniform across the grid. No divergence.

### 3.4 Thread Model Within a Block

Unchanged from the current kernel:

- 256 threads per block
- Each block processes one row of one tile
- Phase 1: Parity sieve (a^b parity mark)
- Phase 2: Sieve table marking (p=1 mod 4 primes up to 10,000)
- Phase 3: Mod-3 primes marking (p=3 mod 4 axis primes)
- Phase 4: Small-norm rescue
- Phase 5: Survivors → Miller-Rabin → atomic bitmap write

Each phase uses `__syncthreads()` barriers. The block-local shared memory
sieve bitmap is allocated via `extern __shared__`.

---

## 4. Host-Side Logic

### 4.1 Initialization (Once)

```
1. Parse CLI arguments
2. cudaSetDevice(0)
3. init_row_sieve_tables()          // upload sieve tables to constant memory
4. copy_tile_k_sq(k_sq)             // upload k_sq to constant memory
5. Precompute backward offsets for UF
6. Compute stripe offsets: b_lo[j] = collar + j * stride for j=0..M-1
7. Compute shell bounds: (a_lo_i, a_hi_i, r_center_i) for all shells
8. Allocate device bitmap array: M * bitmap_words_per_tile * sizeof(uint32_t)
9. Allocate host bitmap buffer: same size (for D2H)
10. Allocate device TileParams array: M * sizeof(TileParams)
11. Allocate host UF buffers: parent[total_points], rank[total_points]
```

### 4.2 Campaign Loop

```
for shell_i in 0..num_shells:
    if step > 1 and shell_i % step != 0:
        continue   // sparse scan support

    // --- Prepare tile parameters ---
    for j in 0..M:
        params[j] = TileParams {
            a_lo:   shell_a_lo,
            b_lo:   stripe_offsets[j],
            collar: collar,
            side_exp: side_exp,
            bitmap_offset_words: j * bitmap_words_per_tile
        }

    // --- GPU: primality ---
    cudaMemcpy(d_params, params, M * sizeof(TileParams), H2D)
    cudaMemset(d_bitmap, 0, M * bitmap_bytes_per_tile)
    batched_primality_kernel<<<M * side_exp, 256, sieve_shared_bytes>>>(
        d_params, M, d_bitmap
    )
    cudaMemcpy(h_bitmap, d_bitmap, M * bitmap_bytes_per_tile, D2H)

    // --- CPU: UF + classify ---
    TileResult results[M]
    parallel_for j in 0..M:    // or sequential
        results[j] = classify_components(geom_j, h_bitmap + j*bitmap_words)

    // --- Aggregate ---
    connected = count(results[j].io_count > 0 for j in 0..M)
    f_r = connected / M

    // --- Output ---
    emit trace line (stderr)
    accumulate shell record
```

### 4.3 Output Formatting

Match the Rust ISE output contract:

**stderr trace** (when `--trace`):
```
trace shell 0: a=[1000000, 1002000] R~1001000.0 f(r)=0.9375 io=[1,1,1,0,1,...] primes=68421 160ms
```

**JSON trace** (when `--json-trace PATH`):
```json
{
  "config": { "k_sq": 26, "r_min": 1000000, ... },
  "shells": [
    { "shell_idx": 0, "r_center": 1001000.0, "a_lo": 1000000, "a_hi": 1002000,
      "io_counts": [1,1,1,0,...], "f_r": 0.9375, "is_candidate": false,
      "num_primes": 68421, "shell_time_ms": 160 }
  ],
  "summary": { "total_shells": 50, "total_tiles": 1600,
               "candidates": [], "total_time_ms": 8000 }
}
```

**CSV summary** (when `--csv PATH`):
```
shell_idx,r_center,a_lo,a_hi,f_r,is_candidate,num_primes,shell_time_ms
0,1001000.0,1000000,1002000,0.937500,false,68421,160
```

### 4.4 Shell Scheduling

**Phase 1: One shell at a time.** Process each shell sequentially. Within a
shell, all M stripe tiles are batched into one kernel launch. After the kernel
completes, all M bitmaps are transferred D2H, then UF runs on CPU.

This is the simplest and most memory-efficient approach. It uses exactly
M x bitmap_size GPU memory and one set of UF buffers on CPU.

**Phase 2 (optional): Double-buffered shells.** While CPU runs UF for shell_i,
GPU computes primality for shell_{i+1}. This overlaps GPU compute with CPU
compute. Requires 2 x M x bitmap_size GPU memory (32 MB on Jetson — still fine).

**Phase 3 (optional): Multi-shell batching.** On A100/4090 with large GPU
memory, launch multiple shells worth of tiles in a single kernel. This increases
GPU utilization (more blocks) but requires more memory and complicates the
params layout. Only beneficial if per-shell kernel launch overhead is significant
(it is ~10us, negligible vs 200ms kernel runtime).

**Recommendation: Start with Phase 1. Move to Phase 2 only if profiling shows
CPU UF is slower than GPU primality, creating a stall.**

---

## 5. CLI Interface

```
ise_cuda — GPU-accelerated Independent Strip Ensemble for Gaussian moat detection

USAGE:
    ise_cuda --k-sq <K_SQ> --r-max <R_MAX> [OPTIONS]

REQUIRED:
    --k-sq <K_SQ>              Squared step bound k^2 (2, 26, 32, 36, 40)
    --r-max <R_MAX>            Maximum radius to scan

OPTIONS:
    --r-min <R_MIN>            Minimum radius [default: 0]
    --tile-size <S>            Square tile shorthand: W = H = S [default: 2000]
    --tile-width <W>           Tile width (overrides --tile-size)
    --tile-height <H>          Tile height (overrides --tile-size)
    --stripes <M>              Number of independent stripes [default: 32]
    --stride <STRIDE>          Center-to-center b-spacing between stripes
                               [default: tile_width + 2*collar]
    --step <N>                 Process every N-th shell only [default: 1]

OUTPUT:
    --trace                    Print per-shell trace to stderr
    --json-trace <PATH>        Write JSON trace to file
    --csv <PATH>               Write CSV summary to file

VALIDATION:
    --validate                 Run built-in validation suite (k^2=2 moat)

DEVICE:
    --device <ID>              CUDA device index [default: 0]

EXAMPLES:
    # Quick smoke test: k^2=2 moat detection
    ise_cuda --k-sq 2 --r-max 50 --tile-size 8 --stripes 8 --trace

    # k^2=26 cross-validation with Rust ISE
    ise_cuda --k-sq 26 --r-min 1000000 --r-max 1010000 --stripes 32 \
             --trace --csv k26_cuda.csv

    # k^2=36 production campaign, sparse scan every 50K units
    ise_cuda --k-sq 36 --r-max 80100000 --step 25 --stripes 32 \
             --trace --json-trace k36_campaign.json --csv k36.csv

    # k^2=40 sparse exploration
    ise_cuda --k-sq 40 --r-max 200000000 --step 100 --stripes 32 \
             --trace --csv k40_sparse.csv
```

### Parameter Defaults

| Parameter | Default | Rationale |
|-----------|---------|-----------|
| tile-size | 2000 | Matches Rust ISE "deep" preset |
| stripes | 32 | Statistical power: p^32 false positive bound |
| stride | W + 2*collar | Minimum for disjoint expanded neighborhoods (LaTeX Sec 4.2) |
| step | 1 | Process every shell (dense scan) |
| r-min | 0 | Start from origin |

---

## 6. Implementation Phases

### Phase 1: Batched Primality (MVP)

**Goal:** Single kernel launch processes M tiles' primality bitmaps.

**Tasks:**
1. Create `src/ise_main.cu` — the new binary entry point
2. Create `src/ise_kernel.cuh` — batched primality kernel (adapts row_sieve.cuh)
3. Define `TileParams` struct and device array
4. Implement `shell_bounds()` and `stripe_offsets()` (port from orchestrator.rs)
5. CLI argument parsing (k-sq, r-min, r-max, tile-size, stripes, stride, step)
6. Campaign loop: prepare params, launch kernel, D2H transfer
7. CPU UF + face classification (reuse from tile_main.cu with buffer recycling)
8. f(r) computation and stderr trace output
9. Add `ise_cuda` target to CMakeLists.txt

**Gate:** `ise_cuda --k-sq 2 --r-max 50 --tile-size 8 --stripes 8 --trace`
detects moat candidates at R > 15 (same shells as Rust ISE `--validate`).

**Estimated effort:** 400-600 lines of new code. Most of it is host-side
orchestration — the kernel is a thin wrapper around the existing row-sieve kernel.

### Phase 2: Output Parity

**Goal:** JSON trace and CSV output matching Rust ISE format exactly.

**Tasks:**
1. JSON trace writer (nlohmann/json or manual fprintf — no heavy deps)
2. CSV summary writer
3. `--validate` mode with built-in k^2=2 test
4. Human-readable summary output

**Gate:** `ise_cuda --k-sq 26 --r-min 1000000 --r-max 1010000 --stripes 32 --csv cuda.csv`
produces identical f(r) values as `ise --k-squared 26 --r-min 1000000 --r-max 1010000
--stripes 32 --csv rust.csv` (diff on CSV, f_r column matches to 6 decimal places).

### Phase 3: Performance Optimization

**Goal:** Double-buffered shell processing, CPU-parallel UF.

**Tasks:**
1. CUDA stream for async D2H overlap with next shell's kernel
2. CPU thread pool (std::thread or OpenMP) for parallel UF across M tiles
3. Benchmark: per-shell wall time on each platform
4. Profile: GPU utilization, D2H stalls, CPU UF balance

**Gate:** Per-shell time on Jetson < 500ms (vs estimated 330ms GPU + 1.28s CPU
sequential → target with overlap: < 500ms).

### Phase 4: Sparse Scan + Production Campaigns

**Goal:** Run the calibration campaign across all k^2 values.

**Tasks:**
1. `--step N` parameter for sparse radial scanning
2. Campaign scripts for each k^2 value
3. Result aggregation and visualization

**Gate:** Complete calibration campaign (Section 9) produces publishable f(r) curves.

---

## 7. Test Plan

### 7.1 Unit Tests

| Test | What it verifies | Command |
|------|-----------------|---------|
| Backward offsets k_sq=36 | 56 offsets | Compile-time assertion in ise_kernel.cuh |
| Single-tile match | CUDA tile matches `tile_cuda` for same (a_lo, b_lo, side, k_sq) | Run both, compare JSON output |
| Stripe offset computation | b_lo[j] = collar + j * stride | Assert in host code |
| Shell bounds | Correct [a_lo, a_hi] intervals covering [r_min, r_max] | Assert shell coverage |

### 7.2 Cross-Validation with Rust ISE

**Protocol:** Run identical campaigns on both CUDA and Rust ISE, compare per-shell f(r).

| k_sq | r_min | r_max | tile | stripes | stride | Expected |
|------|-------|-------|------|---------|--------|----------|
| 2 | 0 | 50 | 8 | 8 | default | Candidates at R > 15 |
| 26 | 1000000 | 1010000 | 2000 | 32 | default | f(r) > 0 everywhere |
| 26 | 0 | 1000 | 200 | 8 | default | f(r) > 0 everywhere |
| 36 | 1000000 | 1002000 | 2000 | 32 | default | Cross-check f(r) values |

**Matching criteria:** For each shell, the io_counts vector must be identical
(not just f(r) — the per-stripe binary outcomes must match bit-for-bit).

**Implementation:** A shell script that runs both binaries with `--csv`, then
diffs the f_r columns. Any mismatch is a hard failure.

### 7.3 k^2=2 Moat Detection

The definitive correctness gate. k^2=2 has a known moat at R ~ 11.7 (farthest
point (11, 4), norm 137). The ISE should show f(r) = 0 for shells beyond the
moat boundary.

```
ise_cuda --k-sq 2 --r-max 50 --tile-size 8 --stripes 8 --trace
```

Expected: shells near origin have f(r) > 0, shells at R > 25 have f(r) = 0.

### 7.4 k^2=26 Ground Truth

k^2=26 has no known moat up to Gethner/Wagon/Wick's 1998 boundary. At R < 1000,
all shells should show f(r) > 0.

```
ise_cuda --k-sq 26 --r-min 0 --r-max 1000 --tile-size 200 --stripes 8 --trace
```

Expected: zero candidates.

### 7.5 Performance Regression Tests

After Phase 3, establish baselines:

| Platform | k_sq | r_range | Per-shell time | Tile count |
|----------|------|---------|---------------|-----------|
| Jetson | 26 | 1M-1.01M | < 500ms | 160 |
| Jetson | 36 | 1M-1.01M | < 500ms | 160 |
| 4090 | 26 | 1M-1.1M | < 100ms | 1600 |

---

## 8. Performance Projections

### 8.1 Current Single-Tile Timing (tile_cuda, Jetson)

From existing measurements on `tile_cuda` at side=2000, k_sq=36:

| Phase | Time | Notes |
|-------|------|-------|
| CUDA context init | ~150ms | One-time per process |
| GPU primality (row-sieve + MR) | ~200ms | ~2013 blocks x 256 threads |
| D2H bitmap transfer | ~2ms | 507 KB |
| CPU UF + classify | ~40ms | Sequential, single core |
| JSON output | ~1ms | |
| **Total** | **~330ms** | Context init dominates overhead |

### 8.2 Projected Batched ISE Timing (M=32 tiles per shell)

#### Jetson Orin Nano (8 SMs, 6 ARM cores)

| Phase | Time | Calculation |
|-------|------|-------------|
| CUDA context init | 150ms | **Once per campaign** |
| GPU primality (32 tiles) | ~800ms | 32 x 200ms / (8 SMs / ~1 block active per SM at a time) ... see below |
| D2H bitmap transfer | ~8ms | 32 x 507 KB = 16 MB, ~2 GB/s PCIe |
| CPU UF (32 tiles, 6 threads) | ~215ms | 32 x 40ms / 6 cores |
| Shell aggregation | ~0.1ms | Trivial |
| **Per-shell total** | **~1.0s** | |

**GPU primality breakdown:** The kernel launches 64,416 blocks (32 x 2013). Each
block processes one row (2013 columns, 256 threads). With 8 SMs and ~4 blocks per
SM concurrently (based on register/shared memory limits), we get 32 blocks in
flight. 64,416 blocks / 32 concurrent = 2013 waves. Each wave does row-sieve +
MR for 256 threads over 2013 columns ≈ 8 iterations per thread. At ~1us per MR
call and ~7% survivor rate, each row takes ~50us. Total: 2013 x 50us ≈ 100ms per
tile-worth of rows. But with 32 tiles interleaved across SMs, the total is roughly
32 x 100ms / 4 = 800ms (4 blocks per SM).

Actually, more precisely: the GPU processes all 64,416 blocks. Time ≈ 64,416 blocks
/ (8 SMs x 4 blocks/SM) x row_time ≈ 64,416 / 32 x 50us ≈ 100ms. Wait — this is
the same as a single tile because the blocks from different tiles can execute on
different SMs simultaneously. The key insight: **32 tiles x 2013 rows = same total
rows as 1 tile x 64,416 rows. GPU parallelism flattens the tile dimension.**

**Revised GPU estimate:** ~200ms for M=1, ~800ms for M=32 if memory bandwidth
limited, but if compute limited (which row-sieve + MR is), the GPU should scale
linearly with total blocks. With 8 SMs: 64,416 blocks / 32 active = 2013 waves.
Each wave ~50us. Total: ~100ms... This can't be right for M=32 tiles of 2013 rows.

Let me be more careful. Current single tile (2013 blocks) takes ~200ms on Jetson.
That's 2013 blocks processed at 200ms → ~100us per wave (2013 / (8 SMs x ~1 block)
= 252 waves at ~800us each... no, that doesn't add up either).

**Calibration from existing data:** `tile_cuda` at side=2000 on Jetson: gpu_primality_ms
≈ 170ms for 2013 blocks. So each block takes ~170ms x 8 SMs / 2013 ≈ 0.67ms average
(accounting for ~1 block per SM concurrent given register pressure). For 32 tiles:
64,416 blocks, 8 SMs → 64,416 / 8 = 8,052 serial waves → 8,052 x 0.67ms / 8...

Simpler approach: **GPU time scales linearly with block count** on a fixed GPU.
Single tile: 2013 blocks → 170ms. 32 tiles: 64,416 blocks → 170ms x 32 = **5.4s**.

No — that assumes 1 SM. With 8 SMs processing blocks in parallel:
Single tile: 2013 blocks / 8 SMs ≈ 252 waves → 170ms → 0.67ms per wave.
32 tiles: 64,416 blocks / 8 SMs ≈ 8,052 waves → 8,052 x 0.67ms = **5.4s**.

**This matches: 32 tiles x 170ms = 5.4s. The GPU can only run ~8 blocks
simultaneously. 32 tiles don't parallelize on an 8-SM GPU — they queue.**

This is worse than expected. Let me reconsider the strategy.

#### Revised: Wave-Based Processing on Jetson

With 8 SMs and register pressure limiting to ~2-4 blocks per SM, the Jetson can
run 16-32 blocks simultaneously. 32 tiles x 2013 rows = 64,416 blocks. At 16
concurrent blocks: 64,416 / 16 = 4,026 waves. At 32 concurrent: 2,013 waves.

Time per wave: ~170ms / (2013/16) ≈ 1.35ms... Let me just use the linear scaling:

**GPU primality for M tiles: M x single_tile_gpu_time**

| Platform | Single tile GPU | M=32 GPU | CPU UF (32 tiles, parallel) | Per-shell |
|----------|----------------|----------|---------------------------|-----------|
| Jetson | 170ms | **5.4s** | 215ms (6 threads) | **~5.6s** |
| 4090 | ~11ms* | **350ms** | ~50ms (16+ threads) | **~400ms** |
| A100 | ~13ms* | **420ms** | ~60ms (16+ threads) | **~480ms** |

*Estimated from throughput ratios: 4090 sieve is 3.0x Jetson at sqrt(36) scale,
tile kernel should be similar. So 170ms / 3.0 ≈ 57ms... but the tile kernel is
different from the norm-sieve kernel. Use the actual tile_cuda measurement if
available, otherwise estimate conservatively.

**For Jetson at M=32: ~5.6s per shell is the honest estimate.** This is still
66x better than the current approach (32 x 330ms = 10.5s with context init, but
also loses process-level parallelism since tiles ran as independent processes).

**The real win: amortized context init.** For a 100-shell campaign:
- Current: 3,200 process spawns x 330ms = 1,056s ≈ 17.6 minutes
- Batched: 100 x 5.6s + 0.15s init = 560s ≈ 9.3 minutes (1.9x speedup)

**For M=8 (reduced stripes):**
- Jetson: 8 x 170ms = 1.36s GPU + 54ms CPU = 1.4s per shell
- 100 shells: 140s + 0.15s = 2.3 minutes

**For small tiles (side=500, "balanced" preset):**
- Single tile GPU ≈ 170ms x (500/2000)^2 ≈ 10.6ms
- M=32: 340ms GPU + 13ms CPU = 353ms per shell
- 100 shells: 35s

### 8.3 Summary of Performance Projections

| Platform | Tile | M | Per-shell | 100 shells | 1000 shells |
|----------|------|---|-----------|-----------|------------|
| **Jetson** | 2000 | 32 | 5.6s | 9.3 min | 93 min |
| **Jetson** | 2000 | 8 | 1.4s | 2.3 min | 23 min |
| **Jetson** | 500 | 32 | 0.35s | 35s | 5.8 min |
| **4090** | 2000 | 32 | 0.4s | 40s | 6.7 min |
| **4090** | 2000 | 128 | 1.6s | 2.7 min | 27 min |
| **A100** | 2000 | 32 | 0.5s | 50s | 8.3 min |

**Key insight for campaigns:** At k^2=36 and k^2=40, we use sparse scanning
(--step 25 or --step 100). A sweep from R=0 to R=80M with step=25 at tile_height
=2000 gives 80,000,000 / 2000 / 25 = 1,600 shells. On 4090 at M=32: 1,600 x
0.4s = 10.7 minutes. Very feasible.

---

## 9. The Calibration Campaign

Exact parameters for the clean sweep across all k^2 values.

### 9.1 k^2 = 2 (Validation)

Known moat at R ~ 11.7. Small tiles sufficient.

```
ise_cuda --k-sq 2 --r-max 100 --tile-size 8 --stripes 16 --trace \
         --json-trace calibration/k2.json --csv calibration/k2.csv
```

- Shells: ceil(100 / 8) = 13
- Expected: candidates at R > ~15
- Purpose: correctness gate (must see f(r) = 0)
- Time: < 1s on any platform

### 9.2 k^2 = 26 (Dense Baseline)

No known moat. All f(r) > 0 expected up to R ~ 10^7.

```
# Phase A: Dense near-origin validation
ise_cuda --k-sq 26 --r-max 100000 --tile-size 2000 --stripes 32 --trace \
         --json-trace calibration/k26_dense.json --csv calibration/k26_dense.csv

# Phase B: Sparse long-range exploration
ise_cuda --k-sq 26 --r-max 10000000 --tile-size 2000 --stripes 32 --step 5 --trace \
         --csv calibration/k26_sparse.csv
```

- Phase A shells: 50, Phase B shells: 1,000
- collar = 6, stride = 2000 + 12 = 2012
- max_b = 6 + 31 x 2012 = 62,378. Angular coverage at R=10M: 0.36 degrees
- Expected: f(r) > 0 everywhere
- Time: Phase A ~25s Jetson, Phase B ~23 min Jetson / ~7 min 4090

### 9.3 k^2 = 32 (Reference Point)

Tsuchimura established a lower bound. Moat boundary unknown precisely.

```
# Dense near-origin
ise_cuda --k-sq 32 --r-max 1000000 --tile-size 2000 --stripes 32 --trace \
         --csv calibration/k32_dense.csv

# Sparse out to 50M
ise_cuda --k-sq 32 --r-max 50000000 --tile-size 2000 --stripes 32 --step 25 --trace \
         --csv calibration/k32_sparse.csv
```

- collar = 6, stride = 2012
- Dense shells: 500, Sparse shells: 1,000
- Expected: f(r) depression at large R, possible candidates
- Time: Dense ~47 min Jetson / 3 min 4090. Sparse ~23 min Jetson / 7 min 4090

### 9.4 k^2 = 36 (Tsuchimura's Record)

Known moat at R ~ 80,015,782. The crown jewel.

```
# Phase A: Near-origin dense (confirm connectivity)
ise_cuda --k-sq 36 --r-max 1000000 --tile-size 2000 --stripes 32 --trace \
         --csv calibration/k36_near.csv

# Phase B: Mid-range sparse
ise_cuda --k-sq 36 --r-min 1000000 --r-max 40000000 --tile-size 2000 --stripes 32 \
         --step 10 --trace --csv calibration/k36_mid.csv

# Phase C: Approach moat boundary
ise_cuda --k-sq 36 --r-min 40000000 --r-max 80100000 --tile-size 2000 --stripes 32 \
         --step 5 --trace --csv calibration/k36_approach.csv

# Phase D: Dense scan around known boundary
ise_cuda --k-sq 36 --r-min 79000000 --r-max 80100000 --tile-size 2000 --stripes 32 \
         --trace --json-trace calibration/k36_boundary.json --csv calibration/k36_boundary.csv
```

- collar = 6, stride = 2012
- Phase A: 500 shells. Phase B: 3,900 shells. Phase C: 8,000 shells. Phase D: 550 shells
- Expected: f(r) ≈ 1.0 near origin, depression starting around R ~ 50M, candidates
  near R ~ 80M
- Time on 4090: A=3min, B=26min, C=53min, D=4min. Total ~86min
- Time on Jetson: A=47min, B=6.1hr, C=12.4hr, D=51min. Total ~20hr

### 9.5 k^2 = 40 (The Frontier)

No known moat. This is the scientifically interesting target.

```
# Phase A: Near-origin
ise_cuda --k-sq 40 --r-max 1000000 --tile-size 2000 --stripes 32 --trace \
         --csv calibration/k40_near.csv

# Phase B: Sparse exploration
ise_cuda --k-sq 40 --r-max 200000000 --tile-size 2000 --stripes 32 \
         --step 100 --trace --csv calibration/k40_sparse.csv

# Phase C: Dense around interesting regions (informed by Phase B)
# Parameters TBD based on Phase B results
```

- collar = 7, stride = 2000 + 14 = 2014
- max_b at M=32: 7 + 31 x 2014 = 62,441
- Phase A: 500 shells. Phase B: 1,000 shells
- Expected: f(r) ≈ 1.0 throughout (no moat expected at accessible R)
- Time on 4090: A=3min, B=7min. Very fast at sparse scan.
- Time on Jetson: A=47min, B=23min.

### 9.6 Campaign Summary

| k^2 | Total shells | 4090 time | Jetson time | Primary question |
|-----|-------------|-----------|-------------|-----------------|
| 2 | 13 | < 1s | < 1s | Correctness gate |
| 26 | 1,050 | ~8 min | ~25 min | Baseline (no moat) |
| 32 | 1,500 | ~10 min | ~70 min | Connectivity vs R profile |
| 36 | 12,950 | ~86 min | ~20 hr | Reproduce Tsuchimura |
| 40 | 1,500 | ~10 min | ~70 min | Frontier exploration |

**Total on 4090: ~2 hours for the complete calibration sweep.**
**Total on Jetson: ~24 hours (can run overnight).**

---

## 10. Implementation Notes

### 10.1 Stride Computation

Port directly from `orchestrator.rs::stripe_offsets()`:

```cpp
int64_t collar = static_cast<int64_t>(ceil_sqrt_u64(k_sq));
int64_t stride = (user_stride > 0) ? user_stride : (tile_width + 2 * collar);
for (int j = 0; j < M; ++j) {
    stripe_b_lo[j] = collar + j * stride;
}
```

The default stride ensures expanded tile neighborhoods (with collar) are disjoint,
which is required for statistical independence of stripe outcomes per LaTeX Sec 4.2.

### 10.2 Face Classification Boundary Conditions

The current `tile_main.cu` uses `<=` for face boundary checks (lines 313-324):

```cpp
if (a - geom.a_lo <= geom.collar) component_faces[c] |= FACE_INNER_BIT;
if (geom.a_hi - a <= geom.collar) component_faces[c] |= FACE_OUTER_BIT;
```

This matches the Rust ISE `scanline.rs` (lines 175-189). The `<=` is critical:
a face boundary zone is `collar+1` deep, not `collar` deep. Changing to `<`
would break face classification and produce different f(r) values.

### 10.3 Reusing tile_main.cu Code

The UF + face classification logic in `tile_main.cu` (lines 188-369) can be
extracted into a shared header `tile_classify.cuh` and reused by both `tile_main.cu`
and `ise_main.cu`. The key functions:

- `precompute_backward_offsets(k_sq)` — already standalone
- `uf_find()`, `uf_union()` — already standalone
- `classify_components(geom, bitmap)` — needs to be refactored from
  `std::vector<uint32_t>` to accept a raw pointer + size

### 10.4 Error Handling

- **GPU OOM:** Check `cudaMalloc` return code. If OOM, halve M and retry with two
  kernel launches per shell. Print warning to stderr.
- **Nonsensical results:** After each shell, check num_primes > 0 (a tile at any
  reasonable R should have primes). If num_primes == 0, print warning. If io_count
  > total_components, something is wrong — abort.
- **CUDA errors:** Use the existing `CUDA_CHECK` macro for all CUDA calls.

### 10.5 File Layout

```
src/
    ise_main.cu           New binary entry point
    ise_kernel.cuh        Batched primality kernel (wraps row_sieve.cuh)
    tile_classify.cuh     Refactored UF + face classification (shared)
    tile_main.cu          Existing single-tile binary (uses tile_classify.cuh)
    row_sieve.cuh         Unchanged
    tile_kernel.cuh       Unchanged
    miller_rabin.cuh      Unchanged
    cornacchia.cuh        Unchanged
    modular_arith.cuh     Unchanged
```

CMakeLists.txt addition:
```cmake
add_executable(ise_cuda
    src/ise_main.cu
)
target_include_directories(ise_cuda PRIVATE src/)
```

---

## 11. Open Questions

### 11.1 Tile Size at Large R

At R = 80,000,000 with tile_size = 2000, the angular coverage per stripe is
tiny: arctan(2000 / 80M) ≈ 0.0014 degrees. Each stripe is an extremely thin
radial slit. Is this enough to detect connectivity?

Answer: Yes. The ISE method does not require angular coverage. Each stripe is an
independent sample. We are asking "does inner-to-outer connectivity exist at this
angular position?" The answer is independent of angular coverage — it depends only
on the prime density within the tile. At R = 80M, norm ≈ 6.4 x 10^15, prime
density ≈ 1 / (2 ln(6.4e15)) ≈ 1.37%. A 2000x2000 tile has ~4M points, ~55K
primes — plenty for connectivity analysis.

### 11.2 Fallback Heights

The Rust ISE supports `--fallback-heights` for re-probing candidates at different
tile heights. This is a Phase 4 feature for the CUDA ISE. The first implementation
should focus on single-height campaigns.

### 11.3 GPU Union-Find (Future)

If CPU UF becomes the bottleneck (it will, on 4090/A100 where GPU primality is
10-15x faster than Jetson), a GPU UF kernel would be the next major optimization.
The algorithm research document recommends hook-to-min with separate compression
passes over compacted primes. This requires:

1. Stream compaction: bitmap → dense prime list (~55K entries for a 2000^2 tile)
2. Hook-to-min CC kernel over compacted graph
3. Face classification on GPU

This is estimated at 500-800 lines of CUDA code and would eliminate the D2H
transfer bottleneck entirely.

---

## 12. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|-----------|
| GPU primality gives different results than CPU | Wrong f(r), invalid science | Cross-validate against Rust ISE on k^2=26 (Section 7.2) |
| Face boundary `<=` vs `<` mismatch | Different io_counts | Exact port from tile_main.cu, bit-for-bit io_counts comparison |
| Memory pressure on Jetson at M=32 | OOM crash | Budget analysis shows 38 MB total — not a real risk |
| Per-shell time on Jetson worse than projected | Campaign too slow | Reduce to M=8 stripes or tile_size=500 for Jetson runs |
| Row-sieve constant memory conflict with main sieve | Corrupt sieve tables | ISE runner uses its own binary, no conflict with gm_cuda_primes |
| Kernel launch overhead at 64K blocks | GPU starvation | Not a risk: 64K blocks is routine for CUDA. Launch overhead < 10us |
