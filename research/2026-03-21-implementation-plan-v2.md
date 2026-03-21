# Implementation Plan v2 — Independent Strip Ensemble (ISE)

**Date:** 2026-03-21
**Status:** plan
**Engine:** coordinator (R. Jenkins)
**Supersedes:** `2026-03-21-implementation-plan.md`

---

## 1. Architecture Overview

### 1.1 Workspace Layout

```
gaussian-moat-cuda/
  tile-probe/
    Cargo.toml          # workspace root
    crates/
      moat-kernel/      # shared library crate
        Cargo.toml
        src/
          lib.rs        # pub mod primes, tile, kernel, compose, profile;
          primes.rs     # PrimeSieve (legacy), RowSieveTable (optimized), gaussian prime enumeration
          tile.rs       # TileOperator, build_tile_with_sieve, SimpleUF, face types
          kernel.rs     # TileKernel trait, CpuKernel, TileResult (rich, mode-agnostic)
          compose.rs    # horizontal/vertical composition (LB mode only)
          profile.rs    # ProbeProfile, PhaseTimer, get_rss_kb
      ise/              # Independent Strip Ensemble binary
        Cargo.toml
        src/
          main.rs       # CLI entry point (clap)
          orchestrator.rs  # shell loop, stripe dispatch, f(r) aggregation
          output.rs     # JSON trace writer, CSV summary writer
      lb-probe/         # Lower Bound probe binary (current tile-probe, renamed)
        Cargo.toml
        src/
          main.rs       # current main.rs (migrated, import paths updated)
          probe.rs      # current probe.rs (migrated, import paths updated)
```

### 1.2 Workspace Cargo.toml

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

### 1.3 Design Principles

1. **One kernel, two modes.** The kernel is the universal atom. It computes ALL face connectivity for every tile. ISE reads `io_count`. LB reads face component lists for chaining. The kernel does not know what mode it is in.

2. **Variable tile sizes.** Tile dimensions `(W, H)` are runtime parameters, not compile-time constants. The kernel handles any `(W, H)`. The orchestrator can mix tile sizes within a campaign. Storage records tile dimensions per record.

3. **Stripe-oriented storage.** Data is organized by stripe (vertical tower), not by shell. Each stripe is a sequence of TileRecords from r_min to r_max.

4. **Post-processing adjacency.** `connects_left` and `connects_right` are computed by comparing face component sets between adjacent tiles AFTER kernel execution (LB mode only). In ISE mode, `connects_below` is set to `None` -- correct vertical matching requires coordinate-level port comparison via `compose_vertical`, which is a future LB-mode feature. See Section 2.2.

---

## 2. Data Model

### 2.1 TileResult (Rich, Mode-Agnostic)

The kernel returns a `TileResult` for every tile, always computing all face connectivity:

```rust
/// Result of running the kernel on a single tile.
/// Contains full face connectivity -- mode-agnostic.
/// ISE reads io_count. LB reads face component lists.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TileResult {
    // --- Cross-face component counts ---
    pub io_count: usize,       // components touching both I-face and O-face
    pub il_count: usize,       // components touching both I-face and Left-face
    pub ir_count: usize,       // components touching both I-face and Right-face
    pub ol_count: usize,       // components touching both O-face and Left-face
    pub or_count: usize,       // components touching both O-face and Right-face
    pub lr_count: usize,       // components touching both Left-face and Right-face

    // --- Per-face component ID lists (for adjacency matching in LB mode) ---
    pub i_face_components: Vec<usize>,   // component IDs touching I-face
    pub o_face_components: Vec<usize>,   // component IDs touching O-face
    pub l_face_components: Vec<usize>,   // component IDs touching Left-face
    pub r_face_components: Vec<usize>,   // component IDs touching Right-face

    pub num_primes: usize,
}
```

**Extraction from TileOperator** (existing code is the source of truth):

```rust
impl TileResult {
    /// Build a TileResult from a TileOperator by inspecting component_faces
    /// and collecting per-face component ID sets.
    pub fn from_tile_operator(tile: &TileOperator) -> Self {
        let mut io = 0; let mut il = 0; let mut ir = 0;
        let mut ol = 0; let mut or_ = 0; let mut lr = 0;
        let mut i_comps = Vec::new();
        let mut o_comps = Vec::new();
        let mut l_comps = Vec::new();
        let mut r_comps = Vec::new();

        for (id, &faces) in tile.component_faces.iter().enumerate() {
            let has_i = faces & FACE_INNER_BIT != 0;
            let has_o = faces & FACE_OUTER_BIT != 0;
            let has_l = faces & FACE_LEFT_BIT  != 0;
            let has_r = faces & FACE_RIGHT_BIT != 0;

            if has_i && has_o { io += 1; }
            if has_i && has_l { il += 1; }
            if has_i && has_r { ir += 1; }
            if has_o && has_l { ol += 1; }
            if has_o && has_r { or_ += 1; }
            if has_l && has_r { lr += 1; }

            if has_i { i_comps.push(id); }
            if has_o { o_comps.push(id); }
            if has_l { l_comps.push(id); }
            if has_r { r_comps.push(id); }
        }

        TileResult {
            io_count: io, il_count: il, ir_count: ir,
            ol_count: ol, or_count: or_, lr_count: lr,
            i_face_components: i_comps,
            o_face_components: o_comps,
            l_face_components: l_comps,
            r_face_components: r_comps,
            num_primes: tile.num_primes,
        }
    }
}
```

### 2.2 Stripe-Oriented Storage

```rust
/// A single tile's record within a stripe, including position, dimensions, and results.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TileRecord {
    pub a_lo: i64,
    pub b_lo: i64,
    pub width: u32,          // W -- variable per tile
    pub height: u32,         // H -- variable per tile
    pub result: TileResult,  // full face connectivity
    pub connects_below: Option<bool>,  // Always None in ISE mode; LB mode uses compose_vertical
    pub connects_left: Option<bool>,   // LB mode post-processing only
    pub connects_right: Option<bool>,  // LB mode post-processing only
    pub time_ms: u64,
}

/// A vertical stripe: a sequence of tile records from r_min to r_max.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StripeRecord {
    pub stripe_id: usize,
    pub b_lo: i64,
    pub tiles: Vec<TileRecord>,  // ordered by a_lo (radially outward)
}
```

**Post-processing adjacency:**

