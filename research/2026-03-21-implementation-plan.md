# Implementation Plan — Independent Strip Ensemble (ISE)

**Date:** 2026-03-21
**Status:** plan
**Engine:** coordinator (R. Jenkins)

---

## 1. Architecture Overview

### 1.1 Crate Structure

Restructure into a workspace with a shared library crate and two binary crates:

```
gaussian-moat-cuda/
  tile-probe/
    Cargo.toml          # workspace root
    crates/
      moat-kernel/      # shared library crate
        Cargo.toml
        src/
          lib.rs
          primes.rs     # PrimeSieve, gaussian_primes_in_rect, gaussian_primes_in_rect_with_sieve
          tile.rs       # TileOperator, build_tile_with_sieve, SimpleUF, face types
          kernel.rs     # TileKernel trait + CPU implementation
          compose.rs    # horizontal/vertical composition (LB mode only)
          profile.rs    # ProbeProfile, PhaseTimer, get_rss_kb
      ise/              # Independent Strip Ensemble binary
        Cargo.toml
        src/
          main.rs       # CLI entry point
          orchestrator.rs  # shell loop, stripe dispatch, f(r) aggregation
          output.rs     # JSON trace writer, CSV summary writer
      lb-probe/         # Lower Bound probe binary (current tile-probe, renamed)
        Cargo.toml
        src/
          main.rs       # current main.rs (migrated)
          probe.rs      # current probe.rs (migrated)
```

### 1.2 Why a Workspace

- **moat-kernel** is the shared library. Both `ise` and `lb-probe` depend on it. The CUDA kernel will eventually replace `moat-kernel::kernel::CpuKernel` behind the `TileKernel` trait.
- **ise** is the new binary implementing the ISE algorithm.
- **lb-probe** is the current tile-probe binary, renamed. It keeps composition and accumulated transport.
- The solver crate at `gaussian-moat-cuda/solver/` stays untouched — `moat-kernel` depends on it (if needed) or absorbs the relevant prime-sieving logic.

### 1.3 Workspace Cargo.toml

```toml
[workspace]
members = ["crates/moat-kernel", "crates/ise", "crates/lb-probe"]
resolver = "2"

[workspace.dependencies]
rayon = "1.10"
clap = { version = "4", features = ["derive"] }
fxhash = "0.2"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
smallvec = "1.13"
libc = "0.2"

[profile.release]
opt-level = 3
lto = true
codegen-units = 1
```

---

## 2. Data Flow

```
CLI args
  │
  ├─ k_sq, r_min, r_max, W, H, M, threads, output paths
  │
  ▼
Orchestrator::run()
  │
  ├─ Compute shell bounds: [(a_lo, a_hi, r_center), ...]
  │     shells = r_min, r_min+H, r_min+2H, ..., r_max-H
  │     N = ceil((r_max - r_min) / H)
  │
  ├─ Compute stripe offsets: [b_lo_0, b_lo_1, ..., b_lo_{M-1}]
  │     Default contiguous: b_lo_j = -(M*W)/2 + j*W
  │     Optional scattered: user-supplied or evenly-spaced across wider range
  │
  ▼
  For each shell (sequential — shells are independent but we want ordered output):
  │
  ├─ Build PrimeSieve for this shell's radial band
  │     sieve_limit = max(a_hi + collar, max_b + collar)
  │     One sieve shared across all M tiles in this shell
  │
  ├─ Parallel tile dispatch (Rayon par_iter over M stripes):
  │   │
  │   ├─ For stripe j: tile bounds = (a_lo, a_lo+H, b_lo_j, b_lo_j+W)
  │   ├─ kernel.run(tile_bounds, k_sq, &sieve) → TileResult { io_count: usize }
  │   │     Internally:
  │   │       1. Enumerate primes in expanded rect (tile + collar)
  │   │       2. Spatial hash into cells of size ceil(sqrt(k_sq))
  │   │       3. Union-find over all primes within k distance
  │   │       4. Classify components by face membership (I, O only — L/R not needed)
  │   │       5. Count components touching both I-face and O-face
  │   │       6. Return count
  │   │
  │   └─ Collect: io_counts: Vec<usize> of length M
  │
  ├─ Aggregate:
  │     f_r = io_counts.iter().filter(|&&c| c > 0).count() as f64 / M as f64
  │     is_candidate = f_r == 0.0
  │
  ├─ Record ShellRecord { shell_idx, r_center, io_counts, f_r, is_candidate, timing }
  │
  └─ If trace enabled: print shell summary to stderr

Output:
  ├─ JSON trace: { config, shells: [ShellRecord, ...] }
  └─ CSV summary: shell_idx, r_center, f_r, is_candidate, tile_time_ms
```

---

## 3. Module-by-Module Specification

### 3.1 `moat-kernel::primes` (REWRITTEN — row-wise b-dimension sieve)

