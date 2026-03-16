# gaussian-moat-cuda

> **Work in progress.** This is an active research scratchpad. Performance numbers are being validated, architecture is under active optimization, and breaking changes are expected. Contributions welcome.

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

**Device Profiles.** Compile-time configuration via `device_config.cuh` for Jetson Orin Nano (SM 8.7, 1024 CUDA cores) and A100 (SM 8.0, 108 SMs). Tuned register counts, block sizes, and segment sizes per target.

## Performance

### Jetson Orin Nano 8GB (verified)

**CUDA Sieve:**

| Scale | Throughput | Notes |
|-------|-----------|-------|
| Near origin (10^9 norms) | 6.67M primes/sec | `--norm-lo 0 --norm-hi 1000000000` |
| sqrt(36) range (10^15 norms) | 1.45M primes/sec | High norm offsets |

**Rust Angular Connector:**

| Mode | Throughput | Notes |
|------|-----------|-------|
| With GPRF file, angular=6 | 1.35--1.78M primes/sec | 6 ARM cores, 26.4M primes |
| CPU-only fallback (no GPRF) | ~197K primes/sec | Internal sieve on ARM cores |

**Pipeline balanced throughput:** ~1.4M primes/sec (CUDA sieve feeding Rust connector).

Hardware: NVIDIA Jetson Orin Nano 8GB, 6-core ARM A78AE, JetPack 6.x, CUDA 12.x. Measured February--March 2026. All Jetson numbers are lower-bound mode only; upper-bound mode was never benchmarked on Jetson.

### Cloud: A100-SXM4-40GB (preliminary -- treat as directional)

Two A100 sessions were run on vast.ai in March 2026. The CUDA sieve numbers are solid. The connector numbers are unreliable due to a wedge-count misconfiguration (128 wedges instead of 32) that caused 88.6 GB RSS for 13.7M primes, and a GPRF filter bug that rejected all primes in upper-bound mode. Both bugs are fixed (commit e9780cc), but the connector has not been re-benchmarked at sqrt(36) scale since the fix.

**CUDA Sieve (post-D3 segmented base sieve):**

| Scale | Sieve Mode | MR Mode | Notes |
|-------|-----------|---------|-------|
| 10^9 (near origin) | 20.7M primes/sec | 9.0M primes/sec | 3.6x over Jetson |
| 10^15 (sqrt(36)) | 4.0M primes/sec | 1.7M primes/sec | 2.7x over Jetson |
| 10^18 (sqrt(40) probe) | 1.33M primes/sec | 1.49M primes/sec | MR overtakes sieve here |

At 10^18, the Miller-Rabin kernel overtakes the sieve (zero CPU base-prime prep). The crossover point is between 10^15 and 10^18.

**Bottleneck profile by scale:**

| Scale | GPU kernel time share | CPU base sieve share | Wall clock bottleneck |
|-------|----------------------|---------------------|----------------------|
| 10^9 | ~95% | negligible | GPU kernel |
| 10^15 | ~72% | ~2.4% (0.087s) | GPU kernel |
| 10^18 (pre-D3) | ~28% | ~69% (8.7s of 12.2s) | CPU base sieve |
| 10^18 (post-D3) | ~68% | ~32% (2.9s of 9.1s) | GPU kernel (fixed by D3) |

**Connector (first A100 attempt, pre-fix, unreliable):**

| Scenario | Throughput | RSS | Problem |
|----------|-----------|-----|---------|
| k^2=36, 13.7M primes, lower-bound | 28.7K primes/sec | 88.6 GB | Merge phase 91% of time; 128 wedges caused memory explosion |
| k^2=6, 783K primes, post-fix | 11.2K primes/sec | 2.3 GB | Small-scale only |

The connector at 28.7K primes/sec is roughly 120x slower than the sieve at 3.96M primes/sec on A100. The sieve sits 99% idle waiting for the connector. However, the 120x gap is likely inflated by the wedge-count misconfiguration -- the true gap after proper tuning is unknown.

Hardware: Session 1 -- NVIDIA A100-SXM4-40GB, 32-vCPU AMD EPYC 7713, 85 GB RAM, CUDA 12.1. Session 2 -- same GPU, 12-vCPU Intel Xeon. Measured March 2026.

### Verified Results

| k^2 | Moat found at | Origin component size |
|-----|--------------|----------------------|
| 2 | (11, 4) | -- |
| 4 | -- | 92 |
| 20 | -- | 273,791,623 |

## GPU Comparison

The sieve kernel is **shared-memory-throughput-bound**, not global-memory-bandwidth-bound. This means high-clock consumer GPUs can outperform datacenter GPUs for this specific workload.

| Metric | Jetson Orin Nano | RTX 4090 | A100 SXM4 40GB |
|--------|-----------------|----------|----------------|
| SMs | 16 (SM 8.7) | 128 (SM 8.9) | 108 (SM 8.0) |
| CUDA cores | 1,024 | 16,384 | 6,912 |
| Shared mem/SM | 48--100 KB | 48--100 KB | 48--164 KB |
| L2 cache | 2 MB | 72 MB | 40 MB |
| Boost clock | ~625 MHz | ~2520 MHz | ~1410 MHz |
| Memory BW | 68 GB/s | 1,008 GB/s | 1,555 GB/s |