In ISE mode, `connects_below` is always `None`. Correct vertical matching requires coordinate-level port comparison -- checking whether any O-face prime in the lower tile is within distance sqrt(k_sq) of any I-face prime in the upper tile. This logic already exists in `compose_vertical` (LB mode) but is NOT available as a lightweight post-processing step on `TileResult` alone, because component IDs are tile-local and do not carry coordinate information.

**Do not ship a placeholder** that merely checks `!o_face_components.is_empty() && !i_face_components.is_empty()` -- that would return `true` for any pair of non-empty tiles, producing misleading data.

For LB mode (future), vertical connectivity is computed by the full `compose_vertical` pipeline operating on `TileOperator` data. For ISE mode, `connects_below = None` is the correct value -- ISE detection depends only on per-tile `io_count`, not inter-tile adjacency.

### 2.3 Shell Aggregation (ISE-specific)

```rust
/// Per-shell aggregation for ISE mode.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShellRecord {
    pub shell_idx: usize,
    pub r_center: f64,
    pub a_lo: i64,
    pub a_hi: i64,
    pub io_counts: Vec<usize>,    // per-stripe I->O count (length M)
    pub f_r: f64,                 // fraction of stripes with io > 0
    pub is_candidate: bool,       // f_r == 0
    pub num_primes: usize,        // total primes across all tiles in shell
    pub shell_time_ms: u64,
}
```

---

## 3. Mode-Agnostic Kernel Specification

### 3.1 TileKernel Trait

```rust
/// The kernel trait. CPU implementation today, CUDA tomorrow.
/// Mode-agnostic: computes full face connectivity for every tile.
///
/// The trait does NOT take a sieve/prime-source parameter. The sieve strategy
/// is an implementation detail of each backend:
/// - CpuKernel holds a &PrimeSieve or &RowSieveTable internally.
/// - CudaKernel uses device-side sieve in shared memory.
/// This keeps the trait clean and compatible with all backends.
pub trait TileKernel: Send + Sync {
    /// Run the kernel on a single tile. Returns rich TileResult with all face connectivity.
    fn run_tile(
        &self,
        a_lo: i64,
        a_hi: i64,
        b_lo: i64,
        b_hi: i64,
        k_sq: u64,
    ) -> TileResult;
}
```

### 3.2 CpuKernel Implementation

The `CpuKernel` holds a reference to its prime source internally. This can be a `PrimeSieve` (legacy path) or a `RowSieveTable` (optimized path). The trait caller does not know or care which.

```rust
/// CPU kernel backed by a PrimeSieve (legacy/Day-0 path).
pub struct CpuKernel<'a> {
    sieve: &'a PrimeSieve,
}

impl<'a> CpuKernel<'a> {
    pub fn new(sieve: &'a PrimeSieve) -> Self {
        Self { sieve }
    }
}

impl<'a> TileKernel for CpuKernel<'a> {
    fn run_tile(
        &self,
        a_lo: i64, a_hi: i64, b_lo: i64, b_hi: i64,
        k_sq: u64,
    ) -> TileResult {
        let tile = build_tile_with_sieve(a_lo, a_hi, b_lo, b_hi, k_sq, self.sieve, false);
        TileResult::from_tile_operator(&tile)
    }
}

/// CPU kernel backed by a RowSieveTable (optimized path).
pub struct CpuKernelRowSieve<'a> {
    table: &'a RowSieveTable,
}

impl<'a> CpuKernelRowSieve<'a> {
    pub fn new(table: &'a RowSieveTable) -> Self {
        Self { table }
    }
}

impl<'a> TileKernel for CpuKernelRowSieve<'a> {
    fn run_tile(
        &self,
        a_lo: i64, a_hi: i64, b_lo: i64, b_hi: i64,
        k_sq: u64,
    ) -> TileResult {
        let primes = gaussian_primes_in_rect_rowsieve(a_lo, a_hi, b_lo, b_hi, self.table);
        let tile = build_tile_from_primes(a_lo, a_hi, b_lo, b_hi, k_sq, primes, false);
        TileResult::from_tile_operator(&tile)
    }
}
```

### 3.3 Kernel Invariants

1. The kernel receives tile bounds `(a_lo, a_hi, b_lo, b_hi)` and `k_sq`. It does NOT receive mode information or a sieve parameter. The prime source is internal to each kernel implementation.
2. The kernel always computes the collar expansion: `collar = ceil(sqrt(k_sq))`.
3. The kernel always enumerates primes in the expanded rectangle `[a_lo - collar, a_hi + collar] x [b_lo - collar, b_hi + collar]`.
4. The kernel always performs spatial hashing, union-find, and face classification for ALL four faces (I, O, L, R).
5. The returned `TileResult` contains all cross-face counts and per-face component ID lists.
6. The kernel is deterministic: same inputs always produce the same `TileResult`.

### 3.4 CUDA Mapping

One CUDA thread block maps to one tile kernel invocation:

```
CUDA Grid:
  blockIdx.x = stripe index (0..M-1)
  blockIdx.y = shell batch index (0..B-1, where B = batch size)

Within each block:
  threads collaborate on:
    1. Prime enumeration (parallel per-row sieve)
    2. Spatial hash construction (atomic inserts)
    3. Union-find (parallel label propagation)
    4. Face classification (parallel scan)
    5. Cross-face count reduction (warp reduce)
```

The batch launch processes `M * B` tiles in a single kernel launch. Results are written to a device-side `TileResult` array and copied back to host.

---

## 4. Sieve Strategy

### 4.1 Why Segmented Norm Sieve Is Wrong

At R=10^6, a 2000x2000 tile centered at `(R, 0)` has norms ranging from ~`R^2` to ~`(R+2000)^2`, a span of ~4*10^9. The tile contains only 4M lattice points (fill factor 0.1% of the norm range). Sieving a 4B-element boolean array costs 2-3s per tile in both memory and time. This is WORSE than per-point Miller-Rabin.

### 4.2 Day 0: Trial Division + Reduced MR Witnesses

Replace the current 12-witness MR with a cheaper pipeline:

```rust
/// Small primes for trial division (all primes up to 199, 46 primes).
const SMALL_PRIMES: [u64; 46] = [2, 3, 5, 7, 11, 13, ..., 193, 197, 199];

/// Reduced MR witness set. {2, 3, 5, 7} is provably correct for all
/// n < 3,215,031,751 (~3.2*10^9). For norms up to 2*10^12 (R~10^6),
/// use {2, 3, 5, 7, 11} (correct for n < 2,152,302,898,747).
/// For norms up to 3.41*10^14, use {2, 3, 5, 7, 11, 13, 17} (7 witnesses).
const MR_WITNESSES_4: [u64; 4] = [2, 3, 5, 7];
const MR_WITNESSES_5: [u64; 5] = [2, 3, 5, 7, 11];
const MR_WITNESSES_7: [u64; 7] = [2, 3, 5, 7, 11, 13, 17];

/// Threshold: 5 witnesses cover n < 2.152×10¹².
const MAX_NORM_5_WITNESS: u64 = 2_152_302_898_747;
/// Threshold: 7 witnesses cover n < 3.41×10¹⁴.
const MAX_NORM_7_WITNESS: u64 = 341_550_071_728_321;

fn is_prime_fast(n: u64) -> bool {
    // 1. Trial division by SMALL_PRIMES (46 divisions, ~10ns)
    for &p in &SMALL_PRIMES {
        if n == p { return true; }
        if n % p == 0 { return false; }
    }
    // 2. Reduced MR with runtime bounds check (NOT debug_assert).
    //    If norm exceeds the 5-witness threshold, fall back to 7 witnesses.
    //    This is a single branch — negligible cost.
    if n < 3_215_031_751 {
        miller_rabin(n, &MR_WITNESSES_4)
    } else if n < MAX_NORM_5_WITNESS {
        miller_rabin(n, &MR_WITNESSES_5)
    } else {
        assert!(n < MAX_NORM_7_WITNESS,
            "norm {} exceeds 7-witness MR correctness bound (3.41e14)", n);
        miller_rabin(n, &MR_WITNESSES_7)
    }
}
```

**Expected performance**: Trial division by primes up to 199 eliminates ~77% of composites before MR (Mertens-product estimate: `1 - prod(1-1/p)` for p up to 199). Reduced witnesses (4-5 instead of 12) cut MR cost by ~2.5x on survivors. Combined: estimated 3-5x speedup over the current 12-witness path (to be confirmed by independent benchmarking). Exact speedup depends on cache effects and branch prediction at the target R value.

### 4.3 Optimized Path: Row-Wise b-Dimension Sieve

For fixed row `a`, ask: for which `b in [b_lo, b_lo+W)` is `a^2 + b^2` prime?

```rust
/// Precomputed table for row-wise sieve. Built once, shared across all tiles.
/// Contains (p, sqrt(-1) mod p) for small primes p = 1 (mod 4),
/// plus primes p = 3 (mod 4).
pub struct RowSieveTable {
    /// (p, sqrt_neg1_mod_p) for each prime p = 1 (mod 4), p <= SIEVE_BOUND
    pub primes_1mod4: Vec<(u32, u32)>,
    /// primes p = 3 (mod 4), p <= SIEVE_BOUND
    pub primes_3mod4: Vec<u32>,
}

impl RowSieveTable {
    /// Build the table. SIEVE_BOUND=1000 -> 86 primes p=1(4) + 87 primes p=3(4).
    /// Eratosthenes for prime list, Tonelli-Shanks for sqrt(-1) mod p.
    pub fn new(sieve_bound: u32) -> Self;
}

/// Enumerate Gaussian primes in a single row using the sieve table.
/// Returns primes (a, b) for fixed a, b in [b_lo, b_lo + w).
fn primes_in_row(a: i64, b_lo: i64, w: usize, table: &RowSieveTable) -> Vec<(i64, i64)> {
    let mut composites = vec![false; w];  // 2KB for w=2000, fits L1

    // Step 1: Parity filter
    // If a = b (mod 2), then a^2 + b^2 = 0 (mod 2) -> composite.
    let parity = a.rem_euclid(2);
    for i in 0..w {
        if ((b_lo + i as i64).rem_euclid(2)) == parity {
            composites[i] = true;
        }
    }

    // Step 2: For each small prime p = 1 (mod 4):
    // p | (a^2 + b^2) iff b = +/- a * sqrt(-1) (mod p)
    // Two residue classes, sieve with stride p.
    for &(p, sqrt_neg1) in &table.primes_1mod4 {
        let p = p as i64;
        let s = sqrt_neg1 as i64;
        let a_mod_p = a.rem_euclid(p);
        let r1 = (a_mod_p * s).rem_euclid(p);
        let r2 = (p - r1) % p;

        for r in [r1, r2] {
            let first = (r - b_lo.rem_euclid(p)).rem_euclid(p) as usize;
            let mut j = first;
            while j < w {
                composites[j] = true;
                j += p as usize;
            }
        }
    }

    // Step 3: For each small prime p = 3 (mod 4):
    // p | (a^2 + b^2) only if p | a AND p | b.
    // Only sieve when p divides the current row's a value.
    for &p in &table.primes_3mod4 {
        let p = p as i64;
        if a.rem_euclid(p) != 0 { continue; }
        let first = ((-b_lo).rem_euclid(p)) as usize;  // first b with b = 0 (mod p)
        let mut j = first;
        while j < w {
            composites[j] = true;
            j += p as usize;
        }
    }

    // Step 4: Axis-prime special cases.
    // Gaussian primes on the real axis (a, 0) have norm a² (composite for |a|>1),
    // so the norm-based MR test rejects them. They must be detected explicitly:
    //   (a, 0) is Gaussian prime iff |a| is a rational prime AND |a| ≡ 3 (mod 4).
    // Similarly for the imaginary axis (0, b):
    //   (0, b) is Gaussian prime iff |b| is a rational prime AND |b| ≡ 3 (mod 4).
    //
    // For b=0 in range: check if |a| is prime AND |a| ≡ 3 (mod 4) AND |a| ≥ 2.
    // For a=0 row: check each b for imaginary axis prime condition.

    let mut primes = Vec::new();

    // Handle b=0 axis prime (if b=0 is in range)
    if b_lo <= 0 && 0 < b_lo + w as i64 {
        let idx_b0 = (0 - b_lo) as usize;
        let abs_a = a.unsigned_abs();
        if abs_a >= 2 && abs_a % 4 == 3 && is_prime_fast(abs_a) {
            primes.push((a, 0i64));
        }
        // Mark b=0 as handled so the general path skips it
        composites[idx_b0] = true;
    }

    // Handle a=0 row: all primes are axis primes
    if a == 0 {
        for i in 0..w {
            if composites[i] { continue; }
            let b = b_lo + i as i64;
            let abs_b = b.unsigned_abs();
            if abs_b >= 2 && abs_b % 4 == 3 && is_prime_fast(abs_b) {
                primes.push((0, b));
            }
        }
        return primes;
    }

    // Step 5: General case — survivors -> norm-based MR test
    for i in 0..w {
        if composites[i] { continue; }
        let b = b_lo + i as i64;
        if b == 0 { continue; }  // already handled above
        let norm = (a as i128 * a as i128 + b as i128 * b as i128) as u64;
        if norm < 2 { continue; }
        if is_prime_fast(norm) {
            primes.push((a, b));
        }
    }
    primes
}

/// Enumerate Gaussian primes in a rectangle using row-wise sieve.
/// Parallelizes over rows with Rayon.
pub fn gaussian_primes_in_rect_rowsieve(
    a_min: i64, a_max: i64, b_min: i64, b_max: i64,
    table: &RowSieveTable,
) -> Vec<(i64, i64)> {
    let w = (b_max - b_min + 1) as usize;
    (a_min..=a_max)
        .into_par_iter()
        .flat_map(|a| primes_in_row(a, b_min, w, table))
        .collect()
}
```

