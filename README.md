# gaussian-moat-cuda

> **Work in progress.** This is an active research scratchpad. Performance numbers are being validated, architecture is under active optimization, and breaking changes are expected. Contributions welcome.

**Latest stable commit:** `cab53c3` -- "Fix 200x connector regression: per-prime angular overlap computation". All performance numbers below reference this commit or earlier unless noted otherwise.

GPU-accelerated solver for the [Gaussian moat problem](https://en.wikipedia.org/wiki/Gaussian_moat).

The Gaussian moat problem asks: can you walk from the origin to infinity in the complex plane, stepping only on Gaussian primes, with each step at most sqrt(k)? Tsuchimura (2005) proved computationally that for k = 36 the walk gets stuck -- the origin's connected component is finite, bounded at distance ~80M. This project is a modern CUDA + Rust pipeline to reproduce that result and push toward sqrt(40).

## Architecture

Four-stage pipeline: CPU base prime generation, GPU sieve kernel with Cornacchia decomposition, CPU bucket construction for large primes, and CPU-parallel connectivity analysis.

```
Stage 1: CPU Base Sieve         Stage 2: CPU Bucket Construction
Segmented Eratosthenes on CPU   Pre-compute per-segment bucket lists
generates base primes up to     for large primes that don't fit in
sqrt(norm_hi). At 10^15 this    shared memory. Feeds GPU kernel
takes ~0.09s. At 10^18 this     so each block knows which large
was the wall (8.7s) until D3    primes hit its segment.
segmented base sieve fixed it.

        |                               |
        v                               v

Stage 3: GPU Sieve Kernel       Stage 4: Rust Angular Connector
Per-block segmented sieve in    Memory-mapped GPRF reader
shared memory (32KB on A100,    + Parallel angular wedge decomposition
16KB on Jetson):                + Sliding-band union-find per wedge
  Phase 2A: tiny primes         + Boundary stitching
    (cooperative marking)         --> moat detection
  Phase 2B: medium primes
    (round-robin marking)       Supports lower-bound search (grow from
  Phase 2C: large primes        origin) and upper-bound probe (start
    (bucket-based, global mem)  from known boundary, verify no escape).
  Output: warp-level scan +
    Cornacchia decomposition
    --> GPRF binary file
    (a:i32, b:i32, norm:u64,
     16 bytes/record)
```

**Stage 1 -- CPU Base Sieve.** Generates all primes up to sqrt(norm_hi) on CPU. These are the sieving primes used by the GPU kernel. At 10^15 norms this is fast (0.09s), but at 10^18 the base prime range is enormous. The D3 segmented base sieve (commit on March 2026) reduced 10^18 base sieve time from 8.7s to 2.9s.

**Stage 2 -- CPU Bucket Construction.** Large primes (those that skip entire segments) are pre-sorted into per-segment bucket lists on CPU, so the GPU kernel can read them from global memory without searching. Small/medium primes are handled directly in the GPU shared memory bitmap.

**Stage 3 -- GPU Sieve Kernel + Cornacchia.** Each CUDA block processes one segment of the norm range using a shared-memory bitmap (32KB on A100, 16KB on Jetson). Three marking phases handle different prime sizes. After sieving, surviving candidates are extracted via warp-level parallel scan and decomposed into Gaussian primes via Cornacchia's algorithm (p = a^2 + b^2). Results are written in GPRF binary format.

**Stage 4 -- Rust Angular Connector.** Reads GPRF files via memory-mapped I/O. Decomposes the first octant into angular wedges processed in parallel (Rayon), each using a spatial hash grid and union-find structure (BandProcessor). Supports both lower-bound search (grow from origin) and upper-bound probe mode (Tsuchimura's trick: start from a known boundary distance and verify no path escapes).

**Device Profiles.** Compile-time configuration via `device_config.cuh` for Jetson Orin Nano (SM 8.7, 1024 CUDA cores), RTX 4090 (SM 8.9, 16384 CUDA cores), and A100 (SM 8.0, 108 SMs). Tuned register counts, block sizes, and segment sizes per target.

## Performance

### CUDA Sieve (primes/sec)

| Scale | Jetson Orin Nano | RTX 4090 | A100 SXM4 |
|-------|-----------------|----------|-----------|
| 10^8 (100M) | -- | 10.9M | -- |
| 10^9 | 6.67M | 33.0M | 20.7M |
| 10^15 | 1.45M | 4.84M | 4.0M |
| 10^18 | -- | -- | 1.33M (sieve) / 1.49M (MR) |

The sieve kernel is **shared-memory-throughput-bound**, not global-memory-bandwidth-bound. This means high-clock consumer GPUs can outperform datacenter GPUs: the 4090's advantage comes from higher clock speed (2520 vs 1410 MHz) and more SMs (128 vs 108). The A100's higher memory bandwidth (1555 vs 1008 GB/s) is largely wasted because the sieve lives in shared memory.

At 10^18, the Miller-Rabin kernel overtakes the sieve (zero CPU base-prime prep). The crossover point is between 10^15 and 10^18.

### Connector

The Rust angular connector is CPU-bound. GPU choice is irrelevant; host CPU core count and memory bandwidth determine throughput.

**Connector regression (commits d45612b through cab53c3):**

The connector suffered a severe performance regression that was diagnosed and fixed across five commits. The root cause was in the angular overlap computation: `overlap_radians = sqrt(k^2) / sqrt(start_norm)` was computed globally instead of per-prime. In lower-bound mode with `start_norm=2`, this produced `overlap = 4.24 radians > pi/4`, so every prime replicated to every wedge. N wedges meant N copies of the full problem -- zero parallelism benefit, Nx memory.

The fix (cab53c3) computes overlap per-prime using each prime's actual norm. High-norm primes route to 1-2 wedges instead of all wedges.

**Before/after (2.88M primes, k^2=36, angular auto, commit cab53c3):**

| Metric | Pre-fix | Post-fix | Improvement |
|--------|---------|----------|-------------|
| Jetson throughput | 3,323/sec | 2,396,408/sec | 721x |
| 4090 throughput | 7,176/sec | 3,893,128/sec | 542x |
| Jetson RSS | 4.9 GB | 146 MB | 34x reduction |
| 4090 RSS | 15.3 GB | 115 MB | 133x reduction |

Correctness: both platforms report farthest point (8458, 5335), distance 9999.999, 2.88M/2.88M primes in origin component. 4090 is 1.6x faster than Jetson at the same scale.

At sqrt(36) scale (25.4M primes), Jetson sustains 1,684,683 primes/sec at 912 MB RSS -- within the 8 GB envelope.

**Sieve-to-connector ratio:** pre-fix the sieve was ~120x faster than the connector (the connector was the wall). Post-fix the ratio is ~1:1 -- connector throughput matches sieve throughput. The pipeline is now sieve-bound, not connector-bound.

### Verified Results

| k^2 | Moat found at | Origin component size |
|-----|--------------|----------------------|
| 2 | (11, 4) | -- |
| 4 | -- | 92 |
| 20 | -- | 273,791,623 |

### sqrt(36) Campaign Feasibility (Post-Fix)

With connector throughput recovered, the sqrt(36) full campaign arithmetic changes substantially:

- Total norm range: [0, 6.4e15), at 1e9 norms/band = 6.4 million bands
- Per band at 10^15: ~14.5M primes, ~3.6s sieve
- Connector per band: ~8.6s on Jetson (1.7M/sec), ~3.7s on 4090 (3.9M/sec)
- **Jetson estimate:** ~12.2s/band, ~78M seconds total (~2.5 years)
- **4090 estimate:** ~7.3s/band, ~47M seconds total (~1.5 years)

Pre-fix, the connector alone was ~480s/band (28K/sec with replication bug), making the campaign ~36 days on A100 but solver-dominated 99%. Post-fix, the pipeline is balanced: sieve and connector are roughly equal time per band. The remaining bottleneck is the sheer number of bands (6.4M). Parallelizing across multiple GPUs or reducing band count (larger norm windows) is the path to feasibility.

## GPU Comparison

| Metric | Jetson Orin Nano | RTX 4090 | A100 SXM4 40GB |
|--------|-----------------|----------|----------------|
| SMs | 16 (SM 8.7) | 128 (SM 8.9) | 108 (SM 8.0) |
| CUDA cores | 1,024 | 16,384 | 6,912 |
| Shared mem/SM | 48--100 KB | 48--100 KB | 48--164 KB |
| L2 cache | 2 MB | 72 MB | 40 MB |
| Boost clock | ~625 MHz | ~2520 MHz | ~1410 MHz |
| Memory BW | 68 GB/s | 1,008 GB/s | 1,555 GB/s |

## Known Issues and Changelog

### Bugs Found and Fixed

| Commit | Bug | Fix |
|--------|-----|-----|
| d45612b | `effective_wedge_count` floor set to 130, producing far too many wedges | Fixed in c3b56b5: floor changed to 4 |
| 2d08cd9 | `[u32; 72]` fixed-size neighbor array silently dropped neighbors beyond 72 | Fixed in c3b56b5: replaced with SmallVec for dynamic allocation |
| e9780cc | GPRF filter rejecting all primes in upper-bound mode (`start_norm` matched file's `norm_min`) | Fixed in e9780cc: use `reader.norm_min` instead of re-filtering |
| e9780cc | Wedge count explosion on many-core hosts (128 wedges on 32-core A100) | Fixed in e9780cc: cap at `max(cores, 4).min(32)` |
| cab53c3 | Angular overlap computed globally instead of per-prime (every prime replicated to every wedge) | Fixed in cab53c3: per-prime overlap computation |

### Other Known Issues

**GPU Occupancy.** GPU occupancy on A100 is ~50%, limited by shared memory usage (32KB per block at 64 registers). Room for improvement via shared memory reduction or occupancy-aware tuning.

**No CUDA Stream Overlap.** Zero overlap between sieve batches. The campaign script pipelines sieve N+1 while solving N, but the solver dominates so heavily (~480s vs ~4s) that the sieve finishes during the first seconds of the solver run.

**Cross-Band Boundary Stitching.** Not implemented. The campaign script processes each band independently. Moat detection across band boundaries requires stitching -- an architecture gap, not a regression.

## Roadmap

### Top 3 Optimizations

1. **CUDA stream double-buffering (1.5-2x sieve throughput).** Overlap kernel execution with output D2H transfer and next-batch CPU prep. Currently zero overlap between batches.

2. **Halve shared memory per block (50% to ~80% GPU occupancy).** The sieve uses 32KB shared memory per block, capping occupancy at ~50% on A100. Reducing to 16KB would allow more concurrent blocks per SM.

3. **GPU-accelerated neighbor search (potential 100-1000x connector speedup).** Option C hybrid: GPU spatial hash for neighbor pair generation, CPU union-find for connectivity. A prototype showed 924-1430x speedup on neighbor pairs at k^2=8. Code exists in `gaussian-moat-solver-hybrid/` but is not integrated.

### Done

- [x] Segmented Eratosthenes GPU sieve with Cornacchia decomposition
- [x] Miller-Rabin alternative kernel (wins at 10^18+)
- [x] D3 segmented base sieve (fixed 10^18 CPU bottleneck: 12.2s -> 9.1s wall time)
- [x] Angular wedge connector with parallel union-find
- [x] GPRF binary format for sieve-to-solver handoff
- [x] Device profiles for Jetson Orin Nano, RTX 4090, and A100
- [x] Upper-bound probe mode (Tsuchimura's trick)
- [x] Wedge count regression fix (e9780cc)
- [x] GPRF filter bug fix in upper-bound mode (e9780cc)
- [x] Connector angular overlap regression fix (cab53c3)

### Not Yet Optimized

- [ ] CUDA stream overlap / double-buffered output within sieve
- [ ] GPU occupancy improvement (currently ~50% on A100, shared-memory-limited)
- [ ] GPU-accelerated neighbor search (Option C hybrid integration)
- [x] Connector re-benchmark at sqrt(36) scale after cab53c3 fix (validated: 2.4M/sec Jetson, 3.9M/sec 4090)
- [ ] Cross-band boundary stitching in campaign script
- [ ] Streaming band processor (evict old primes instead of loading all upfront)
- [ ] Compressed union-find (current ~6.5 KB/prime, target ~8 bytes/prime)

## Building

### Prerequisites

- CUDA Toolkit 12.x and an NVIDIA GPU
- Rust toolchain (stable, edition 2021)
- CMake >= 3.18

### CUDA Sieve

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

By default this targets Jetson Orin Nano (SM 8.7). For other devices:

```bash
cmake -DTARGET_DEVICE=a100 .. && make -j$(nproc)
cmake -DTARGET_DEVICE=rtx4090 .. && make -j$(nproc)
```

### Rust Solver

```bash
cd solver && cargo build --release
```

## Usage

### Full pipeline (sieve + connectivity)

```bash
./run-pipeline.sh --k-squared 36 --norm-hi 1000000000 --output-dir ./output
```

### CUDA sieve only (generate GPRF file)

```bash
./build/gm_cuda_primes --norm-lo 0 --norm-hi 1000000000 --output primes.gprf --mode sieve
```

### Rust solver with GPRF (angular connectivity, lower-bound)

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 36 --angular 6 --prime-file primes.gprf
```

### Upper-bound probe (Tsuchimura's trick)

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 36 --angular 6 --prime-file primes.gprf --start-distance 80015782
```

### Norm-stream mode (CPU-only, no GPU required)

```bash
./solver/target/release/gaussian-moat-solver \
    --k-squared 26 --norm-bound 25000000000000
```

## Directory Structure

```
gaussian-moat-cuda/
  src/                  CUDA sieve (.cu, .cuh) -- kernels, Cornacchia, Miller-Rabin, GPRF writer
  solver/               Rust angular connectivity solver
    src/                angular, band, sieve, union-find, stitcher, GPRF reader
  tests/                CUDA correctness tests
  tools/                Analysis scripts
  deploy/               A100 deployment and campaign scripts
  run-pipeline.sh       Two-stage pipeline runner
  CMakeLists.txt        CUDA build config (device profiles)
  PERFORMANCE.md        Full benchmark data and bottleneck analysis
  AUDIT-2026-03-16.md   Deep audit: regression timeline, architecture analysis, feasibility
```

## License

MIT -- see [LICENSE](LICENSE).
