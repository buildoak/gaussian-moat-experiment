# gaussian-moat-cuda

CUDA + Rust pipeline for the Gaussian moat problem. Targets Tsuchimura's 2004 sqrt(36) result as the first milestone and sqrt(40) as the scientifically interesting next step.

> **Work in progress.** Architecture is stable, performance is validated across three GPU platforms, five bugs have been found and fixed. The pipeline works end-to-end but the full sqrt(36) campaign has not yet been run to completion.

**Latest stable commit:** `cab53c3` -- all performance numbers reference this commit unless noted otherwise.

---

## The Problem

### Gaussian Primes

A Gaussian integer is a complex number a + bi where a and b are integers. Its norm is a^2 + b^2. A Gaussian integer is a Gaussian prime if:

- **Split primes:** norm p is a rational prime with p = 1 (mod 4). Then p = a^2 + b^2 for unique a > b > 0 (Cornacchia's algorithm finds a, b).
- **Inert primes:** a Gaussian integer (p, 0) where p = 3 (mod 4) is a rational prime. Norm is p^2.
- **Ramified prime:** (1, 1) with norm 2.

In the first octant of the complex plane (a >= b >= 0), every Gaussian prime falls into exactly one of these categories.

### The Gaussian Moat Problem

Can you walk from the origin to infinity, stepping only on Gaussian primes, with each step at most sqrt(k) in Euclidean distance? Equivalently: is the connected component of the origin finite under the step distance sqrt(k)?

The **step parameter k^2** determines which primes are neighbors. Two Gaussian primes (a1, b1) and (a2, b2) are connected if (a1 - a2)^2 + (b1 - b2)^2 <= k^2. Small k^2 means the walk gets stuck quickly (moat found close to origin). Larger k^2 means the connected component extends further before isolation.

### Why sqrt(36)?

Tsuchimura (2004) proved computationally that for k^2 = 36, the walk from the origin gets stuck. The origin's connected component is finite. An upper bound of ~80,015,782 on the farthest reachable distance was established; the true farthest point may be smaller. It is the current computational record. (Note: the 139 billion prime figure belongs to the sqrt(32) lower-bound computation, not sqrt(36).)

### Why sqrt(40)?

At k^2 = 36, the step vectors p(k) ≈ 14 (the number of lattice points within the step circle; Tsuchimura reports p(√36)=14). At k^2 = 40, p(k) jumps to 18. This transition changes the connectivity structure and is the next scientifically meaningful threshold. The norm range extends to 10^18, roughly 150x the computational work of sqrt(36).

### Upper-Bound vs Lower-Bound Probing

Tsuchimura's key insight: you do not need to grow the connected component from the origin all the way out. The **upper-bound probe** works as follows: choose a distant Gaussian prime y; fictitiously assume ALL primes with |z| ≤ |y| are already connected to the origin (this only enlarges the component); then run the same sequential subgraph construction forward from |y|. If the algorithm terminates (the origin component root exits the processing band without finding further connections), then |y| is an upper bound on the farthest reachable distance. Because the assumption only enlarges the component, termination still proves the bound.

This project implements both modes:
- **Lower-bound:** grow from the origin, find the farthest reachable prime.
- **Upper-bound:** start from a known boundary, verify isolation.

---

## Architecture

Four-stage pipeline: CPU base sieve, CPU bucket construction, GPU sieve with Cornacchia decomposition, and Rust angular connector.

```
Stage 1: CPU Base Sieve            Stage 2: CPU Bucket Construction
Segmented Eratosthenes on CPU      Two-pass scatter: count hits per
generates base primes up to         segment, then fill bit positions.
sqrt(norm_hi). L1-friendly 16KB    Large primes (span > segment size)
working set avoids cache thrash.    are pre-sorted into per-segment
At 10^15: 0.09s. At 10^18: 2.9s   bucket lists so the GPU can read
(after D3 segmented base fix).     them from global memory.
        |                                   |
        v                                   v
Stage 3: GPU Sieve Kernel          Stage 4: Rust Angular Connector
Per-block segmented sieve in       Memory-mapped GPRF reader (mmap)
shared memory (32KB A100/4090,     + Parallel angular wedge decomp
16KB Jetson):                        (Rayon thread pool)
  Phase 2A: tiny primes (<256)     + Per-prime overlap routing
    cooperative marking, all         (cab53c3 fix: per-prime angular
    threads sweep one prime          overlap instead of global)
  Phase 2B: medium primes          + Sliding-band BandProcessor per
    round-robin, one thread          wedge: spatial hash grid +
    per prime                        union-find with slot recycling
  Phase 2C: large primes           + Boundary stitching across wedges
    bucket-based, global mem
                                   Supports lower-bound (origin grow)
  Output: warp-level scan +        and upper-bound probe (Tsuchimura's
  Cornacchia decomposition         trick: assume all primes ≤|y| connected,
  --> GPRF binary file              prove termination from |y| = UB).
```

### GPRF Format

The sieve-to-solver handoff uses a custom binary format:

- **Header:** 64 bytes (magic `0x47505246`, version, prime count, norm range, sieve bound)
- **Records:** 16 bytes each (a: i32, b: i32, norm: u64), packed, no padding
- **Ordering:** sorted by (norm, a, b)
- **I/O:** C++ writer with 64KB buffered writes; Rust reader via memory-mapped I/O with binary search for norm-range queries

### How the Sieve Works

Each CUDA block processes one segment of the norm range using a shared-memory bitmap. Bits represent odd numbers; even numbers are skipped. Three phases handle primes of different sizes:

- **Phase 2A (tiny primes, p < 256):** All threads cooperatively mark multiples of each small prime. High thread utilization but shared-memory bank conflicts from atomicOr.
- **Phase 2B (medium primes, 256 <= p <= segment span):** Round-robin assignment, one thread per prime. Each thread marks all multiples of its assigned prime within the segment.
- **Phase 2C (large primes, p > segment span):** These primes may hit only 0-1 times per segment. CPU pre-computes which segments each large prime hits (bucket construction). GPU reads hit positions from global memory.

After sieving, surviving candidates are extracted via warp-level parallel scan (ballot + popc + prefix sum), then decomposed into Gaussian primes via Cornacchia's algorithm on GPU. Output uses warp-level compaction: one atomicAdd per warp instead of per thread.

### How Cornacchia Works

For a prime p = 1 (mod 4), Cornacchia's algorithm finds a, b such that a^2 + b^2 = p. It first finds a square root of -1 mod p via Euler's criterion, then applies a GCD-like descent. Each GPU thread independently decomposes its candidate prime. The sieve kernel also handles inert primes (p = 3 mod 4) by emitting (p, 0, p^2).

### How the Angular Connector Works

The Rust solver decomposes the first octant (0 to pi/4 radians) into angular wedges processed in parallel via Rayon:

1. **Prime routing** (`prime_router.rs`): Each prime is assigned to a primary wedge based on its angle. Primes near wedge boundaries are replicated to adjacent wedges with overlap proportional to sqrt(k^2) / sqrt(norm). The cab53c3 fix computes this overlap per-prime using the prime's own norm -- previously it was computed globally with start_norm=2, causing every prime to replicate to every wedge.

2. **BandProcessor** (`band.rs`): Each wedge maintains a spatial hash grid (cell size = ceil(sqrt(k^2))) and a union-find structure. For each incoming prime, the processor inserts it into the grid, searches the 3x3 neighborhood of adjacent cells for primes within step distance, and unions connected components. An eviction queue recycles slots for primes whose norm falls below the reachable window.

3. **Boundary stitching** (`stitcher.rs`): After all wedges complete, shared primes in overlap zones are used to merge components across wedge boundaries. The stitcher maps component roots from adjacent wedges via the overlap primes and merges origin components.

4. **Upper-bound mode:** Fictitiously assumes all primes with norm ≤ boundary_plus_k are already connected to the origin (this only enlarges the component). The algorithm then runs forward from that boundary; if the origin component root exits the processing band without finding further connections, the start distance is confirmed as an upper bound on the farthest reachable distance.

---

## Performance

All numbers measured February-March 2026. Validated on three GPU platforms with bitwise-identical prime counts across all devices at every tested scale.

### CUDA Sieve (Gaussian primes/sec)

- **Jetson Orin Nano** (SM 8.7, 1024 cores):
  - 10^9 norms (near origin): **6.67M primes/sec**
  - 10^15 norms (sqrt(36) scale): **1.45M primes/sec**

- **RTX 4090** (SM 8.9, 16384 cores):
  - 10^8 norms: **10.9M primes/sec**
  - 10^9 norms: **33.0M primes/sec** (4.6x over Jetson)
  - 10^15 norms: **4.84M primes/sec** (3.0x over Jetson)

- **A100 SXM4 40GB** (SM 8.0, 108 SMs), post-D3 segmented base sieve:
  - 10^9 norms: **20.7M primes/sec**
  - 10^15 norms: **4.0M primes/sec**
  - 10^18 norms (sieve): **1.33M primes/sec** (9.09s wall)
  - 10^18 norms (Miller-Rabin): **1.49M primes/sec** (8.07s wall)

**Why the 4090 beats the A100:** The sieve kernel is shared-memory-throughput-bound, not global-memory-bandwidth-bound. The 4090's advantages -- higher boost clock (2520 vs 1410 MHz) and more SMs (128 vs 108) -- directly translate to sieve performance. The A100's superior memory bandwidth (1555 vs 1008 GB/s) is largely wasted because the bitmap lives in shared memory.

**MR crossover at 10^18:** At 10^18, the Miller-Rabin kernel (1.49M/sec) overtakes the sieve (1.33M/sec) by 12%. MR has zero CPU base-prime preparation -- each thread independently tests one candidate. The crossover point is between 10^15 (where sieve is 2.4x faster) and 10^18 (where MR wins). This matters for sqrt(40).

### Angular Connector (post-fix, cab53c3)

The connector is CPU-bound. GPU choice is irrelevant; host CPU core count and single-thread performance determine throughput.

**Post-fix results (2.88M primes, k^2=36):**

- **Jetson Orin Nano:** 2,396,408 primes/sec, 146 MB RSS
- **RTX 4090 host:** 3,893,128 primes/sec, 115 MB RSS (1.6x over Jetson)
- **Jetson at sqrt(36) scale** (25.4M primes): 1,684,683 primes/sec, 912 MB RSS

Correctness: both platforms report farthest point (8458, 5335), distance 9999.999, 2,881,124/2,881,124 primes in origin component. Results are bitwise identical.

### The 721x Fix

Commits d45612b through e9780cc contained a critical bug in angular wedge decomposition. The overlap radius was computed globally as `sqrt(k^2) / sqrt(start_norm)` instead of per-prime using each prime's actual norm. In lower-bound mode, `start_norm` defaults to 2, producing `overlap = sqrt(36) / sqrt(2) = 4.24 radians`. Since the first octant spans only pi/4 = 0.785 radians, every prime replicated to every wedge. N wedges meant N copies of the full problem: zero parallelism, Nx memory.

The 3-line fix in `solver/src/prime_router.rs` (cab53c3) computes overlap per-prime:

```rust
let overlap_radians = (k_squared as f64).sqrt() / prime_norm.sqrt();
```

High-norm primes (the vast majority) have tiny angular overlap and route to 1-2 wedges instead of all wedges.

| Metric | Pre-fix | Post-fix | Factor |
|--------|---------|----------|--------|
| Jetson throughput | 3,323/sec | 2,396,408/sec | **721x** |
| 4090 throughput | 7,176/sec | 3,893,128/sec | **542x** |
| Jetson RSS | 4.9 GB | 146 MB | 34x reduction |
| 4090 RSS | 15.3 GB | 115 MB | 133x reduction |

**Pipeline balance:** Pre-fix, the sieve was ~120x faster than the connector (the connector was the wall). Post-fix, the sieve-to-connector ratio is ~1:1.7 (connector is now faster). The pipeline is sieve-bound.

### Verified Correctness Results

| k^2 | Moat found | Farthest point | Origin component size |
|-----|-----------|----------------|----------------------|
| 2 | Yes | (11, 4) at sqrt(137) | 14 primes |
| 4 | Yes | -- | 92 primes |
| 20 | Yes | -- | 273,791,623 primes |

The primary correctness gate is `farthest_point == (11, 4)` at k^2 = 2, validated on all three platforms in both lower-bound and upper-bound modes.

---

## GPU Profiles

Three device profiles in `src/device_config.cuh`, selected at compile time:

**Jetson Orin Nano** (SM 8.7) -- development and test device
- 16 SMs, 1024 CUDA cores, ~625 MHz boost, 8 GB unified memory
- 16KB shared memory per segment (256K norms), 48 max registers
- Thermal management enabled (throttles at 78C)
- Uses CUDA managed memory for GPU sort

**RTX 4090** (SM 8.9, Ada Lovelace) -- primary compute
- 128 SMs, 16384 CUDA cores, ~2520 MHz boost, 24 GB GDDR6X
- 32KB shared memory per segment (512K norms), 64 max registers
- 10000 segments per batch

**A100 SXM4** (SM 8.0) -- validated, cloud deployment
- 108 SMs, 6912 CUDA cores, ~1410 MHz boost, 40 GB HBM2e
- 32KB shared memory per segment (512K norms), 64 max registers
- 8000 segments per batch

**Adding a new profile:** Add an `#elif defined(TARGET_NEWGPU)` block in `src/device_config.cuh` with the appropriate segment span, bitmap words, register count, grid cap, and batch size. Add the corresponding CMake block in `CMakeLists.txt` with the SM architecture code and compile definitions.

---

## Build and Run

### Prerequisites

- CUDA Toolkit 12.x and an NVIDIA GPU (SM 8.0+)
- Rust toolchain (stable, edition 2021)
- CMake >= 3.18

### Build CUDA Sieve

```bash
mkdir -p build && cd build

# Jetson Orin Nano (default)
cmake .. && make -j$(nproc)

# RTX 4090
cmake -DTARGET_DEVICE=4090 .. && make -j$(nproc)

# A100
cmake -DTARGET_DEVICE=a100 .. && make -j$(nproc)
```

### Build Rust Solver

```bash
cd solver && cargo build --release
```

### Correctness Gate (quick test)

```bash
# Generate primes for small norm range
./build/gm_cuda_primes --norm-lo 0 --norm-hi 10000 --output /tmp/test.gprf --mode sieve

# Verify: must output "farthest point: (11, 4)"
./solver/target/release/gaussian-moat-solver \
    --k-squared 2 --angular 1 --prime-file /tmp/test.gprf
```

### Full Pipeline (sieve + connectivity)

```bash
./run-pipeline.sh --k-squared 36 --norm-hi 1000000000 --output-dir ./output
```

### CUDA Sieve Only (generate GPRF)

```bash
./build/gm_cuda_primes --norm-lo 0 --norm-hi 1000000000 --output primes.gprf --mode sieve
```

### Rust Angular Connector (lower-bound)

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 36 --angular 6 --prime-file primes.gprf
```

### Upper-Bound Probe (Tsuchimura's trick)

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 36 --angular 6 --prime-file primes.gprf --start-distance 80015782
```

### Norm-Stream Mode (CPU-only, no GPU required)

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 26 --norm-bound 25000000000000
```

---

## Project History

This project evolved from a pure Rust solver into a CUDA + Rust hybrid pipeline.

**Phase 1: Rust-only solver** (archived at `_archive/gaussian-moat-solver-final/`)
- Norm-stream architecture: CPU-generated Gaussian primes fed into band-based union-find
- Validated sqrt(20) and sqrt(26) small-scale reproductions
- Throughput ceiling: ~630K primes/sec on cloud x86 hardware

**Phase 2: CUDA acceleration** (this repository)
- `06c59c9` Initial CUDA generator for Jetson Orin Nano (Miller-Rabin kernel)
- `9cce73c` MR kernel optimization: 481K -> 1.06M primes/sec (2.2x)
- `1ed29f9` Segmented sieve kernel: 5x speedup over MR at near-origin scales
- `56ec553` Large prime bucketing, mod-4 filter, GPU sort
- `4e8021b` GPRF binary format for sieve-to-solver handoff
- `d45612b` Consolidated Rust angular solver into repo

**Phase 3: Multi-GPU validation and bug fixing**
- `87bf891` A100 config, warp-ballot Cornacchia compaction, buffered GPRF writes
- `463c02a` A100 deployment scripts and sqrt(36) campaign script
- `e9780cc` Fixed GPRF filter bug + wedge count explosion on A100
- `c3b56b5` Fixed wedge count floor (130 -> 4) and SmallVec for dynamic neighbors
- `d538440` D3 segmented base sieve: 3x base-prime speedup at 10^18

**Phase 4: Performance recovery**
- `cab53c3` Fixed the 721x connector regression (per-prime angular overlap)
- `947bd82` RTX 4090 vs Jetson 5-experiment benchmark matrix
- `6e37a52` Validated post-fix results across platforms

---

## Bug Chronicle

Five bugs found and fixed during the project. Documented here for future sessions -- knowing what broke and why prevents repeats.

### Bug 1: Wedge Count Floor (d45612b -> c3b56b5)

**Symptom:** `effective_wedge_count` had a floor of 130, producing far too many wedges on any hardware. Each wedge allocates its own BandProcessor with grid hash, node vectors, and union-find.

**Root cause:** Copy-paste from an earlier prototype that assumed a different data model.

**Fix:** Changed floor to 4 in `solver/src/angular.rs`.

### Bug 2: Fixed-Size Neighbor Array (d45612b -> c3b56b5)

**Symptom:** `connect_neighbors` used `[u32; 72]` fixed-size array. When a prime had more than 72 neighbors within step distance, excess neighbors were silently dropped. This caused incorrect component sizes at higher k^2 values.

**Root cause:** Static array size assumed a maximum neighbor count that does not hold for large k^2.

**Fix:** Replaced with `SmallVec<[u32; 32]>` in `solver/src/band.rs` -- stack-allocated for common case, heap-spills for rare dense neighborhoods.

### Bug 3: GPRF Filter Rejects All Primes in Upper-Bound Mode (d45612b -> e9780cc)

**Symptom:** `--start-distance 80015782 --prime-file primes.gprf` processed 0 primes. The solver loaded a GPRF file then filtered by start_norm, which matched the file's own norm range, rejecting everything.

**Root cause:** The code applied `iter_norm_range(start_norm, ...)` to GPRF files. When the GPRF's norm_min equaled the computed start_norm (~6.4e15 for sqrt(36)), the binary search filter rejected all records.

**Fix:** When a GPRF file is provided, trust the file's own norm range. The CUDA sieve already produced primes for exactly the right range; re-filtering was redundant and destructive.

### Bug 4: Wedge Count Explosion on Many-Core Hosts (d45612b -> e9780cc)

**Symptom:** On A100's 32-core host, `effective_wedge_count` returned 128 (4 * cores). Each wedge allocated a full BandProcessor. At 128 wedges, 13.7M primes consumed 82.6 GB RSS (~6 KB/prime vs expected ~16 bytes).

**Root cause:** Formula `max(4*cores, 130)` was calibrated for a Jetson with 6 cores. On 32-core hosts it produced extreme wedge counts, and each wedge's per-prime overhead (grid hash map entries, node vectors, union-find slots) compounded with overlap replication.

**Fix:** Changed to `max(cores, 4).min(32)` -- 1 wedge per core with a ceiling of 32.

### Bug 5: Angular Overlap Computed Globally (d45612b -> cab53c3)

**Symptom:** Connector throughput inversely proportional to wedge count. 4 wedges: 28K/sec. 32 wedges: 1.6K/sec. Memory scaled linearly with wedge count. The "more parallelism = more speed" assumption was inverted.

**Root cause:** In `prime_router.rs`, overlap was computed as `sqrt(k^2) / sqrt(start_norm)` with start_norm=2 (lower-bound default). This produced overlap = 4.24 radians, exceeding the entire first-octant span of pi/4 = 0.785 radians. Every prime was replicated to every wedge.

**Fix:** Compute overlap per-prime using the prime's actual norm: `sqrt(k^2) / sqrt(prime.norm)`. Three lines changed, 721x throughput improvement.

---

## Known Limitations and Optimization Roadmap

### Current Limitations

**GPU occupancy at ~50%.** On A100 (SM 8.0), 256 threads/block at 64 regs/thread = 16,384 regs/block. With 65,536 regs/SM, the register limit caps at 4 blocks (1024 threads out of 2048 max) = 50% occupancy. On 4090 (SM 8.9), 32KB shared/block against 100KB/SM caps at 3 blocks (768 threads out of 1536 max) = 50%. Halving shared memory to 16KB helps on 4090 (100KB/16KB = 6 blocks, registers still cap at 4 -> 1024/1536 = ~67%) but **not** on A100 (164KB/16KB = 10 blocks, but registers still cap at 4 -> 1024/2048 = 50%). Improving past 50% on A100 requires reducing register pressure below 64/thread.

**Zero CPU-GPU pipeline overlap.** Within the sieve, there is no overlap between batch processing stages. The CPU prepares base primes and bucket data, then the GPU runs, then the CPU copies results. CUDA stream double-buffering could overlap kernel execution with output D2H transfer and next-batch CPU preparation.

**Sieve throughput degrades at scale.** At higher norms, prime density decreases (fewer primes per candidate) and base-prime generation takes longer. On A100, the 10^9 -> 10^15 -> 10^18 progression shows 20.7M -> 4.0M -> 1.33M primes/sec. (Jetson numbers at 10^9 and 10^15 are 6.67M and 1.45M respectively -- do not mix hardware series.)

**Cross-band boundary stitching not implemented.** The campaign script processes each band independently. Moat detection across band boundaries requires stitching -- the architecture supports it (upper-bound mode with start-distance) but the automation is not wired up.

**Connector memory overhead.** At ~6.5 KB/prime (with high wedge counts), the connector's memory footprint limits the maximum prime count per band. At 25.4M primes with auto wedges, even 64 GB RAM can be insufficient. Post-fix (cab53c3) this is dramatically reduced (146 MB for 2.88M primes) but remains a concern at larger scales.

### Optimization Roadmap

1. **CUDA stream double-buffering** (1.5-2x sieve throughput). Overlap kernel execution with output transfer and next-batch CPU prep. Currently zero batch overlap.

2. **Halve shared memory per block** (50% to ~67% on 4090, no change on A100). Reduces 4090 binding constraint from shared memory (3 blocks) to registers (4 blocks). On A100, registers are already the bottleneck at 4 blocks -- smaller segments alone don't help. Pair with register reduction for A100 gains.

3. **GPU-accelerated neighbor search** (potential 100-1000x connector speedup). A prototype in `gaussian-moat-solver-hybrid/` showed 924-1430x speedup on neighbor pair generation at k^2=8 using GPU spatial hash + CPU union-find. Not yet integrated.

4. **Streaming band processor.** Evict old primes instead of loading all upfront. The eviction logic exists in `band.rs` but the angular mode currently collects all primes into memory before processing.

5. **Compressed union-find.** Current overhead is ~6.5 KB/prime at high wedge counts. A compact implementation (parent + rank packed into 4 bytes, 4 bytes for norm) could reduce to ~8 bytes/prime.

6. **Hybrid sieve+MR mode.** Use the sieve kernel at low/medium norms and switch to MR at high norms (beyond the 10^15-10^18 crossover point) to eliminate the CPU base-prime bottleneck.

### Auto-Research Loop (not yet ready)

The test infrastructure exists (CUDA unit tests, Rust integration tests, the k^2=2 correctness gate) but three pieces are missing for safe automated kernel optimization:

- `gate.sh` -- single script that builds, runs all tests, verifies correctness, and checks no immutable files were modified
- Regression gates -- performance baselines with tolerance bands at each scale tier
- Reference GPRFs at scale -- a small band at 10^15 for diff-based correctness checking

---

## sqrt(36) Campaign Feasibility

The campaign uses **upper-bound (UB) probing** -- Tsuchimura's trick. Instead of growing the connected component from the origin across the entire norm range [0, 6.4e15), we choose a candidate distance X, fictitiously assume all primes with |z| ≤ X are connected to the origin, and run the algorithm forward from X. If the algorithm terminates without the component extending further, X is confirmed as an upper bound on the farthest reachable distance. This reduces the problem from scanning ~6.4 million bands to processing a single narrow shell.

### Upper-Bound Probe Mechanics

A UB probe at `--start-distance X` with k^2=36 (k=6):

1. **Start norm:** `upper_bound_start_norm(36, X)` = `(X - 6)^2` (from `angular.rs`)
2. **Boundary plus k:** `ceil_radius_sum_sq(X^2, 36)` = `(X + 6)^2` -- all primes below this norm are fictitiously assumed connected to the origin (the UB assumption)
3. **Moat threshold:** `ceil_radius_sum_sq(farthest_norm, 36)` -- when processing passes this norm without extending the component, moat is detected
4. **Effective shell width:** From `(X-6)^2` to `(X+6)^2` ≈ 24X norms. The BandProcessor's eviction window means only this sliding shell is in memory at any time.

### Known-Boundary Campaign (sqrt(36), Tsuchimura boundary ≈ 80,015,782)

**Shell parameters at X = 80,015,782:**

- Shell width: 24 * 80,015,782 ≈ 1.92e9 norms
- Prime density at norm ~6.4e15: 1 / (2 * ln(6.4e15)) ≈ 1/72.8 primes per norm
- Primes in shell: ~26.4M primes

**Single UB probe time on RTX 4090:**

- Sieve: 26.4M primes at 4.84M primes/sec ≈ **5.5s**
- Connector: 26.4M primes at 3.9M primes/sec ≈ **6.8s**
- **Total: ~12s** for a single probe confirming the moat

**Single UB probe on Jetson Orin Nano:**

- Sieve: 26.4M primes at 1.45M primes/sec ≈ 18.2s
- Connector: 26.4M primes at 1.7M primes/sec ≈ 15.5s
- **Total: ~34s**

### Blind Search (boundary unknown)

If we don't know the boundary distance, use progressive UB probes at geometrically increasing distances (1000, 2000, 4000, ..., 80M):

- Doublings to reach 80M from 1000: log2(80,000) ≈ 17 probes
- Each probe i at distance D_i has shell width 24 * D_i. Total primes across all probes is dominated by the final probe (geometric series sums to ~2x the largest term).
- **Total time: ~25s on single 4090** (~2x the final probe)
- If boundary were at 200M: ~18 probes, ~30s total

### Why UB vs LB

The lower-bound (LB) approach processes ALL primes in [0, 6.4e15) -- on the order of hundreds of billions of Gaussian primes across 6.4 million bands (for context: Tsuchimura's sqrt(32) LB run generated ~139 billion primes; sqrt(36) would be larger). The upper-bound approach processes only the ~26M primes in the boundary shell. The ratio is approximately **5,000x less work**.

### LB Estimates (for reference -- theoretical lower-bound cost)

If the campaign were run in lower-bound mode (grow from origin, process entire norm range):

- **Total norm range:** [0, 6.4e15), at 10^9 norms/band = 6.4 million bands
- **Per band at 10^15 scale:** ~14.5M primes, sieve ~3.0s, connector ~3.7s on 4090
- **RTX 4090:** ~6.7s/band, ~43M seconds total (~1.4 years). Unfeasible single-GPU.
- **10x 4090s with 10x connector speedup:** ~30 days at ~$3K cloud cost.

### Critical Path

With UB probing, the campaign is **already feasible on a single 4090 in under a minute.** The critical remaining work is:

1. **Automation:** Wire up the UB probe into the campaign script with progressive distance doubling
2. **Validation:** Cross-check the UB result against a partial LB run (e.g., first 10^12 norms) to confirm the boundary distance
3. **Correctness gate:** Verify the upper bound on farthest reachable distance matches Tsuchimura's published result (UB ≈ 80,015,782; the true farthest point is ≤ this value)

---

## Directory Structure

```
gaussian-moat-cuda/
  src/                    CUDA sieve sources
    main.cu               Host pipeline: batch loop, sort, GPRF output
    sieve_kernel.cu       Segmented sieve + Cornacchia dispatch kernels
    kernel.cu             Miller-Rabin alternative kernel
    sieve_base.cuh        CPU segmented base sieve (D3)
    device_config.cuh     Compile-time GPU profiles
    gprf_writer.cuh       GPRF binary writer with buffered I/O
    cornacchia.cuh        Cornacchia decomposition (device)
    miller_rabin.cuh      Miller-Rabin primality test (device)
    modular_arith.cuh     Modular exponentiation (device)
    types.h               GaussianPrime struct
  solver/                 Rust angular connectivity solver
    src/
      main.rs             CLI entry point
      angular.rs          Angular wedge orchestrator (Rayon parallel)
      band.rs             BandProcessor: spatial hash + union-find
      prime_router.rs     Per-prime angular overlap routing (cab53c3 fix)
      union_find.rs       Union-find with slot recycling and path halving
      gprf_reader.rs      Memory-mapped GPRF reader
      stitcher.rs         Cross-wedge boundary stitching
      sieve.rs            CPU prime stream (fallback, no GPU)
      runner.rs           Norm-stream mode runner
      progress.rs         Progress reporting
  tests/                  CUDA correctness tests
  tools/                  Analysis scripts
  deploy/                 Cloud deployment and campaign scripts
    a100-deploy.sh        Full A100 deployment (rsync, build, smoke test)
    a100-sqrt36-campaign.sh  Band-iterative campaign with pipeline overlap
    a100-mr-benchmark.sh  MR kernel benchmarking
    4090-connector-experiments.sh  4090 connector experiment matrix
  run-pipeline.sh         Two-stage local pipeline runner
  CMakeLists.txt          CUDA build config with device profile selection
  PERFORMANCE.md          Raw benchmark data and bottleneck analysis
  LICENSE                 MIT
```

---

## References

- Tsuchimura, H. (2004). "Computational results for Gaussian moat problem." Technical Report METR 2004-13, Nihon University. Established the sqrt(36) upper bound (≤ 80,015,782) and sqrt(32) lower bound as the current computational records. (A journal version may have appeared in 2005.)
- Gethner, Wagon, & Wick (1998). "A stroll through the Gaussian primes." *American Mathematical Monthly*. Introduced the Gaussian moat problem.
- GPRF format: custom binary format defined in `src/gprf_writer.cuh` and `solver/src/gprf_reader.rs`. 64-byte header + 16 bytes per record (a: i32, b: i32, norm: u64).

---

## License

MIT -- see [LICENSE](LICENSE).