**Key properties of RowSieveTable**:
- ~2KB total (170 small primes with precomputed sqrt(-1) mod p). Fits any cache level.
- Read-only, `Send + Sync`. Zero contention across Rayon threads.
- Universal across all R values (the MR witness set adapts to norm magnitude at runtime).
- Per-row sieve array: `[bool; W]` = 2KB at W=2000. L1-hot. Perfect locality.
- Each row is independent -> Rayon par_iter over rows within a tile.
- Replaces the need for per-shell `PrimeSieve` allocation entirely.

**Expected performance**: ~53ms per 2000x2000 tile at R=10^6 single-thread. ~6ms with 8 Rayon threads.

### 4.4 Axis Primes

Axis primes require special handling because the norm-based MR test does NOT detect them. A Gaussian integer on the real axis `(a, 0)` has norm `a²`, which is composite for `|a| > 1`. The MR test on `a²` always returns false. Same for imaginary axis `(0, b)` with norm `b²`.

**The rule**: `(a, 0)` is a Gaussian prime iff `|a|` is a rational prime AND `|a| ≡ 3 (mod 4)`. Symmetrically for `(0, b)`.

**Implementation in `primes_in_row`**: The function handles axis primes explicitly before the general norm-based path:
- **b=0 in range**: Check if `|a| >= 2`, `|a|` is a rational prime, and `|a| ≡ 3 (mod 4)`. If so, push `(a, 0)`. Mark b=0 as handled.
- **a=0 row**: The entire row consists of axis primes. For each survivor `b`, check if `|b| >= 2`, `|b|` is a rational prime, and `|b| ≡ 3 (mod 4)`. Return early -- do NOT fall through to the norm-based path.
- **General case** (a != 0, b != 0): Use norm-based MR as before.

The legacy API (`gaussian_primes_in_rect_with_sieve`) has its own axis-prime logic (lines 168-186). Gate 2 cross-checks both paths to confirm they produce identical prime sets including axis primes.

### 4.5 CUDA Sieve Path

On GPU, the calculus changes: GPU threads are cheap, regular memory access maps well to SIMD.

- **Per-thread trial division in shared memory**: Each thread handles one `(a, b)` point. Trial division by small primes in registers. MR on survivors.
- **Per-row segmented sieve on GPU**: Each warp processes one row. Shared memory holds the `[bool; W]` sieve array (2KB, fits in 48KB shared memory). Coalesced memory access.

The `TileKernel` trait boundary isolates this decision. CPU uses `RowSieveTable`. GPU uses device-side sieve. Both return the same `TileResult`.

---

## 5. Parallelism

### 5.1 Key Insight: No Inter-Shell Dependency in ISE

ISE shells are independent. There is no accumulated state, no composition between shells, no sequential dependency. This means shells AND stripes can all be parallelized simultaneously. The full work set is `N * M` independent tiles.

**Ordering constraint**: Stripe bounds computation must complete before sieve/kernel construction. The `CpuKernel` (or `CpuKernelRowSieve`) must be constructed with a prime source whose limit covers the maximum norm across all tiles. For `CpuKernel` with `PrimeSieve`, this means computing all stripe offsets first, then calling `sieve_limit_for_tiles` (see `sieve_limit_for_batch` in Section 10.2) to determine the sieve bound.

**Centered stripe layout**: Stripe offsets are placed symmetrically around b=0, exploiting conjugate symmetry of Gaussian primes. A Gaussian integer `a + bi` is prime iff `a - bi` is prime. This means stripes at `+b_lo` and `-b_lo` have identical `io_count` values, so we only need to compute one and mirror. (Optimization: halve M for symmetric pairs. Not required for correctness -- both produce correct results independently.)

### 5.2 Day 0: Nested par_iter

```rust
// Outer: parallel over shells (N work items)
// Inner: parallel over stripes (M work items per shell)
// Rayon work-stealing balances across both levels.

// Note: sieve construction must follow stripe bounds computation (A-3).
// The kernel holds its sieve internally — callers pass only tile bounds + k_sq.

let shell_records: Vec<ShellRecord> = shells.par_iter().enumerate()
    .map(|(shell_idx, &(a_lo, a_hi, r_center))| {
        let started = Instant::now();

        let tile_results: Vec<TileResult> = stripe_offsets.par_iter()
            .map(|&b_lo| {
                let b_hi = b_lo + tile_width as i64;
                kernel.run_tile(a_lo, a_hi, b_lo, b_hi, k_sq)
            })
            .collect();

        let io_counts: Vec<usize> = tile_results.iter().map(|r| r.io_count).collect();
        let connected = io_counts.iter().filter(|&&c| c > 0).count();
        let f_r = connected as f64 / io_counts.len() as f64;

        ShellRecord {
            shell_idx,
            r_center,
            a_lo, a_hi,
            io_counts,
            f_r,
            is_candidate: f_r == 0.0,
            num_primes: tile_results.iter().map(|r| r.num_primes).sum(),
            shell_time_ms: started.elapsed().as_millis() as u64,
        }
    })
    .collect();
```

At N=8 shells, M=32 stripes: 256 independent work items. Saturates any CPU. Rayon's indexed par_iter preserves order in the output.

### 5.3 Optimized CPU: Flat Pool