**Problem**: At R=10⁶, norms are ~10¹², far beyond the 10M sieve cap. Every primality check falls through to Miller-Rabin (12 witnesses, ~80ns each). A 2000×2000 tile = 4M MR calls ≈ 0.3s. Prime enumeration is ~100× more expensive than the graph work that follows. This is the bottleneck — must be fixed from day 0.

**Why NOT segmented sieve over norm range**: At R=10⁶, a 2000×2000 tile has norm span ~4×10⁹ but only 4M actual points (fill factor 0.1%). Sieving that 4B range costs 2-3s per tile — worse than MR. The norm space is too sparse.

**Solution**: Row-wise b-dimension sieve. For fixed row `a`, we ask: for which `b ∈ [b_lo, b_lo+W)` is `a²+b²` prime? This is sievable directly in the b dimension:

1. **Parity filter**: If a ≡ b (mod 2), then a²+b² ≡ 0 (mod 2) — composite. Eliminates 50% in O(1).
2. **For each small prime p ≡ 1 (mod 4)**: `p | (a²+b²)` iff `b ≡ ±a·√(-1) (mod p)`. Two residue classes of b, sievable with stride p over the W-element array.
3. **For each small prime p ≡ 3 (mod 4)**: `p | (a²+b²)` only if `p | a` AND `p | b`. So only sieve when p divides the current row's a-value — most rows skip entirely.
4. **Survivors → 5-witness MR**: Deterministic for norms < 2.15×10¹² (covers R=10⁶). Only ~16% of points survive the sieve.

**Expected speedup**: 0.3s → ~53ms single-thread (5.7×), ~6ms with 8 Rayon threads (~50×).

**Public API**:

```rust
/// One-time precomputed table: (p, √(-1) mod p) for all primes p ≡ 1 (mod 4), p ≤ SIEVE_BOUND.
/// Built once, shared across all tiles. Read-only, trivially parallelizable.
pub struct RowSieveTable {
    /// (p, sqrt_neg1_mod_p) for p ≡ 1 (mod 4)
    primes_1mod4: Vec<(u32, u32)>,
    /// primes p ≡ 3 (mod 4) (only needed when p | a)
    primes_3mod4: Vec<u32>,
    /// MR witness set — 5 witnesses for norms < 2.15×10¹²
    mr_witnesses: [u64; 5],  // {2, 3, 5, 7, 11}
}
impl RowSieveTable {
    pub fn new(sieve_bound: u32) -> Self;  // Eratosthenes + Tonelli-Shanks
}

/// Enumerate Gaussian primes in a rectangle using row-wise sieve.
/// Parallelizes over rows with Rayon internally.
pub fn gaussian_primes_in_rect_rowsieve(
    a_min: i64, a_max: i64, b_min: i64, b_max: i64,
    table: &RowSieveTable,
) -> Vec<(i64, i64)>;

/// Single-row sieve (called by par_iter over rows).
fn primes_in_row(a: i64, b_lo: i64, w: usize, table: &RowSieveTable) -> Vec<(i64, i64)>;

/// Legacy API (backward compatible, used by lb-probe).
pub fn gaussian_primes_in_rect_with_sieve(
    a_min: i64, a_max: i64, b_min: i64, b_max: i64, sieve: &PrimeSieve
) -> Vec<(i64, i64)>;
```

**Key design notes**:
- `RowSieveTable` replaces `PrimeSieve` as the shared object. Sieve bound P=1000 → 86 primes p≡1(4) + 87 primes p≡3(4). Tiny.
- Per-row sieve array: `[bool; W]` where W=2000 → 2KB. Fits in L1 cache. Perfect locality.
- Each row is independent → Rayon par_iter over rows within a tile.
- Tonelli-Shanks for √(-1) mod p: O(log²p) per prime, done once at setup.
- Legacy `PrimeSieve` + `gaussian_primes_in_rect_with_sieve` kept for `lb-probe` backward compatibility.
- MR witnesses: {2,3,5,7,11} covers all n < 2,152,302,898,747. Add `debug_assert!(norm < 2_152_302_898_747)` safety check. For R > ~1.4M, bump to 7 witnesses {2,3,5,7,11,13,17} (covers n < 3.4×10¹⁴).

### 3.2 `moat-kernel::tile` (unchanged from current)

Keep the existing `tile.rs` verbatim. Public API used by ISE:

```rust
pub struct TileOperator {
    pub a_min: i64,
    pub a_max: i64,
    pub b_min: i64,
    pub b_max: i64,
    pub face_inner: Vec<FacePort>,
    pub face_outer: Vec<FacePort>,
    pub face_left: Vec<FacePort>,   // unused by ISE, used by LB
    pub face_right: Vec<FacePort>,  // unused by ISE, used by LB
    pub num_components: usize,
    pub component_faces: Vec<FaceSet>,
    pub origin_component: Option<usize>,
    pub num_primes: usize,
    pub detail: Option<TileDetail>,
}

pub fn build_tile_with_sieve(
    a_min: i64, a_max: i64, b_min: i64, b_max: i64,
    k_sq: u64, sieve: &PrimeSieve, export_detail: bool,
) -> TileOperator;
```

