---
date: 2026-03-22
engine: coordinator
status: complete
type: implementation-plan
---

# Scanline Tile Kernel — Implementation Plan

## What We're Building

Replace the current prime-sieve-based tile kernel with a **scanline kernel**: single-pass bottom-to-top sweep that discovers primes and builds connected components simultaneously. Optimized for large-R tiles (norms ~10¹⁵) where the current sieve caps out and falls back to expensive per-point Miller-Rabin.

**Target:** k²=36, tiles at R≈80M, tile sizes 2000² and 4000².

## Why

Current kernel uses a precomputed sieve capped at 10M norms. At R=80M, norms are ~6.4×10¹⁵ — every point falls through to unoptimized Miller-Rabin. A tile that should take <2s takes minutes. The scanline approach: parity pre-filter → trial division with early exit (kills 92% of composites) → 9-witness deterministic Miller-Rabin on survivors only.

## Architecture

### New Files

```
moat-kernel/src/primality.rs    — trial division + deterministic Miller-Rabin
moat-kernel/src/scanline.rs     — scanline tile kernel
```

### Modified Files

```
moat-kernel/src/lib.rs          — add mod primality; mod scanline;
moat-kernel/src/kernel.rs       — add ScanlineKernel to CpuKernel enum / TileKernel dispatch
ise/src/main.rs                 — add --kernel scanline flag (default: scanline)
```

### Unchanged

```
ise/src/orchestrator.rs         — no changes. Calls TileKernel::run_tile() as before.
ise/src/output.rs               — no changes. TileResult/TileRecord/ShellRecord contracts preserved.
moat-kernel/src/tile.rs         — kept as legacy kernel. Not deleted.
```

## Interface Contract

The scanline kernel MUST return the same `TileResult` as the current kernel:

```rust
TileResult {
    io_count: usize,        // ISE metric: components spanning I→O
    il_count, ir_count, ol_count, or_count, lr_count: usize,
    i_face_components, o_face_components, l_face_components, r_face_components: Vec<usize>,
    num_primes: usize,
}
```

**No prime storage by default.** `TileDetail` (primes, edges, face_assignments, component_ids) is only populated when `export_primes=true`. The scanline kernel tracks only:
- Prime bitmap (1 bit per point)
- Union-find parent array (u32 per point)
- Face port lists (only primes on faces)

## Primality Module (`primality.rs`)

### Constants

```rust
const SMALL_PRIMES: [u64; 168] = [2, 3, 5, 7, ..., 997];
const MR_WITNESSES_9: [u64; 9] = [2, 3, 5, 7, 11, 13, 17, 19, 23];
// Valid for all n < 3.825×10¹⁸ (covers R up to ~1.38 billion)
```

### Functions (each maps 1:1 to a future CUDA `__device__` function)

```rust
/// Modular multiplication via u128 intermediate. CUDA: emulate with __umul64hi.
#[inline(always)]
pub fn mulmod(a: u64, b: u64, m: u64) -> u64

/// Modular exponentiation by squaring.
#[inline(always)]
pub fn powmod(base: u64, exp: u64, m: u64) -> u64

/// Trial division by primes ≤ 997. Returns true if n proved composite.
/// Early exit on first factor found. Average ~3-5 divisions for composites.
#[inline]
pub fn has_small_factor(n: u64) -> bool

/// Miller-Rabin with 9 deterministic witnesses.
/// PRE: n is odd, n > 1, n has no factor ≤ 997.
pub fn miller_rabin_9(n: u64) -> bool  // returns true if prime

/// Combined primality test. The public API.
/// Parity filter → small factor test → Miller-Rabin.
pub fn is_gaussian_prime(a: i64, b: i64) -> bool {
    // Special cases: axis primes (a=0 or b=0)
    // General case: check if a² + b² is a rational prime
    let norm = (a as u128) * (a as u128) + (b as u128) * (b as u128);
    // norm fits in u64 for R < ~4 billion — assert this
    let n = norm as u64;
    if n < 2 { return false; }
    if n == 2 { return true; }
    if n & 1 == 0 { return false; }  // even → composite (free parity filter)
    if has_small_factor(n) { return false; }
    miller_rabin_9(n)
}
```

### Parity Pre-Filter (Free)

When both a and b have the same parity (both even or both odd), a²+b² is even and >2 at large R → automatically composite. This eliminates ~50% of points before any arithmetic. Encoded inside `is_gaussian_prime` via the `n & 1 == 0` check — the norm being even IS the parity filter.

## Scanline Kernel (`scanline.rs`)

### Memory Layout

For tile with nominal bounds (a_lo, a_hi, b_lo, b_hi), collar c = ceil(sqrt(k_sq)):