```rust
// Pre-build RowSieveTable (shared, read-only, ~2KB)
let table = RowSieveTable::new(1000);

// Enumerate all N*M work items, let Rayon load-balance globally
let work: Vec<(usize, usize, i64, i64, i64, i64)> = shells.iter().enumerate()
    .flat_map(|(s, &(a_lo, a_hi, _))| {
        stripe_offsets.iter().enumerate().map(move |(t, &b_lo)| {
            (s, t, a_lo, a_hi, b_lo, b_lo + tile_width as i64)
        })
    })
    .collect();

let results: Vec<(usize, usize, TileResult)> = work.par_iter()
    .map(|&(s, t, a_lo, a_hi, b_lo, b_hi)| {
        let result = kernel.run_tile(a_lo, a_hi, b_lo, b_hi, k_sq);
        (s, t, result)
    })
    .collect();

// Group by shell_idx, aggregate into ShellRecords
// Group by stripe_id, construct StripeRecords (see Section 5.4)
```

This gives Rayon full visibility into ALL work items for optimal load balancing. Tiles near axes (more primes, slower) are naturally interleaved with sparse tiles. Eliminates shell-level stragglers.

**Why RowSieveTable enables this**: Unlike old `PrimeSieve` (up to 10MB, shell-specific sieve limit), `RowSieveTable` is ~2KB and universal. Same table works for any R. No per-shell sieve allocation. Build once, share everywhere.

### 5.4 StripeRecord Construction

The orchestrator must construct `StripeRecord`s from tile results. This is an explicit post-processing step after the shell loop.

```rust
// Maintain per-stripe accumulators during the shell loop.
let mut stripe_accumulators: Vec<Vec<TileRecord>> = vec![Vec::new(); num_stripes];

// After each shell's tiles are computed, push each tile result
// into the appropriate stripe's accumulator:
for (stripe_idx, tile_result) in shell_tile_results.iter().enumerate() {
    let record = TileRecord {
        a_lo: shell_a_lo,
        b_lo: stripe_offsets[stripe_idx],
        width: tile_width,
        height: tile_height,
        result: tile_result.clone(),
        connects_below: None,  // ISE mode: always None (see Section 2.2)
        connects_left: None,
        connects_right: None,
        time_ms: tile_time_ms,
    };
    stripe_accumulators[stripe_idx].push(record);
}

// After the shell loop completes, construct StripeRecords:
let stripes: Vec<StripeRecord> = stripe_accumulators.into_iter().enumerate()
    .map(|(id, tiles)| StripeRecord {
        stripe_id: id,
        b_lo: stripe_offsets[id],
        tiles,  // ordered by a_lo (radially outward) from shell iteration order
    })
    .collect();
```

This ensures the JSON trace includes the full stripe-oriented view alongside the shell-aggregated view.

### 5.5 Memory at Runtime

Per-tile allocation with row-wise sieve:

| Allocation | Size | Lifetime | Notes |
|-----------|------|----------|-------|
| `RowSieveTable` | ~2KB | Process | Built once, shared read-only |
| Per-row sieve array | 2KB (W=2000) | Row | Stack-allocated, L1-resident |
| Per-tile primes vec | ~2.3MB at R=10^6 | Tile | ~144K primes * 16 bytes |
| UF parent array | ~1.2MB at R=10^6 | Tile | `[u32; n]` where n ~ 144K primes + collar neighbors |
| Spatial hash | ~1.0MB at R=10^6 | Tile | FxHashMap cell lists for neighbor lookup |
| TileResult | ~200 bytes | Returned | Small, heap-allocated vecs for face components |

**Per-tile peak**: ~2.3 + 1.2 + 1.0 = **~4.5MB**.

**Peak RSS at 16 threads**: 16 * 4.5MB (tile working set) + 2KB (table) ~ **72MB**.

---

## 6. CLI Specification

### 6.1 ISE Binary Arguments

```rust
#[derive(Parser)]
#[command(name = "ise", about = "Independent Strip Ensemble -- Gaussian moat candidate detector")]
struct Args {
    /// Squared step bound k^2 (threshold for adjacency in the Gaussian prime graph).
    #[arg(long, required = true)]
    k_squared: u64,

    /// Minimum radius to scan.
    #[arg(long, default_value = "0")]
    r_min: f64,

    /// Maximum radius to scan.
    #[arg(long, required = true)]
    r_max: f64,

    // --- Tile size: three ways to specify ---

    /// Tile width W (lateral extent). Overrides --tile-size and --preset.
    #[arg(long)]
    tile_width: Option<u32>,

    /// Tile height H (radial extent). Overrides --tile-size and --preset.
    #[arg(long)]
    tile_height: Option<u32>,

    /// Square tile shorthand: W = H = S. Overridden by explicit --tile-width/--tile-height.
    #[arg(long)]
    tile_size: Option<u32>,

    /// Tile size preset. Overridden by --tile-size, --tile-width, --tile-height.
    /// Values: "screen" (200x200), "balanced" (500x500), "deep" (2000x2000).
    #[arg(long, default_value = "deep")]
    preset: String,

    /// Number of independent stripes M.
    /// More stripes = better statistical coverage but linear cost.
    /// 8=fast screening, 32=production, 128=high-confidence.
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

    /// Print timing profile at end.
    #[arg(long)]
    profile: bool,

    /// Fallback tile heights for ISE retry. If no moat candidate is detected
    /// at the primary tile height, retry with each height in order.
    /// E.g., --fallback-heights 200,500,2000
    #[arg(long, value_delimiter = ',')]
    fallback_heights: Option<Vec<u32>>,

    /// Run validation against known moats.
    #[arg(long)]
    validate: bool,
}
```

### 6.2 Tile Size Resolution

Resolution priority (highest wins):

1. `--tile-width W --tile-height H` (explicit, non-square OK)
2. `--tile-width W` alone or `--tile-height H` alone: the missing dimension is filled from the active preset (NOT assumed square). E.g., `--tile-width 4000 --preset screen` gives `(4000, 200)`.
3. `--tile-size S` (square shorthand: W=H=S)
4. `--preset NAME`:
   - `"screen"`: W=200, H=200
   - `"balanced"`: W=500, H=500
   - `"deep"`: W=2000, H=2000 (default)

```rust
fn resolve_tile_size(args: &Args) -> (u32, u32) {
    let preset = match args.preset.as_str() {
        "screen" => (200, 200),
        "balanced" => (500, 500),
        _ => (2000, 2000),  // "deep" is default
    };

    if let (Some(w), Some(h)) = (args.tile_width, args.tile_height) {
        return (w, h);
    }
    if let Some(s) = args.tile_size {
        return (s, s);
    }
    // Partial specification: fill missing dimension from preset.
    let w = args.tile_width.unwrap_or(preset.0);
    let h = args.tile_height.unwrap_or(preset.1);
    (w, h)
}
```

### 6.3 Example Invocations