The existing `build_tile_with_sieve` does everything the ISE kernel needs. The ISE kernel is a thin wrapper that calls it and extracts the I→O count.

### 3.3 `moat-kernel::kernel` (NEW)

This module defines the kernel abstraction — the hot-path unit that is swappable for CUDA.

```rust
/// Result of running the kernel on a single tile.
#[derive(Debug, Clone)]
pub struct TileResult {
    pub io_count: usize,       // number of components spanning I-face to O-face
    pub num_primes: usize,     // total primes in tile (including collar)
}

/// The kernel trait. CPU implementation today, CUDA implementation later.
pub trait TileKernel: Send + Sync {
    fn run_tile(
        &self,
        a_lo: i64,
        a_hi: i64,
        b_lo: i64,
        b_hi: i64,
        k_sq: u64,
        sieve: &PrimeSieve,
    ) -> TileResult;
}

/// CPU kernel implementation using existing build_tile_with_sieve.
pub struct CpuKernel;

impl TileKernel for CpuKernel {
    fn run_tile(
        &self,
        a_lo: i64, a_hi: i64, b_lo: i64, b_hi: i64,
        k_sq: u64, sieve: &PrimeSieve,
    ) -> TileResult {
        let tile = build_tile_with_sieve(a_lo, a_hi, b_lo, b_hi, k_sq, sieve, false);
        let io_count = tile.component_faces.iter()
            .filter(|&&f| f & FACE_INNER_BIT != 0 && f & FACE_OUTER_BIT != 0)
            .count();
        TileResult {
            io_count,
            num_primes: tile.num_primes,
        }
    }
}
```

**Why a trait, not a function pointer**: The CUDA kernel will carry state (device handles, pre-allocated buffers). A trait object (`Box<dyn TileKernel>`) or a generic parameter (`fn run<K: TileKernel>(kernel: &K, ...)`) both work. Prefer generics for zero-cost dispatch on the CPU path.

**CUDA readiness note**: The `CpuKernel::run_tile` calls `build_tile_with_sieve` which does everything: sieve lookup, spatial hash, union-find, face classification. A `CudaKernel` would replace all of this with a device launch. The sieve would be pre-uploaded to device memory. The `TileKernel` trait boundary is exactly where CPU/GPU diverges.

### 3.4 `moat-kernel::compose` (unchanged from current)

Keep the existing `compose.rs` verbatim. It is used only by `lb-probe`. ISE never imports it.

### 3.5 `moat-kernel::profile` (unchanged from current)

Keep existing `profile.rs`. Used by both ISE and LB.

### 3.6 `ise::orchestrator` (NEW — core of the ISE binary)

```rust
use moat_kernel::{
    kernel::{TileKernel, TileResult, CpuKernel},
    primes::PrimeSieve,
    profile::{PhaseTimer, ProbeProfile, get_rss_kb},
};

pub struct IseConfig {
    pub k_sq: u64,
    pub r_min: f64,
    pub r_max: f64,
    pub tile_width: f64,   // W
    pub tile_height: f64,  // H
    pub num_stripes: usize, // M
    pub trace: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct ShellRecord {
    pub shell_idx: usize,
    pub r_center: f64,
    pub a_lo: i64,
    pub a_hi: i64,
    pub io_counts: Vec<usize>,   // per-stripe I→O count
    pub f_r: f64,                // fraction of stripes with io > 0
    pub is_candidate: bool,      // f_r == 0
    pub num_primes: usize,       // total primes across all tiles in shell
    pub shell_time_ms: u64,
}

pub struct IseResult {
    pub candidates: Vec<(usize, f64)>,  // (shell_idx, r_center) for f_r == 0
    pub shells: Vec<ShellRecord>,
    pub profile: ProbeProfile,
}

pub fn run_ise<K: TileKernel>(config: &IseConfig, kernel: &K) -> IseResult {
    // 1. Compute shell bounds
    // 2. Compute stripe offsets (contiguous, centered)
    // 3. For each shell:
    //    a. Build sieve for this radial band
    //    b. par_iter over stripes → kernel.run_tile() → collect TileResult
    //    c. Compute f(r), record ShellRecord
    // 4. Return IseResult
}
```

**Stripe offset computation:**

```rust
fn stripe_offsets(num_stripes: usize, tile_width: f64) -> Vec<i64> {
    let total_width = num_stripes as f64 * tile_width;
    let origin = -(total_width / 2.0);
    (0..num_stripes)
        .map(|j| (origin + j as f64 * tile_width).floor() as i64)
        .collect()
}
```