```
Expanded bounds: (a_lo - c, a_hi + c, b_lo - c, b_hi + c)
Grid dimensions: W_exp = (a_hi - a_lo) + 2c, H_exp = (b_hi - b_lo) + 2c
```

Wait — we fixed tiles as SQUARE. So tile_width = tile_height = S.

```
Expanded side: S_exp = S + 2c
Total points: S_exp²
```

Allocations per tile:
- `prime_bitmap: BitVec` — S_exp² bits. 2012² = 506KB. 4012² = 2MB.
- `parent: Vec<u32>` — S_exp² entries. 2012² × 4 = 16MB. 4012² × 4 = 64MB.
- `rank: Vec<u8>` — S_exp² entries. 2012² = 4MB. 4012² = 16MB.

Total: ~20MB for 2000², ~82MB for 4000². Acceptable on CPU.

### The Scanline

```rust
pub fn run_scanline_tile(
    a_lo: i64, b_lo: i64,
    side: u32, k_sq: u64,
    export_detail: bool,
) -> TileResult {
    let c = (k_sq as f64).sqrt().ceil() as i64;
    let s = side as i64;
    let ea_lo = a_lo - c;  // expanded bounds
    let ea_hi = a_lo + s + c;
    let eb_lo = b_lo - c;
    let eb_hi = b_lo + s + c;
    let w = (eb_hi - eb_lo) as usize;
    let h = (ea_hi - ea_lo) as usize;

    let mut bitmap = BitVec::from_elem(w * h, false);
    let mut parent: Vec<u32> = (0..w * h).map(|i| i as u32).collect();
    let mut rank: Vec<u8> = vec![0; w * h];
    let mut num_primes: usize = 0;

    // BACKWARD_OFFSETS: 56 offsets (da, db) with da²+db² ≤ k_sq
    // and (da < 0) or (da == 0 and db < 0)
    let offsets = precompute_backward_offsets(k_sq);

    // === PASS 1: Primality scan ===
    // Bottom to top (a ascending), left to right (b ascending)
    for row in 0..h {
        let a = ea_lo + row as i64;
        for col in 0..w {
            let b = eb_lo + col as i64;
            if is_gaussian_prime(a, b) {
                let idx = row * w + col;
                bitmap.set(idx, true);
                num_primes += 1;
            }
        }
    }

    // === PASS 2: Union-Find scan ===
    // Same order. For each prime, check backward neighbors.
    for row in 0..h {
        for col in 0..w {
            let idx = row * w + col;
            if !bitmap[idx] { continue; }
            for &(da, db) in &offsets {
                let nr = row as i64 + da;
                let nc = col as i64 + db;
                if nr < 0 || nr >= h as i64 || nc < 0 || nc >= w as i64 { continue; }
                let nidx = nr as usize * w + nc as usize;
                if bitmap[nidx] {
                    union(&mut parent, &mut rank, idx, nidx);
                }
            }
        }
    }

    // === Build face ports and count crossings ===
    // Face membership: distance from nominal edge <= collar (inclusive)
    build_tile_result(
        &bitmap, &parent, &rank,
        a_lo, b_lo, s, c, w, h, num_primes,
        export_detail,
    )
}
```

Note: Two separate passes (primality scan then UF scan) — NOT combined. This is:
1. Simpler to reason about (no race conditions even conceptually)
2. Better cache behavior (pass 1 writes bitmap sequentially; pass 2 reads it)
3. Directly maps to the CUDA two-kernel design

### Face Classification

A prime at absolute position (a, b) belongs to:
- **I-face** if `a - a_lo <= c` (within collar of inner nominal edge)
- **O-face** if `(a_lo + s) - a <= c` (wait: `a_lo + s - 1` is the last nominal row, so `(a_lo + s - 1 + c) - a <= c` — need to match existing `tile.rs:267` exactly)
- **L-face** if `b - b_lo <= c`
- **R-face** if `(b_lo + s - 1 + c) - b <= c`

IMPORTANT: Must match `tile.rs` face membership EXACTLY — use `<=` not `<`.

Only primes within the **nominal** tile bounds (not collar) get face assignments — collar primes participate in UF but are not face ports. (Verify this against existing `tile.rs`.)

### Union-Find (Path-Splitting, GPU-Portable)

```rust
fn find(parent: &mut [u32], mut x: usize) -> usize {
    while parent[x] as usize != x {
        let next = parent[x] as usize;
        parent[x] = parent[next];  // path splitting
        x = next;
    }
    x
}

fn union(parent: &mut [u32], rank: &mut [u8], a: usize, b: usize) {
    let ra = find(parent, a);
    let rb = find(parent, b);
    if ra == rb { return; }
    if rank[ra] < rank[rb] { parent[ra] = rb as u32; }
    else if rank[ra] > rank[rb] { parent[rb] = ra as u32; }
    else { parent[rb] = ra as u32; rank[ra] += 1; }
}
```