```bash
# Quick screening with small tiles
ise --k-squared 26 --r-min 1010000 --r-max 1025000 --preset screen --stripes 8 --trace

# Production run with deep tiles
ise --k-squared 26 --r-min 1010000 --r-max 1025000 --preset deep --stripes 32 \
    --json-trace results/k26-ise.json --csv results/k26-ise.csv --profile

# Non-square wide tiles for lateral coverage experiments
ise --k-squared 26 --r-min 1010000 --r-max 1025000 --tile-width 4000 --tile-height 500 \
    --stripes 16 --trace

# Validation
ise --validate
```

---

## 7. Output Format

### 7.1 JSON Trace

```json
{
  "config": {
    "k_sq": 26,
    "r_min": 1010000.0,
    "r_max": 1025000.0,
    "tile_width": 2000,
    "tile_height": 2000,
    "num_stripes": 32
  },
  "shells": [
    {
      "shell_idx": 0,
      "r_center": 1011000.0,
      "a_lo": 1010000,
      "a_hi": 1012000,
      "io_counts": [3, 2, 4, 0, 5, 3, 2, 1, "...M values..."],
      "f_r": 0.96875,
      "is_candidate": false,
      "num_primes": 48000000,
      "shell_time_ms": 1234
    }
  ],
  "stripes": [
    {
      "stripe_id": 0,
      "b_lo": -32000,
      "tiles": [
        {
          "a_lo": 1010000, "b_lo": -32000, "width": 2000, "height": 2000,
          "io_count": 3, "il_count": 2, "ir_count": 1,
          "ol_count": 2, "or_count": 1, "lr_count": 4,
          "num_primes": 144000,
          "connects_below": null,
          "time_ms": 62
        }
      ]
    }
  ],
  "summary": {
    "total_shells": 7,
    "total_tiles": 224,
    "candidates": [],
    "total_time_ms": 8654,
    "peak_rss_mb": 72.4
  }
}
```

The `shells` array provides the ISE view (aggregated per shell). The `stripes` array provides the stripe-oriented storage with per-tile detail including variable tile dimensions.

### 7.2 CSV Summary

One row per shell. Lightweight, easy to plot.

```csv
shell_idx,r_center,a_lo,a_hi,f_r,is_candidate,num_primes,shell_time_ms
0,1011000.0,1010000,1012000,0.96875,false,48000000,1234
1,1013000.0,1012000,1014000,0.9375,false,47500000,1198
```

### 7.3 Output Module API

```rust
pub fn write_json_trace(
    path: &str,
    config: &IseConfig,
    shells: &[ShellRecord],
    stripes: &[StripeRecord],
    summary: &IseSummary,
) -> std::io::Result<()>;

pub fn write_csv_summary(path: &str, shells: &[ShellRecord]) -> std::io::Result<()>;

pub fn print_summary(config: &IseConfig, shells: &[ShellRecord], summary: &IseSummary);
```

---

## 8. Verification Gates

Each gate describes a specific observable behavior that proves a module works. Not "tests pass" -- the specific thing that must be true.

### Gate 1: Kernel Correctness (Unit)

**Test A**: `kernel.run_tile(0, 2, 0, 2, k_sq=2)`. Assert `result.io_count > 0`. The origin's neighborhood connects I-face to O-face at k^2=2 in this 3x3 tile. (Kernel constructed with appropriate sieve for the tile's norm range.)

**Test B**: `kernel.run_tile(100, 102, 0, 2, k_sq=2)`. Assert `result.io_count == 0`. At R=100, k^2=2 produces a moat.

**Test C**: `kernel.run_tile(0, 20, 0, 20, k_sq=2)`. Compare `result` against direct `io_crossing_count(build_tile_with_sieve(...))`. Must match exactly. This confirms the kernel wrapper agrees with the direct tile build.

**Test D**: Rich TileResult fields. Run kernel on `(0, 5, 0, 5, k_sq=4)`. Verify `il_count + ir_count + ol_count + or_count + lr_count >= 0` (no panics, all fields populated). Verify `i_face_components.len() >= io_count` (I-face component list is at least as long as the I->O crossing count).

### Gate 2: Sieve Correctness (Cross-Check)

**Test**: Run both legacy path (`gaussian_primes_in_rect_with_sieve`) and new path (`gaussian_primes_in_rect_rowsieve`) on identical rectangles at R=100, R=10000, R=100000. Assert identical prime sets (as sorted `Vec<(i64,i64)>`). Zero tolerance.

**Test**: Run on a rectangle containing axis primes (a=0 or b=0). Verify axis primes are correctly identified by both paths.

### Gate 3: Sieve Sharing (Concurrency)

**Test**: Build one `CpuKernel` (backed by a shared `PrimeSieve`). Spawn two Rayon tasks that call `kernel.run_tile()` with different `(b_lo, b_hi)` ranges on the same kernel instance. Assert both return correct results. This confirms no data races on the kernel's internal sieve reference.

### Gate 4: ISE Moat Detection (Known Moats)

**Test A**: `k_sq=2, r_min=0, r_max=30, W=4, H=4, M=8`. Known moat at R~8-10. Assert: at least one shell has `f_r == 0.0`. Assert: shells near R=0 have `f_r > 0`.

**Test B**: `k_sq=6, r_min=0, r_max=60, W=8, H=8, M=8`. Known moat at R~30-40. Assert: candidate detected.

### Gate 5: ISE No False Negatives (Connected Region)

**Test**: `k_sq=26, r_min=0, r_max=1000, W=200, H=200, M=8`. At these radii, k^2=26 is deeply supercritical. Assert: `f_r > 0` for ALL shells. No spurious candidates.

### Gate 6: JSON/CSV Round-Trip

**Test**: Run ISE. Write JSON trace to tmpfile. Parse JSON back. Assert: all fields present, shell count matches expected, `io_counts` arrays have length M, `f_r` values in `[0.0, 1.0]`, tile dimensions in stripe records match config.

### Gate 7: LB Binary Regression

**Test**: After workspace migration, run `lb-probe --validate`. The existing validation suite (`k_sq=2 r_max=20`, `k_sq=4 r_max=40`, `k_sq=6 r_max=50`) must all pass. No regressions from restructuring.

### Gate 8: Tsuchimura Validation (Integration, Slow)

**Test**: Start with `k_sq=26, r_min=1010000, r_max=1025000, W=2000, H=200, M=32`. At least one shell near R~1,016,000 must have `f_r == 0.0` or significantly depressed `f_r < 0.5`. This validates ISE against Tsuchimura's known moat.