Each stripe j has b_lo = offsets[j], b_hi = offsets[j] + W (as i64). No overlap between stripes.

**Shell bounds computation** (reuse existing logic):

```rust
fn shell_bounds(r_min: f64, r_max: f64, tile_height: f64) -> Vec<(i64, i64, f64)> {
    // Same as current probe.rs shell_bounds
    // Returns (a_lo, a_hi, r_center) for each shell
}
```

**Sieve computation per shell:**

The sieve must cover norms up to `(a_hi + collar)^2 + (max_b + collar)^2`. The current code in `probe.rs` lines 128-147 computes this correctly. Extract that logic into a helper:

```rust
fn sieve_for_shell(a_hi: i64, max_b: i64, k_sq: u64) -> PrimeSieve {
    let collar = (k_sq as f64).sqrt().ceil() as i64;
    let sieve_limit_norm = {
        let ma = (a_hi.unsigned_abs() + collar as u64) as u128;
        let mb = max_b as u128;
        let max_norm = ma.saturating_mul(ma)
            .saturating_add(mb.saturating_mul(mb))
            .min(u64::MAX as u128) as u64;
        max_norm.min(10_000_000)
    };
    let sieve_limit = ((a_hi + collar) as u64)
        .max(max_b as u64)
        .max(sieve_limit_norm);
    PrimeSieve::new(sieve_limit)
}
```

**Important: sieve is shared across all M tiles in a shell.** Build once, pass by reference to all `kernel.run_tile()` calls via Rayon.

### 3.7 `ise::output` (NEW)

```rust
use crate::orchestrator::{IseConfig, IseResult, ShellRecord};

/// Write full JSON trace to a file.
pub fn write_json_trace(path: &str, config: &IseConfig, result: &IseResult) -> std::io::Result<()>;

/// Write CSV summary (one row per shell) to a file.
pub fn write_csv_summary(path: &str, result: &IseResult) -> std::io::Result<()>;

/// Print human-readable summary to stdout.
pub fn print_summary(config: &IseConfig, result: &IseResult);
```

**JSON format:**

```json
{
  "k_sq": 26,
  "r_min": 1010000.0,
  "r_max": 1025000.0,
  "tile_width": 2000,
  "tile_height": 2000,
  "num_stripes": 32,
  "shells": [
    {
      "shell": 0,
      "r_center": 1011000.0,
      "a_lo": 1010000,
      "a_hi": 1012000,
      "io_counts": [3, 2, 4, 0, 5, ...],
      "f_r": 0.96875,
      "is_candidate": false,
      "num_primes": 48000000,
      "shell_time_ms": 1234
    }
  ],
  "summary": {
    "total_shells": 7,
    "candidates": [],
    "total_time_ms": 8654,
    "peak_rss_mb": 512.3
  }
}
```

**CSV format:**

```
shell_idx,r_center,f_r,is_candidate,num_primes,shell_time_ms
0,1011000.0,0.96875,false,48000000,1234
```

### 3.8 `ise::main` (NEW — CLI)

```rust
use clap::Parser;
use moat_kernel::kernel::CpuKernel;
use crate::orchestrator::{IseConfig, run_ise};
use crate::output::{write_json_trace, write_csv_summary, print_summary};

#[derive(Parser)]
#[command(name = "ise", about = "Independent Strip Ensemble — Gaussian moat candidate detector")]
struct Args {
    #[arg(long, required = true)]
    k_squared: u64,

    #[arg(long, default_value = "0")]
    r_min: f64,

    #[arg(long, required = true)]
    r_max: f64,

    /// Tile width W (lateral extent of each stripe). Defaults to tile_height (square tiles).
    /// Customizable for aspect ratio experiments (e.g., wide tiles for lateral coverage).
    #[arg(long)]
    tile_width: Option<f64>,

    /// Tile height H (radial extent of each tile). Controls moat resolution —
    /// smaller H resolves narrower moats but costs more shells.
    /// Typical values: 200 (fine), 500 (balanced), 2000 (coarse/fast).
    #[arg(long, default_value = "2000")]
    tile_height: f64,

    /// Tile size preset: "square" (W=H, default), "wide" (W=2H), "tall" (H=2W).
    /// Overridden by explicit --tile-width.
    #[arg(long, default_value = "square")]
    tile_shape: String,

    /// Number of independent stripes M. More stripes = better statistical coverage
    /// but linear cost. 8=fast screening, 32=production, 128=high-confidence.
    #[arg(long, default_value = "32")]
    stripes: usize,

    /// Number of Rayon threads (0 = all cores).
    #[arg(long, default_value = "0")]
    threads: usize,

    /// Write JSON trace to this path.
    #[arg(long)]
    json_trace: Option<String>,

    /// Write CSV summary to this path.
    #[arg(long)]
    csv: Option<String>,

    /// Print per-shell trace to stderr.
    #[arg(long)]
    trace: bool,

    /// Run validation against known moats (k²=2, k²=4, k²=6).
    #[arg(long)]
    validate: bool,
}

fn main() {
    let args = Args::parse();

    if args.threads > 0 {
        rayon::ThreadPoolBuilder::new()
            .num_threads(args.threads)
            .build_global()
            .unwrap();
    }

    let tile_width = args.tile_width.unwrap_or(args.tile_height); // default: square

    let config = IseConfig {
        k_sq: args.k_squared,
        r_min: args.r_min,
        r_max: args.r_max,
        tile_width,
        tile_height: args.tile_height,
        num_stripes: args.stripes,
        trace: args.trace,
    };

    let kernel = CpuKernel;
    let result = run_ise(&config, &kernel);

    print_summary(&config, &result);

    if let Some(ref path) = args.json_trace {
        write_json_trace(path, &config, &result).expect("failed to write JSON trace");
    }
    if let Some(ref path) = args.csv {
        write_csv_summary(path, &result).expect("failed to write CSV");
    }
}
```