## Integration with ISE Orchestrator

The `run_ise()` function in `orchestrator.rs` calls tiles via the `TileKernel` trait. The scanline kernel implements this trait:

```rust
impl TileKernel for ScanlineKernel {
    fn run_tile(&self, a_lo: i64, a_hi: i64, b_lo: i64, b_hi: i64, k_sq: u64) -> TileResult {
        let side = (a_hi - a_lo) as u32;  // tiles are square
        run_scanline_tile(a_lo, b_lo, side, k_sq, self.export_detail)
    }
}
```

No changes to `orchestrator.rs`, `output.rs`, or `IseConfig`.

## Verification Gates

### Gate 1: Unit Tests (must pass before anything else)
- All 29 existing tests pass with scanline kernel
- New tests for primality module:
  - Known Gaussian primes at large norms (spot checks near 10¹⁵)
  - Known composites that survive trial division (semiprimes with factors > 997)
  - Edge cases: n=2, n=3, axis primes

### Gate 2: Cross-Validation (scanline vs legacy kernel)
- Run k²=2 with both kernels. Exact same TileResult for every tile.
- Run k²=26 at R=1000 with both kernels. Exact same io_count per shell.
- If any discrepancy: scanline kernel is wrong until proven otherwise.

### Gate 3: Performance Benchmark
- Measure per-tile time at k²=36, R=80M, tile_size=2000
- Measure per-tile time at k²=36, R=80M, tile_size=4000
- Target: <2s/tile for 2000², <10s/tile for 4000²
- Log: trial_division_time, miller_rabin_time, union_find_time, total_time

### Gate 4: ISE Moat Detection
- Full ISE run: k²=26, r_min=900000, r_max=1100000, 32 stripes, tile_size=2000
- Expect: f(r) drops in known moat region (~950K–1.05M)
- Full ISE run: k²=2, r_min=0, r_max=100
- Expect: moat detected (f(r)=0 at known distance)

### Gate 5: Memory
- Peak RSS during 4000² tile at R=80M must be < 200MB
- No prime Vec allocation unless export_primes=true

## Performance Projections (from audits)

| Tile Size | Points | Survivors (8.1%) | MR calls | Est. CPU time |
|-----------|--------|-------------------|----------|---------------|
| 2000²     | 4M     | 324K              | 324K     | ~0.9s         |
| 4000²     | 16M    | 1.3M              | 1.3M     | ~3.6s         |

## GSD Dispatch Plan

Single GSD coordinator run. Three phases:

### Phase 1: Primality Module (Worker 1 — Codex or Claude)
- Create `moat-kernel/src/primality.rs`
- Implement: `mulmod`, `powmod`, `has_small_factor`, `miller_rabin_9`, `is_gaussian_prime`
- Write unit tests in `moat-kernel/src/primality.rs` (inline #[cfg(test)])
- Test: `cargo test -p moat-kernel primality`
- Gate: all tests pass, clippy clean

### Phase 2: Scanline Kernel (Worker 2 — Codex or Claude, after Phase 1)
- Create `moat-kernel/src/scanline.rs`
- Implement: `precompute_backward_offsets`, `run_scanline_tile`, face classification, UF
- Wire into `lib.rs`, `kernel.rs`
- Add `--kernel scanline` to ISE CLI (default it)
- Write cross-validation test: run both legacy and scanline on k²=2, assert identical TileResult
- Gate: all 29 existing tests pass + new cross-validation test passes + `cargo clippy` clean

### Phase 3: Benchmark & Validate (Worker 3 — Claude, after Phase 2)
- Add benchmark binary or integration test: single tile at k²=36, R=80M, sizes 2000 and 4000
- Run benchmark, capture timing breakdown
- Run ISE moat detection at k²=26 (known moat) and k²=2 (control)
- Report: per-tile timing, memory usage, moat detection results
- Gate: <2s/tile for 2000², moat detected at k²=2, f(r) profile plausible at k²=26

### Worker Context Injection

Every worker MUST read before writing any code:
- `/Users/otonashi/thinking/building/gaussian-moat-cuda/CLAUDE.md`
- `/Users/otonashi/thinking/building/gaussian-moat-cuda/tile-probe/crates/moat-kernel/src/tile.rs` (existing kernel — match its face logic)
- `/Users/otonashi/thinking/building/gaussian-moat-cuda/tile-probe/crates/moat-kernel/src/kernel.rs` (trait interface)

### Expected Deliverables

1. `moat-kernel/src/primality.rs` — standalone primality module
2. `moat-kernel/src/scanline.rs` — scanline tile kernel
3. Modified `lib.rs`, `kernel.rs`, `ise/src/main.rs`
4. Benchmark results (inline report)
5. All tests green, clippy clean, release build succeeds