**Why H=200 first**: The ISE detection rule `f(r) = 0` requires tile height H to be comparable to or smaller than the moat's radial extent. For k^2=26 with a gap of ~5 lattice units, H=200 gives fine enough radial resolution to isolate the gap. Starting at H=2000 risks averaging the gap within a single tile, making `io_count` always positive and masking the moat entirely.

**Height sweep**: If H=200 detects the moat, retry with H=500 and H=2000 to characterize sensitivity. Use `--fallback-heights 200,500,2000` for automated retry (see Section 6.1).

Run as `--validate-tsuchimura` or a separate integration test binary. Not in the default `cargo test` suite (takes minutes).

**Fallback**: If f(r) does not drop at any height including H=200, there is a bug in the sieve, kernel, or stripe layout.

---

## 9. Phase Plan

### Phase 1: Workspace Restructuring

**Deliverable**: Working workspace with `moat-kernel` lib and `lb-probe` binary. All existing tests pass. Zero new functionality.

**Steps**:
1. Create workspace `Cargo.toml` at `tile-probe/Cargo.toml`.
2. Create `crates/moat-kernel/` directory structure.
3. Move `primes.rs`, `tile.rs`, `compose.rs`, `profile.rs` into `crates/moat-kernel/src/`.
4. Write `crates/moat-kernel/src/lib.rs` (re-export modules).
5. Create `crates/lb-probe/` with `main.rs` and `probe.rs` migrated from `src/`.
6. Update all `crate::` import paths to `moat_kernel::` in lb-probe.
7. Drop unused `solver` dependency (never imported in any source file).
8. Make `SimpleUF` pub (needed by compose.rs which is now in the same crate).
9. Make `build_tile_from_primes` `pub(crate)` in `tile.rs` so the kernel module can call it.
10. `cargo test` -- all existing tests pass.
11. `cargo run --bin lb-probe -- --validate` -- all validations pass.

**Gate**: Gate 7.

### Phase 2: TileKernel Trait + Rich TileResult

**Deliverable**: `moat-kernel::kernel` module with `TileKernel` trait, `CpuKernel`, and rich `TileResult`. Unit tests proving kernel correctness.

**Steps**:
1. Add `TileResult` struct to `moat-kernel/src/kernel.rs` with all 6 cross-face counts and 4 face component ID lists.
2. Add `TileResult::from_tile_operator(&TileOperator) -> TileResult`.
3. Add `TileKernel` trait with `fn run_tile(...) -> TileResult`.
4. Implement `CpuKernel` with internal `&PrimeSieve`, calling `build_tile_with_sieve` + `TileResult::from_tile_operator`. Trait signature: `run_tile(&self, a_lo, a_hi, b_lo, b_hi, k_sq) -> TileResult` (no sieve parameter).
5. Move `io_crossing_count` from `probe.rs` to `moat-kernel::tile` as a pub function.
6. Write unit tests for Gate 1 (all four sub-tests).
7. Write concurrency test for Gate 3.

**Gate**: Gate 1, Gate 3.

### Phase 3: Day-0 Sieve Optimization

**Deliverable**: `is_prime_fast` with trial division + reduced MR witnesses. Measurable speedup over current 12-witness MR.

**Steps**:
1. Add `SMALL_PRIMES` array (46 primes up to 199) to `primes.rs`.
2. Add `is_prime_fast(n: u64) -> bool` with trial division + 4/5-witness MR.
3. Add `gaussian_primes_in_rect_fast(a_min, a_max, b_min, b_max) -> Vec<(i64,i64)>` using `is_prime_fast` instead of `PrimeSieve::is_prime`.
4. Cross-check against legacy path (Gate 2).
5. Benchmark: single tile at R=10^6, W=H=2000. Must be >= 3x faster than 12-witness path.

**Gate**: Gate 2 (sieve correctness cross-check), performance benchmark.

Note: `gaussian_primes_in_rect_fast` is a stepping stone toward `CpuKernelRowSieve` in Phase 4. Day-0 speedup becomes available once the RowSieve kernel is wired into the ISE orchestrator.

### Phase 4: ISE Orchestrator

**Deliverable**: `ise/src/orchestrator.rs` with `run_ise()`. Core ISE algorithm working. Driven by tests, no CLI yet.

**Steps**:
1. Create `crates/ise/` directory and `Cargo.toml`.
2. Write `orchestrator.rs` with `IseConfig`, `ShellRecord`, `StripeRecord`, `IseResult`.
3. Add input validation at orchestrator entry: `assert!(config.num_stripes >= 1, "stripes must be >= 1")`. Guard against division by zero in `f_r` computation.
4. Implement shell bounds computation (reuse logic from probe.rs).
5. Implement stripe offset computation. (Must complete before kernel/sieve construction -- see Section 5.1.)
6. Implement the shell loop: parallel over shells, parallel over stripes within each shell.
7. Implement StripeRecord construction from per-shell tile results (see Section 5.4).
8. Write integration tests for Gate 4 (known moat detection) and Gate 5 (no false negatives).

**Gate**: Gate 4, Gate 5.

### Phase 5: CLI + Output

**Deliverable**: Working `ise` binary with CLI, JSON trace, CSV summary, validation mode.

**Steps**:
1. Write `ise/src/main.rs` with clap argument parsing, tile size resolution.
2. Write `ise/src/output.rs` with JSON and CSV writers.
3. Wire CLI -> orchestrator -> output.
4. Write tests for Gate 6 (JSON/CSV round-trip).
5. Add `--validate` mode with small-k cases.
6. Run performance baseline (Gate 8 as a manual check).

**Gate**: Gate 6, performance baseline recorded.

### Phase 6: Tsuchimura Validation + Optimization

**Deliverable**: Empirical validation against Tsuchimura k^2=26 moat. Optional CPU optimizations.

**Steps**:
1. Run: `ise --k-squared 26 --r-min 1010000 --r-max 1025000 --tile-width 2000 --tile-height 200 --stripes 32 --fallback-heights 200,500,2000 --json-trace research/k26-ise-validation.json --trace --profile`
2. Analyze JSON: check f(r) profile near R~1,016,000.
3. If validation passes (Gate 8), record results.
4. Optional optimizations based on profile data:
   - Row-wise sieve (`RowSieveTable` + `gaussian_primes_in_rect_rowsieve`).
   - Flat pool parallelism (Section 5.3).
   - Thread-local prime vec reuse.
   - (Future: `build_tile_io_only` -- skip L/R face port collection for ISE-only tiles. Out of scope for this plan; requires kernel-level branching. Defer to a profiling-driven optimization pass.)