---

## 4. Parallelism Strategy

### 4.1 Work Distribution

**Day 0 — Nested par_iter (Topology B):**
```
Shell dispatch (parallel, outer): Rayon par_iter over N shells
  └─ Stripe dispatch (parallel, inner): nested par_iter over M stripes
       └─ Kernel execution (sequential per tile)
            └─ Row-wise sieve (parallel per row): par_iter over H rows
```

ISE shells are fully independent — no acc_io accumulation, no inter-shell dependency. Rayon's indexed par_iter preserves insertion order, so output ordering is automatic. All N×M tiles can run in parallel.

Three parallelism levels:
1. **Shells × Stripes**: N×M independent tiles. At N=8, M=32 → 256 work items. Saturates any CPU.
2. **Rows within kernel**: Each tile's row-wise b-sieve parallelizes over H=2000 rows. Optional inner par_iter — enable only if outer parallelism underutilizes cores.
3. Rayon handles work-stealing across all levels automatically with nested par_iter.

**Optimized CPU — Flat pool (Topology D):**
```rust
// Pre-build RowSieveTable (shared across everything)
let table = RowSieveTable::new(1000);

// Enumerate all N×M work items, let Rayon load-balance globally
let work: Vec<(usize, usize)> = iproduct!(0..N, 0..M).collect();
let results: Vec<(usize, usize, TileResult)> = work.par_iter()
    .map(|&(s, t)| {
        let result = kernel.run_tile(shells[s], stripes[t], k_sq, &table);
        (s, t, result)
    }).collect();
```

This gives Rayon full visibility into all work items for optimal load balancing. Tiles with more primes (near axes) naturally take longer — flat pool avoids shell-level stragglers blocking the pipeline.

**Why `RowSieveTable` enables this**: Unlike old `PrimeSieve` (10MB, shell-specific sieve limit), `RowSieveTable` is tiny (~2KB for P=1000) and universal — same table works for any R. No per-shell sieve allocation needed. Build once, share everywhere.

### 4.2 Memory Management

**Per-tile allocation pattern (with row-wise sieve):**

1. `RowSieveTable`: ~2KB, built once, shared read-only. Trivially parallelizable.
2. Per-row sieve array: `[bool; W]` = 2KB at W=2000. Allocated per Rayon task, fits in L1 cache.
3. Per-tile primes vec: ~144K primes at R=10⁶ → ~2.3MB per tile. With M=32 tiles in parallel: ~74MB peak.
4. TileOperator: transient. ISE extracts io_count only, drops immediately.

**Total peak at 16 threads**: ~16 × 2.3MB (tile primes) + 16 × 2KB (sieve) + RowSieveTable ≈ **~37MB**. Excellent.

**Optimization opportunity**: `build_tile_io_only` — skip Left/Right face port collection. Saves ~20% memory per tile. Phase 3 optimization.

### 4.3 Sieve Sharing

`RowSieveTable` is `&RowSieveTable` — immutable reference containing ~170 small primes with precomputed √(-1) mod p. ~2KB total, fits in any cache level. Shared across all Rayon threads with zero contention. No per-shell or per-tile sieve allocation needed — the table is universal across all R values (the MR witness set adapts to max_norm via a runtime check).

---

## 5. CUDA Readiness

### 5.1 The Trait Boundary

```rust
pub trait TileKernel: Send + Sync {
    fn run_tile(&self, a_lo: i64, a_hi: i64, b_lo: i64, b_hi: i64,
                k_sq: u64, table: &RowSieveTable) -> TileResult;
}
```

For a CUDA implementation:

```rust
pub struct CudaKernel {
    device: CudaDevice,
    sieve_gpu: DeviceBuffer<bool>,  // pre-uploaded sieve
    // Pre-allocated per-tile buffers (prime coords, UF arrays, etc.)
}

impl TileKernel for CudaKernel {
    fn run_tile(&self, ...) -> TileResult {
        // 1. Enumerate primes on device (sieve already uploaded)
        // 2. Spatial hash on device
        // 3. Union-find on device
        // 4. Face classification on device
        // 5. Reduce io_count on device
        // 6. Copy io_count back to host
    }
}
```

### 5.2 Batch Launch

For GPU, the orchestrator would batch M tiles into a single kernel launch rather than M individual launches. The trait boundary supports this via a batch variant:

```rust
pub trait TileKernelBatch: Send + Sync {
    fn run_tiles_batch(
        &self,
        tiles: &[(i64, i64, i64, i64)],  // (a_lo, a_hi, b_lo, b_hi) per tile
        k_sq: u64,
        sieve: &PrimeSieve,
    ) -> Vec<TileResult>;
}
```

The CPU implementation of `TileKernelBatch` simply calls `run_tile` in a Rayon par_iter. The CUDA implementation launches a single kernel with M thread blocks.

This batch trait is a Phase 2 addition — Phase 1 uses the single-tile trait with Rayon.

### 5.3 What Must Be Isolatable

The CUDA kernel replaces everything inside `build_tile_from_primes`:
1. Prime enumeration (sieve lookup over rectangle) — GPU can parallelize over rows.
2. Spatial hash construction — GPU atomic inserts into cell lists.
3. Neighbor search + union-find — GPU parallel union-find (e.g., Jayanti-Tarjan or label propagation).
4. Face classification — GPU parallel scan over primes.
5. I→O component counting — GPU reduction.

The `TileKernel::run_tile` boundary encapsulates all five steps. The orchestrator never reaches inside.

---

## 6. LB Compatibility

### 6.1 Shared Kernel

Both ISE and LB modes use the same `build_tile_with_sieve` function (and the same `TileKernel` trait). The tile is the atomic unit of work in both cases.

**ISE mode**: calls `kernel.run_tile()`, extracts `io_count`, discards tile.

**LB mode**: calls `build_tile_with_sieve()` directly (needs full `TileOperator` with face ports for composition), then composes tiles via `compose_horizontal` and `compose_vertical`.

The `TileKernel` trait returns `TileResult` (just counts) which is sufficient for ISE. LB mode calls the underlying `build_tile_with_sieve` directly since it needs the full `TileOperator`. This is fine — both paths use the same sieve and the same prime enumeration + union-find code. The trait is for ISE/CUDA; LB mode uses the library functions directly.

### 6.2 Module Ownership

| Module | Used by ISE | Used by LB |
|--------|------------|------------|
| `moat-kernel::primes` | Yes | Yes |
| `moat-kernel::tile` | Yes (via kernel trait) | Yes (directly) |
| `moat-kernel::kernel` | Yes | No (uses tile directly) |
| `moat-kernel::compose` | No | Yes |
| `moat-kernel::profile` | Yes | Yes |

### 6.3 Future LB Binary

The current `tile-probe` binary becomes `lb-probe`. Its code moves into `crates/lb-probe/` with minimal changes:
- `probe.rs` → `crates/lb-probe/src/probe.rs` (import paths change from `crate::` to `moat_kernel::`)
- `main.rs` → `crates/lb-probe/src/main.rs` (same import path changes)
- `compose.rs`, `tile.rs`, `primes.rs`, `profile.rs` → already in `moat-kernel`

---

## 7. Migration Path

### 7.1 What to Keep (Verbatim)