**Expected sieve performance characteristics:**

| GPU | Sieve (10^15) | Sieve (10^18) | Why |
|-----|--------------|--------------|-----|
| Jetson Orin Nano | 1.45M/sec (measured) | -- | 16 SMs, low clock |
| A100 SXM4 | 4.0M/sec (measured) | 1.33M/sec (measured) | 108 SMs, moderate clock |
| RTX 4090 | ~6--8M/sec (estimated) | -- | 128 SMs, 1.79x higher clock than A100 |

The 4090's advantage comes from higher clock speed (2520 vs 1410 MHz, 1.79x) and more SMs (128 vs 108, 1.19x), giving ~2.1x theoretical advantage. The A100's higher memory bandwidth (1555 vs 1008 GB/s) is largely wasted because the sieve lives in shared memory -- global memory traffic is only bucket reads and output writes.

**For the connector (CPU-bound):** GPU choice is irrelevant. The connector runs entirely on CPU. Host CPU core count and memory bandwidth matter. The A100's typical cloud host (32-core EPYC) has an advantage over typical 4090 hosts for the connector phase.

## Known Issues

### Connector Wedge Count Regression

A sequence of three commits traces the wedge count story:

- **d45612b** (consolidation): `effective_wedge_count()` formula produced 128 wedges on A100's 32-core host. Each wedge allocates a full BandProcessor with grid hash map, node vectors, and union-find. At 13.7M primes, this caused 88.6 GB RSS (~6.5 KB per prime).
- **c3b56b5** (attempted fix): Wedge count adjustments.
- **e9780cc** (final fix): Capped wedge count at `max(cores, 4).min(32)`. On 32-core hosts this produces 32 wedges instead of 128. Memory impact not yet re-measured at sqrt(36) scale.

### GPRF Filter Bug in Upper-Bound Mode

The `iter_norm_range(start_norm, norm_bound)` call in the GPRF reader path rejected all primes when `start_norm` exactly matched the file's `norm_min`. Fixed in e9780cc by trusting the GPRF file's own norm range instead of re-filtering. The bug was latent since the consolidation commit (d45612b) but only manifested on A100 because Jetson tests used the internal sieve path, not GPRF files with `--start-distance`.

### GPU Occupancy

GPU occupancy on A100 is ~50%, limited by shared memory usage (32KB per block at 64 registers). Room for improvement via shared memory reduction or occupancy-aware tuning.

### No CUDA Stream Overlap

Zero overlap between sieve batches. The campaign script pipelines sieve N+1 while solving N, but the solver dominates so heavily (~480s vs ~4s) that the sieve finishes during the first seconds of the solver run. True CUDA stream overlap within the sieve itself (double-buffered output, overlapped kernel launches) is not implemented.

### Cross-Band Boundary Stitching

Not implemented. The campaign script (`a100-sqrt36-campaign.sh`) processes each band independently. Moat detection across band boundaries requires stitching, which is an architecture gap, not a regression.

## Roadmap

### Done

- [x] Segmented Eratosthenes GPU sieve with Cornacchia decomposition
- [x] Miller-Rabin alternative kernel (wins at 10^18+)
- [x] D3 segmented base sieve (fixed 10^18 CPU bottleneck: 12.2s -> 9.1s wall time)
- [x] Angular wedge connector with parallel union-find
- [x] GPRF binary format for sieve-to-solver handoff
- [x] Device profiles for Jetson Orin Nano and A100
- [x] Upper-bound probe mode (Tsuchimura's trick)
- [x] Wedge count regression fix (e9780cc)
- [x] GPRF filter bug fix in upper-bound mode (e9780cc)

### Not Yet Optimized

- [ ] GPU occupancy improvement (currently ~50% on A100, shared-memory-limited)
- [ ] CUDA stream overlap / double-buffered output within sieve
- [ ] Connector re-benchmark at sqrt(36) scale after wedge count fix
- [ ] Cross-band boundary stitching in campaign script
- [ ] Streaming band processor (evict old primes instead of loading all upfront)
- [ ] Compressed union-find (current ~6.5 KB/prime, target ~8 bytes/prime)

### Critical Path: Connector 120x Speedup

The connector is the bottleneck for any sqrt(36) campaign. At current speed (28.7K primes/sec on A100, likely inflated by misconfiguration), a single-GPU campaign would take ~36,000 days. The sieve contributes less than 1% of wall time.

Most promising path: **GPU-accelerated neighbor search** (Option C hybrid). A prototype showed 924--1430x speedup on neighbor pair generation at k^2=8. GPU spatial hash for neighbor finding, CPU union-find for connectivity. Code exists in `gaussian-moat-solver-hybrid/` but is not integrated.

Other targets:
- Streaming band processor with sliding-window eviction (logic exists in `band.rs` but angular mode loads all primes upfront)
- Compressed union-find to reduce memory from 6.5 KB/prime to ~8 bytes/prime
- Larger bands (10e9 or 100e9 norms) to amortize per-band overhead, contingent on memory fix

**Feasibility target:** connector at ~4s/band (120x improvement) would enable sqrt(36) on a single 4090 in ~1 month for ~$300.

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

By default this targets Jetson Orin Nano (SM 8.7). For A100:

```bash
cmake -DTARGET_DEVICE=a100 .. && make -j$(nproc)
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