5. Re-benchmark after optimizations.

**Gate**: Gate 8.

---

## 10. CUDA Compatibility

### 10.1 Kernel Mapping

The `TileKernel` trait is the CPU/GPU boundary. Everything below the trait is replaceable:

| CPU path | GPU path |
|----------|----------|
| `RowSieveTable` (host, shared) | Device-side sieve in shared memory |
| `primes_in_row` (per Rayon thread) | Per-warp row sieve in shared memory |
| `SimpleUF` (per tile) | Parallel label propagation on device |
| `FxHashMap` spatial hash | Device-side cell list in global memory |
| `component_faces` scan | Parallel reduction per component |

### 10.2 Batch Kernel Trait

For GPU, M individual kernel launches are inefficient. Add a batch variant:

```rust
pub trait TileKernelBatch: Send + Sync {
    fn run_tiles_batch(
        &self,
        tiles: &[(i64, i64, i64, i64)],  // (a_lo, a_hi, b_lo, b_hi) per tile
        k_sq: u64,
    ) -> Vec<TileResult>;
}

/// CPU implementation: just par_iter over run_tile.
/// The kernel holds its sieve internally — no sieve parameter needed here.
impl<K: TileKernel> TileKernelBatch for K {
    fn run_tiles_batch(&self, tiles: &[(i64, i64, i64, i64)], k_sq: u64) -> Vec<TileResult> {
        tiles.par_iter()
            .map(|&(a_lo, a_hi, b_lo, b_hi)| self.run_tile(a_lo, a_hi, b_lo, b_hi, k_sq))
            .collect()
    }
}

/// Helper: compute the sieve limit needed for a batch of tiles.
/// Returns the maximum norm across all tiles in the batch.
fn sieve_limit_for_batch(tiles: &[(i64, i64, i64, i64)], k_sq: u64) -> u64 {
    let collar = (k_sq as f64).sqrt().ceil() as i64;
    tiles.iter().map(|&(a_lo, a_hi, b_lo, b_hi)| {
        let a_max = a_hi.abs().max(a_lo.abs()) + collar;
        let b_max = b_hi.abs().max(b_lo.abs()) + collar;
        (a_max as u128 * a_max as u128 + b_max as u128 * b_max as u128) as u64
    }).max().unwrap_or(0)
}

/// CUDA implementation: single kernel launch with M*B thread blocks.
pub struct CudaKernel { /* device handle, pre-allocated buffers */ }
impl TileKernelBatch for CudaKernel {
    fn run_tiles_batch(&self, tiles: &[(i64, i64, i64, i64)], k_sq: u64) -> Vec<TileResult> {
        // 1. Upload tile bounds to device
        // 2. Launch kernel: one thread block per tile
        // 3. Copy TileResult array back to host
    }
}
```

The orchestrator calls `run_tiles_batch` per shell (M tiles) or across multiple shells (M*B tiles). The batch trait is Phase 2+ -- Phase 1 uses single-tile trait with Rayon.

### 10.3 Device Memory Layout

For a tile of size WxH:
- **Prime coordinates**: `[(i64, i64); n]` where n ~ WH * rho(R). At R=10^6, W=H=2000: ~144K primes = 2.3MB.
- **Cell lists**: Grid of `ceil(W/collar) * ceil(H/collar)` cells, each a variable-length list. Use flattened CSR format.
- **Union-find parent array**: `[u32; n]` = 576KB.
- **Face membership bitmask**: `[u8; n]` = 144KB.

Total per-tile device memory: ~3.5MB. An A100 with 80GB can hold ~22K tiles simultaneously (far more than needed for M=32*B=32 = 1024 concurrent tiles).

---

## 11. Risk Register

| # | Risk | Impact | Likelihood | Mitigation |
|---|------|--------|------------|------------|
| 1 | Per-tile I->O does not drop to 0 at Tsuchimura moat | ISE doesn't detect the known moat | **Moderate** (ISE detection rule `f(r) = 0` requires tile height H to be comparable to or smaller than the moat's radial extent; for k²=26 with a ~5 lattice unit gap, H=200 or smaller is likely needed) | Start validation with H=200, sweep upward (H=200, 500, 2000). Use `--fallback-heights` CLI flag (see Section 6.1) to automate retry. The moat gap is ~sqrt(26)~5.1 lattice units. If tiles are too tall, the density drop is averaged out within a single tile, and `io_count` never reaches 0. |
| 2 | Row-wise sieve correctness | Silent wrong results (missed or spurious primes) | Medium (new code) | Gate 2 cross-checks against legacy MR path on identical rectangles at multiple radii. Zero-tolerance comparison. Run at R=100, R=10K, R=100K, R=1M. |
| 3 | Trial division + reduced-witness MR incorrect for some norm | Misclassified prime/composite | Very low (proven correct for n < 3.2*10^9 / 2.15*10^12 / 3.41*10^14 with 4/5/7 witnesses respectively) | Runtime `assert!` on norm range (NOT debug_assert). Automatic fallback from 5 to 7 witnesses when norm exceeds `MAX_NORM_5_WITNESS`. Unit tests with known Gaussian primes and composites at large norms. Cross-check with 12-witness path. |
| 4 | Workspace restructuring breaks builds | Lost development time | Low | Phase 1 is zero-new-functionality. `cargo test` + `cargo run --bin lb-probe -- --validate` must pass before proceeding. |
| 5 | Variable tile sizes cause edge cases in face classification | Wrong io_count for non-square tiles | Medium | Test with W!=H (e.g., W=100, H=500 and W=500, H=100). Verify face classification against known results. The current code handles arbitrary (a_min, a_max, b_min, b_max) already. |
| 6 | Memory pressure at high M on constrained machines | OOM or swap thrashing | Low (~72MB peak at M=32, 16 threads -- includes UF parent array and spatial hash per tile) | Monitor RSS via profile output. Cap concurrent tiles if needed. |
| 7 | Stripe placement misses all connectivity paths (false positive) | Spurious moat candidate | Very low for M>=8 (p^M < 10^-4) | Increase M. Validate any ISE candidate with full LB sweep before claiming a moat. ISE is a candidate detector, not a proof. |
| 8 | `solver` crate removal causes unexpected breakage | Build failure | Very low (solver is never imported) | Verify with `rg 'use solver' tile-probe/src/` and `rg 'solver::' tile-probe/src/` before removing. |