| File | Destination | Changes |
|------|-------------|---------|
| `tile.rs` | `moat-kernel/src/tile.rs` | None (except `pub(crate)` → `pub` for `SimpleUF` if needed by compose) |
| `primes.rs` | `moat-kernel/src/primes.rs` | None |
| `compose.rs` | `moat-kernel/src/compose.rs` | Import paths: `crate::tile` → `crate::tile` (same, it's in the same crate) |
| `profile.rs` | `moat-kernel/src/profile.rs` | None |

### 7.2 What to Move

| File | From | To | Changes |
|------|------|----|---------|
| `probe.rs` | `tile-probe/src/` | `crates/lb-probe/src/probe.rs` | Change `crate::` imports to `moat_kernel::` |
| `main.rs` | `tile-probe/src/` | `crates/lb-probe/src/main.rs` | Change `crate::` imports to `moat_kernel::` and `crate::probe` to local module |

### 7.3 What to Write New

| File | Purpose |
|------|---------|
| `moat-kernel/src/lib.rs` | Re-export modules |
| `moat-kernel/src/kernel.rs` | TileKernel trait + CpuKernel |
| `ise/src/main.rs` | CLI |
| `ise/src/orchestrator.rs` | Shell loop + stripe dispatch |
| `ise/src/output.rs` | JSON/CSV output |

### 7.4 Solver Dependency

The `solver` crate at `gaussian-moat-cuda/solver/` is listed in the current Cargo.toml (`solver = { path = "../solver" }`) but is **never imported in any source file**. The prime-sieving code is entirely self-contained in `primes.rs`. Drop the `solver` dependency from `moat-kernel`. This eliminates a build-time dependency and simplifies the workspace.

---

## 8. Verification Gates

Each gate describes a specific observable behavior that proves the module works. Not "tests pass" — the specific thing that must be true.

### Gate 1: Kernel Correctness

**Test**: Run `CpuKernel::run_tile` on the rectangle `(0, 2, 0, 2)` with `k_sq=2`. Verify `io_count > 0` (the origin's neighborhood connects I-face to O-face in this small tile). Run on `(100, 102, 0, 2)` with `k_sq=2`. Verify `io_count == 0` (at R=100, k²=2 is far too small for connectivity — moat exists).

**Test**: Run `CpuKernel::run_tile` on `(0, 20, 0, 20)` with `k_sq=2`. Verify result matches `io_crossing_count(build_tile_with_sieve(...))` — the kernel wrapper agrees with the direct tile build.

### Gate 2: Sieve Sharing

**Test**: Build a sieve with limit L. Run two tiles with different b-ranges but the same a-range using the same sieve reference. Verify both return correct results (no data races, no stale state). This is a Rayon concurrency test.

### Gate 3: ISE Orchestrator — Known Moat Detection

**Test**: Run ISE with `k_sq=2, r_min=0, r_max=30, W=4, H=4, M=8`. The known moat for k²=2 is at R≈8-10. Verify: at least one shell has `f_r == 0.0` (strong candidate detected). Verify: shells near R=0 have `f_r > 0` (connectivity alive near origin).

**Test**: Run ISE with `k_sq=6, r_min=0, r_max=60, W=8, H=8, M=8`. Known moat for k²=6 at R≈30-40. Verify candidate detection.

### Gate 4: ISE Orchestrator — No False Negatives on Connected Region

**Test**: Run ISE with `k_sq=26, r_min=0, r_max=1000, W=200, H=200, M=8`. At these small radii, k²=26 is deeply supercritical. Verify: `f_r > 0` for all shells (no false moat detection). This confirms the ISE does not generate spurious candidates in connected regions.

### Gate 5: JSON/CSV Output

**Test**: Run ISE, write JSON trace. Parse the JSON back. Verify: all fields present, shell count matches expected, io_counts arrays have length M, f_r values are in [0, 1].

### Gate 6: Tsuchimura Validation (Integration)

**Test**: Run ISE with `k_sq=26, r_min=1010000, r_max=1025000, W=2000, H=2000, M=32`. Verify: at least one shell near R≈1,016,000 has `f_r == 0.0` or significantly depressed `f_r`. This validates against the known Tsuchimura moat.

This is an integration test — it may take several minutes. Run it as a separate `--validate` mode, not in the default test suite.

### Gate 7: LB Binary Still Works

**Test**: After migration, run `lb-probe --validate` (the existing validation suite: k²=2 r_max=20, k²=4 r_max=40, k²=6 r_max=50). All must still pass. This confirms the workspace restructuring did not break the existing code.

### Gate 8: Performance Baseline

**Test**: Run ISE on `k_sq=26, r_min=0, r_max=10000, W=200, H=200, M=8` with `--profile`. Record wall-clock time and peak RSS. Verify: time is reasonable (< 30s on a modern laptop), RSS is bounded (< 500 MB). This establishes a baseline for future optimization comparison.

---

## 9. Phase Plan

### Phase 1: Workspace Restructuring (no new functionality)

**Deliverable**: Working workspace with `moat-kernel` and `lb-probe`. All existing tests pass. No new code beyond `moat-kernel/src/lib.rs` and Cargo.toml files.

Steps:
1. Create workspace Cargo.toml at `tile-probe/Cargo.toml` (or rename the directory).
2. Create `crates/moat-kernel/` with `primes.rs`, `tile.rs`, `compose.rs`, `profile.rs` moved from `src/`.
3. Create `crates/lb-probe/` with `main.rs` and `probe.rs` moved from `src/`.
4. Fix all import paths.
5. `cargo test` — all existing tests pass.
6. `cargo run --bin lb-probe -- --validate` — all validations pass.

**Gate**: Gate 7.

### Phase 2: Row-Wise Sieve + TileKernel Trait

**Deliverable**: `moat-kernel::primes` with `RowSieveTable` and `gaussian_primes_in_rect_rowsieve` (as specified in 3.1). `moat-kernel::kernel` module with `TileKernel` trait and `CpuKernel` using the row-wise sieve. Unit tests proving both sieve and kernel correctness.

Steps:
1. Implement `RowSieveTable::new(sieve_bound)`: Eratosthenes up to P, Tonelli-Shanks for √(-1) mod p for each p ≡ 1 (mod 4), collect primes p ≡ 3 (mod 4) separately.
2. Implement `primes_in_row(a, b_lo, W, &table)`: parity filter → p≡1(4) sieve → p≡3(4) sieve → 5-witness MR on survivors.
3. Implement `gaussian_primes_in_rect_rowsieve`: Rayon par_iter over rows, calling `primes_in_row`.
4. Keep legacy `PrimeSieve` + `gaussian_primes_in_rect_with_sieve` for `lb-probe` backward compat.
5. Write `moat-kernel/src/kernel.rs` with the `TileKernel` trait and `CpuKernel` impl using the row-wise path.
6. Add `io_crossing_count` as a public function in `moat-kernel::tile` (move from probe.rs).
7. **Correctness gate**: Run both old (MR-only) and new (row-sieve + MR) paths on identical rectangles at R=10², R=10⁴, R=10⁶. Assert identical prime sets. Zero tolerance.
8. **Performance gate**: Benchmark single tile at R=10⁶, W=H=2000. New path must be ≥5× faster than MR-only path.
9. Write unit tests for Gate 1, Gate 2.

**Gate**: Gate 1, Gate 2, plus sieve correctness cross-check and perf benchmark.

### Phase 3: ISE Orchestrator

**Deliverable**: `ise/src/orchestrator.rs` with `run_ise()` function. Core algorithm working. No CLI or output yet — driven by tests.

Steps:
1. Write `ise/src/orchestrator.rs` with `IseConfig`, `ShellRecord`, `IseResult`, `run_ise()`.
2. Implement shell bounds computation (reuse from probe.rs).
3. Implement stripe offset computation.
4. Implement the shell loop with Rayon par_iter over stripes.
5. Write integration tests for Gate 3 and Gate 4.

**Gate**: Gate 3, Gate 4.

### Phase 4: CLI + Output

**Deliverable**: Working `ise` binary with CLI, JSON trace, CSV summary.

Steps:
1. Write `ise/src/main.rs` with clap argument parsing.
2. Write `ise/src/output.rs` with JSON trace and CSV summary writers.
3. Wire CLI → orchestrator → output.
4. Write tests for Gate 5.
5. Add `--validate` mode with small-k validation cases.

**Gate**: Gate 5, Gate 8.

### Phase 5: Tsuchimura Validation Run

**Deliverable**: Empirical validation that ISE detects the known k²=26 moat at R≈1,015,639.

Steps:
1. Run: `ise --k-squared 26 --r-min 1010000 --r-max 1025000 --tile-height 2000 --stripes 32 --json-trace research/2026-03-21-k26-ise-validation.json --trace`
2. Analyze JSON trace: check f(r) profile across shells near the moat.
3. If f(r) drops to 0: validation passes. Record the shell and radius.
4. If f(r) does not drop to 0: investigate. Possible causes:
   - Tile depth too large (moat narrower than H). Try H=500 or H=200.
   - Strip width too small (paths escape laterally). Try W=4000.
   - Bug in kernel or orchestrator. Debug with small known cases first.

**Gate**: Gate 6.

### Phase 6: Performance Optimization (Optional, Pre-CUDA)

**Deliverable**: Optimized CPU kernel for ISE throughput.

Potential optimizations (prioritize by measured bottleneck):
1. `build_tile_io_only`: Skip Left/Right face port collection. Avoid allocating FacePort vecs for faces ISE doesn't use. This is a new function alongside `build_tile_with_sieve`, not a replacement.
2. Pre-allocate prime vec per thread: Use a thread-local `Vec<(i64, i64)>` that is cleared and reused per tile, avoiding repeated allocation.
3. Spatial hash cell size tuning: Current cell size is `collar`. For large k, this may create too many cells. Benchmark alternative cell sizes.
4. Batch sieve: For wide radial campaigns, the sieve limit may be similar across adjacent shells. Cache and reuse sieves when limits match.

**Gate**: Gate 8 (must improve over Phase 4 baseline).

---

## 10. Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| Per-tile I→O does not drop to 0 at Tsuchimura moat | ISE doesn't work as designed | Reduce tile height H until moat is resolved. The moat gap is ~sqrt(k²)≈5.1 in norm, but manifests as a density drop over a wider radial range. Empirical: the composed band_io drops to 0 at H=2000, so individual tiles should too — but needs verification. |
| Sieve memory at large R | OOM for R > 10^6 | Sieve limit is capped at 10M in current code. Miller-Rabin fallback handles norms beyond sieve. Monitor RSS. |
| Workspace restructuring breaks solver dependency | Build failure | The solver crate dependency may need path updates. Test build early in Phase 1. |
| Rayon contention on sieve at high M | Slowdown | PrimeSieve is read-only — no contention. But if M > num_cores, thread scheduling overhead may matter. Benchmark M=32 vs M=64 vs M=128. |
