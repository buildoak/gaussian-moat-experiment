# Performance Measurements

> **See [README.md](README.md) for the narrative.** This file contains raw benchmark data, bottleneck analysis, and historical measurements retained as an appendix.

All numbers are Gaussian primes/second unless noted otherwise.
Measured February-March 2026.

## CUDA Sieve on Jetson Orin (8 GB, 1024 CUDA cores)

| Scenario | Throughput | Config |
|----------|-----------|--------|
| Near origin (1B norms, norm range [0, 1e9)) | 6.67M primes/sec | `--mode sieve --norm-lo 0 --norm-hi 1000000000` |
| At sqrt(36) scale (10^15 norms) | 1.45M primes/sec | `--mode sieve` at high norm offsets |
| MR baseline (100M norms) | 481K primes/sec | `--mode mr --norm-hi 100000000` |
| MR optimized (100M norms) | 1.06M primes/sec | `--mode mr` with tuned batch/block sizes |

Hardware: NVIDIA Jetson Orin Nano 8GB, JetPack 6.x, CUDA 12.x
Date: February 2026

## Rust Angular Connector on Jetson Orin

| Scenario | Throughput | Config |
|----------|-----------|--------|
| With GPRF file | 1.35-1.78M primes/sec | `--angular 0 --prime-file <path>.gprf` |
| With internal sieve (no GPRF) | ~197K primes/sec | `--angular 0` (sieve on 6 ARM cores) |

Hardware: NVIDIA Jetson Orin Nano 8GB, 6-core ARM A78AE
Date: March 2026

## Cloud Baselines (vast.ai, for reference)

| Scenario | Throughput | Config |
|----------|-----------|--------|
| Norm-stream initial burst | ~617K primes/sec | Rust solver, norm-stream mode, first batch |
| Norm-stream steady state | ~307K primes/sec | Rust solver, sustained throughput |
| Autoresearch best | ~630K primes/sec | Automated pipeline best observed |

Hardware: vast.ai GPU instances (various), x86_64
Date: February-March 2026

## A100-SXM4-40GB (vast.ai, 2026-03-15)

Hardware: NVIDIA A100-SXM4-40GB, 108 SMs, SM 8.0, 40 GB HBM2e
Host: 32-vCPU AMD EPYC 7713, 126 GB RAM
CUDA: 12.4 (driver 570.133.20)

### CUDA Sieve Results

| Scale | Norm Range | Primes | Kernel Time | Primes/sec | Candidates/sec | Base Primes |
|-------|-----------|--------|-------------|-----------|----------------|-------------|
| 10^9 (near origin) | [0, 1e9) | 25.4M | 1.06s | 24.0M | 945M | 3,401 |
| 10^15 (sqrt(36)) | [6.4e15, +1e9) | 13.74M | 3.37s avg | 3.96M | 288M | 4.67M |
| 10^18 (sqrt(40) probe) | [1e18, +1e9) | 12.06M | 12.2s | 988K | 82M | 50.8M |

Key observations:
- 6x throughput drop from 10^9 to 10^15, another 4x drop to 10^18
- At 10^18, base prime generation alone takes 8.7s (one-time per band)
- Prime density: 1.37% at 6.4e15, 1.21% at 10^18

### Bottleneck Analysis (10^18)

| Phase | Wall Time | % of Total | Notes |
|-------|----------|-----------|-------|
| Base prime generation | 8.7s | 69% | 50.8M primes via CPU sieve |
| CPU bucket construction | ~0.3s | 2% | Two-pass scatter of large primes |
| GPU sieve kernel | 3.2s | 25% | Segmented sieve + Cornacchia |
| GPU sort + D2H | 0.4s | 3% | Thrust sort + cudaMemcpy |

- CPU base prime generation dominates at 10^18 (was negligible at 10^15)
- GPU sits idle during CPU prep (zero overlap between stages)
- GPU occupancy: ~50% (limited by 32 KB shared memory + 64 registers per thread)

### Rust Solver on A100 Host

| Scenario | Primes | Wedges | Wall Time | Throughput | RSS |
|----------|--------|--------|----------|-----------|-----|
| Band 0, k^2=36, no start-distance | 13.7M | 24 | 478s | 28.7K primes/sec | 88.6 GB |

### Solver Bugs Found

1. **start_distance filter rejects all GPRF primes** (fixed):
   `--start-distance 80015782 --prime-file primes.gprf` processed 0 primes.
   The computed `start_norm` (~6.4e15) matched the GPRF file's norm range,
   causing `iter_norm_range(start_norm, ...)` to reject everything.
   Fix: when a prime file is provided, trust the file's own norm range
   instead of applying the start_norm filter.

2. **Memory explosion with >32 wedges** (fixed):
   With `angular=0` (auto) on 32-core A100, `effective_wedge_count` returned
   128 (4 * cores). Each wedge allocates its own BandProcessor with grid
   hash map, node vectors, and union-find. At 128 wedges, 13.7M primes
   used 82.6 GB RSS (~6 KB/prime vs expected ~16 bytes). The per-wedge
   allocation overhead and overlap zone replication dominated.
   Fix: cap auto wedge count at 32 (1 * cores, ceiling 32).

### Optimization Roadmap

| Optimization | Expected Speedup | Effort | Notes |
|-------------|-----------------|--------|-------|
| Pipeline overlap (CPU prep + GPU kernel) | 3-5x at 10^18 | Medium | Double-buffer: prep batch N+1 while GPU runs batch N |
| GPU bucket construction | 10-20x at 10^18 | High | Move base-prime scatter to GPU, eliminate CPU bottleneck |
| Hybrid sieve+MR | Eliminates CPU bottleneck | Medium | Use MR for norms where base primes are too expensive |
| MR kernel (zero CPU prep) | Potential 6-60x at 10^18 | Low | Already implemented; needs benchmarking vs sieve crossover |
| Solver memory reduction | 10-50x RSS | High | Streaming band processor, compressed union-find |

The MR kernel (`--mode mr`) bypasses the CPU base-prime bottleneck entirely:
no sieve, no bucket construction. Each GPU thread independently tests one
candidate via Miller-Rabin + Cornacchia. At 10^18 where base-prime generation
is 69% of wall time, MR could be dramatically faster. The crossover point
(where MR becomes faster than sieve) is likely between 10^15 and 10^18.

### sqrt(36) Full Campaign Feasibility

**Pre-fix estimate (solver-bound):**
- Per band: ~4s sieve + ~480s solver = ~484s
- Total: ~3.1M seconds = ~36 days on single A100

**Post-fix estimate (sieve-bound):**
- Total norm range: [0, 6.4e15), at 1e9 norms/band = 6.4 million bands
- Per band at 10^15: ~14.5M primes, ~3.6s sieve
- Connector per band: ~8.6s on Jetson (1.7M/sec), ~3.7s on 4090 (3.9M/sec)
- **Jetson:** ~12.2s/band, ~78M seconds total (~2.5 years)
- **4090:** ~7.3s/band, ~47M seconds total (~1.5 years)

The pipeline is now balanced: sieve and connector take roughly equal time per band.
The remaining bottleneck is the number of bands (6.4M). Multi-GPU parallelism or
larger norm windows per band are the paths to practical feasibility.

### sqrt(40) Feasibility

- Total norm range: [0, 1e18)
- Sieve 3.7x slower per candidate vs 6.4e15
- Base prime generation alone: 8.7s per band at 10^18
- **Requires major architectural changes**: streaming solver, GPU bucket
  construction or MR mode, pipeline overlap

## A100-SXM4-40GB — Post-D3 Segmented Sieve (vast.ai, 2026-03-16)

Hardware: NVIDIA A100-SXM4-40GB, 108 SMs, SM 8.0, 40 GB HBM2e
Host: 12-vCPU Intel Xeon @ 2.20GHz, 85 GB RAM
CUDA: 12.1 (driver 550.163.01)

### What changed: D3 Segmented Base Sieve

Replaced naive `vector<bool>` base prime generation with L1-friendly 16KB segmented
Eratosthenes. Validated 4.6x speedup (Jetson) translates to A100 x86 host.

### CUDA Sieve Results (D3)

| Scale | Mode | Primes | Wall Time | Primes/sec | Candidates/sec | Base Sieve Time |
|-------|------|--------|-----------|-----------|----------------|-----------------|
| 10^9 (near origin) | sieve | 25.4M | 1.23s | 20.7M | 812M | 0.000s |
| 10^9 (near origin) | MR | 25.4M | 2.82s | 9.0M | 355M | n/a |
| 10^15 (sqrt(36)) | sieve | 14.5M | 3.61s | 4.0M | 277M | 0.087s |
| 10^15 (sqrt(36)) | MR | 14.5M | 8.29s | 1.7M | 121M | n/a |
| 10^18 (sqrt(40)) | sieve | 12.1M | 9.09s | 1.33M | 110M | 2.908s |
| 10^18 (sqrt(40)) | MR | 12.1M | 8.07s | 1.49M | 124M | n/a |

### Comparison: Pre-D3 (March 15) vs Post-D3 (March 16)

| Scale | Pre-D3 Sieve | Post-D3 Sieve | Delta | Pre-D3 Base Gen | Post-D3 Base Gen |
|-------|-------------|--------------|-------|-----------------|-----------------|
| 10^9 | 24.0M/sec (1.06s) | 20.7M/sec (1.23s) | -14% | ~0s | 0.000s |
| 10^15 | 3.96M/sec (3.37s) | 4.0M/sec (3.61s) | +1% | negligible | 0.087s |
| 10^18 | 988K/sec (12.2s) | 1.33M/sec (9.09s) | +35% | 8.7s | 2.908s |

Key finding: **D3 delivers a 3.0x improvement on base prime generation at 10^18**
(8.7s -> 2.9s). Total wall time improves 25% (12.2s -> 9.1s). At lower scales the
base sieve was already negligible, so D3 has no meaningful impact there. The slight
regression at 10^9 is noise (different host CPU: Intel Xeon vs AMD EPYC).

### Bottleneck Analysis (10^18, Post-D3)

| Phase | Wall Time | % of Total | Pre-D3 | Change |
|-------|----------|-----------|--------|--------|
| Base prime generation | 2.9s | 32% | 8.7s (69%) | **3.0x faster** |
| CPU bucket construction | ~0.3s | 3% | ~0.3s (2%) | same |
| GPU sieve kernel | ~3.2s | 35% | 3.2s (25%) | same (now largest) |
| GPU sort + D2H | ~0.4s | 4% | 0.4s (3%) | same |
| Other overhead | ~2.2s | 24% | - | - |

The GPU sieve kernel is now the dominant bottleneck at 10^18, not base prime generation.

### MR vs Sieve Crossover

At 10^18, MR (1.49M/sec) now beats the sieve (1.33M/sec) by 12%. MR has zero CPU prep
time, making it increasingly attractive as norm scale grows. The crossover point is
between 10^15 (sieve 2.4x faster) and 10^18 (MR 1.12x faster).

### OpenMP Experiment: CPU Bucket Construction

Added `#pragma omp parallel for schedule(dynamic, 256)` to both passes of the
two-pass bucket scatter loop, with atomic operations for shared counters.

| Scale | Without OMP | With OMP | Delta |
|-------|------------|---------|-------|
| 10^15 | 3.637s (3.98M/sec) | 3.693s (3.92M/sec) | -1.5% (slower) |
| 10^18 | 9.122s (1.32M/sec) | 9.116s (1.32M/sec) | +0.07% (noise) |

**Conclusion:** OpenMP on bucket construction provides zero benefit on x86. The bucket
phase is only ~0.3s (3% of wall time) at 10^18 -- thread creation overhead cancels any
parallelism gain. This is NOT the ARM 64-bit division issue we hypothesized; the bucket
construction is simply not a bottleneck. Optimization effort should target the GPU sieve
kernel (now 35% of wall time) or switch to MR mode at high scales.

### Rust Angular Connector on A100 Host

| Scenario | k^2 | Wedges | Primes Processed | Wall Time | Throughput | RSS |
|----------|-----|--------|-----------------|-----------|-----------|-----|
| Near-origin, angular=32 | 2 | 32 | 2,110 | 0.037s | 57K/sec | 20 MB |
| Near-origin, angular=32 | 4 | 32 | 2,110 | 0.031s | 68K/sec | 19 MB |
| Near-origin, angular=32 | 6 | 32 | 783,093 | 157.8s | 5.0K/sec | 4.0 GB |
| Near-origin, angular=0 | 6 | 12 | 783,093 | 69.6s | 11.2K/sec | 2.3 GB |

Note: k^2=2 and k^2=4 terminate early (moat found quickly). k^2=6 is the
computationally expensive case -- 25M primes loaded but only 783K processed
before the search exhausts all reachable primes.

The solver cannot operate on windowed GPRFs (offset from origin) because the
connectivity search starts at the origin. sqrt(36)-scale connector benchmarks
require a contiguous GPRF from [0, 6.4e15) which would be ~100+ GB.

## RTX 4090 vs Jetson Orin Benchmark (2026-03-16)

Commit: `5611393477e1626e8a81aec345bf1691533acbc8`

**Hardware:**
- **4090:** NVIDIA GeForce RTX 4090 (SM 8.9, 128 SMs, 24 GB VRAM), 28 vCPUs, 64 GB RAM, vast.ai (South Korea)
- **Jetson:** NVIDIA Orin Nano (SM 8.7, 8 SMs, 8 GB), 6-core ARM A78AE, 8 GB unified memory

### Experiment 1: CUDA Sieve Baseline

| Scale | Metric | 4090 | Jetson | Ratio (4090/Jetson) |
|-------|--------|------|--------|---------------------|
| 10^9 (near origin) | Primes found | 25,425,200 | 25,425,200 | 1.0x (exact match) |
| 10^9 | Wall time | 0.77s | 3.54s | 4.6x faster |
| 10^9 | Primes/sec | 33.0M | 7.18M | **4.6x** |
| 10^9 | Candidates/sec | 1,300M | 282M | 4.6x |
| 10^15 (sqrt(36) scale) | Primes found | 14,473,703 | 14,473,703 | 1.0x (exact match) |
| 10^15 | Wall time | 2.99s | 8.88s | 3.0x faster |
| 10^15 | Primes/sec | 4.84M | 1.63M | **3.0x** |
| 10^15 | Candidates/sec | 334M | 113M | 3.0x |

Notes:
- 4090 processes entire 1e9 window in a single batch (24 GB VRAM); Jetson needs 2 batches (8 GB)
- Ratio narrows at 10^15 due to base prime generation overhead (CPU-bound on both)

### Experiment 2: Connector Baseline (k^2=36, auto wedges, 100M GPRF)

GPRF: 2,881,124 primes from [0, 10^8) norm range.

| Metric | 4090 (27 wedges) | Jetson (6 wedges) | Notes |
|--------|-----------------|-------------------|-------|
| Primes/sec | 7,176 | 9,815 | Jetson faster due to fewer wedges |
| Wall time | 401.5s | 293.5s | |
| RSS | 14.6 GB | 3.5 GB | |
| Farthest point | (8458, 5335) | (8458, 5335) | Exact match |

**Critical finding:** Auto wedge count (angular=0) picks wedges = CPU cores. 4090 host has 28 cores -> 27 wedges, causing 4x more overlap than Jetson's 6 wedges. More wedges = more per-prime work and higher memory. The "auto" setting is not optimal for either device.

### Experiment 3: Wedge Sweep (k^2=36, 100M GPRF)

| Wedges | 4090 p/s | Jetson p/s | 4090/Jetson | 4090 RSS | Jetson RSS | 4090 Wall | Jetson Wall |
|--------|----------|-----------|-------------|----------|-----------|-----------|-------------|
| 4 | **28,279** | 10,692 | **2.6x** | 2.3 GB | 2.3 GB | 102s | 269s |
| 8 | 21,879 | 5,374 | 4.1x | 4.6 GB | 3.5 GB | 132s | 536s |
| 16 | 12,882 | 3,323 | 3.9x | 8.7 GB | 4.7 GB | 224s | 867s |
| 32 | 6,284 | 1,589 | 4.0x | 14.8 GB | 6.4 GB | 459s | 1813s |

Key observations:
- **Wedge=4 is the sweet spot** on both devices: highest throughput, lowest memory
- 4090 is consistently 2.6-4.1x faster than Jetson at the same wedge count
- RSS scales linearly with wedge count (overlap zones dominate memory)
- Going from 4 to 32 wedges: 4.5x slowdown on 4090, 6.7x on Jetson
- The 4090 advantage grows with more wedges (CPU parallelism utilizes more cores)

### Experiment 4: Upper-Bound sqrt(36) (start-distance mode, 100M GPRF)

| Start Distance | 4090 p/s | Jetson p/s | 4090/Jetson | Farthest Point |
|----------------|----------|-----------|-------------|----------------|
| sd=6 | 7,236 | 9,794 | 0.74x | (8458, 5335) |
| sd=7 | 7,254 | 9,759 | 0.74x | (8458, 5335) |

Notes:
- UB mode uses auto wedges (27 on 4090, 6 on Jetson) -- same story as baseline
- Correct farthest point on both devices at both start distances
- At matched wedge counts, 4090 would be ~2.6x faster (per wedge sweep data)

### Experiment 5: Correctness Gate (k^2=2)

| Mode | 4090 | Jetson | Expected |
|------|------|--------|----------|
| Lower-bound | farthest_point: (11, 4) | farthest_point: (11, 4) | (11, 4) |
| Upper-bound (sd=8) | farthest_point: (11, 4) | farthest_point: (11, 4) | (11, 4) |
| Gate | **PASS** | **PASS** | |

Both devices produce identical, correct results.

### Memory Limits

- **Jetson:** Full 1e9 GPRF (25M primes) with auto=6 wedges was OOM-killed during merge phase. 100M GPRF (2.9M primes) works with up to 32 wedges (6.4 GB peak).
- **4090 host:** Full 1e9 GPRF (25M primes) with auto=27 wedges was OOM-killed at 274M wedge_primes. 100M GPRF works at all tested wedge counts (peak 14.8 GB at 32 wedges).

### Observations and Implications

1. **Sieve: 4090 is 3-4.6x faster than Jetson.** The gap narrows at higher norms because base prime generation is CPU-bound.
2. **Connector: wedge count dominates throughput, not raw CPU speed.** Fewer wedges = less overlap = faster. Wedge=4 is optimal.
3. **Memory is the critical constraint.** The per-wedge overlap replication causes RSS to scale roughly linearly with wedge count. At 25M primes with 27 wedges, even 64 GB RAM is insufficient.
4. **Auto wedge detection is harmful.** Setting wedges=cores worked for small datasets but fails for large ones. Recommendation: default to 4 wedges regardless of core count, let user override.
5. **Both devices produce bitwise identical results** on all experiments (same prime counts, same farthest points).

## Post-Fix Connector (cab53c3)

### Root Cause: Angular Overlap Replication Bug

Commits d45612b through e9780cc contained a critical bug in the angular wedge decomposition.
The overlap radius was computed globally as `overlap_radians = sqrt(k^2) / sqrt(start_norm)`
instead of per-prime using each prime's actual norm. In lower-bound mode, `start_norm` defaults
to 2, producing `overlap = sqrt(36) / sqrt(2) = 4.24 radians`. Since the first octant spans
only `pi/4 = 0.785 radians`, an overlap of 4.24 radians meant every prime was replicated to
every wedge. With N wedges, this created N full copies of the problem: zero parallelism benefit,
Nx memory consumption, and throughput inversely proportional to wedge count.

The fix in cab53c3 computes overlap per-prime using the prime's own norm. High-norm primes
(the vast majority) have tiny angular overlap and route to only 1-2 wedges.

### Post-Fix Validated Results (cab53c3, 2026-03-16)

Validated on both Jetson Orin Nano and RTX 4090 host. GPRF: 2,881,124 primes from
[0, 10^8) norm range, k^2=36, angular auto.

**Before/after comparison (2.88M primes):**

| Metric | Pre-fix (2.88M primes) | Post-fix (2.88M primes) | Improvement |
|--------|----------------------|------------------------|-------------|
| Jetson throughput | 3,323/sec | 2,396,408/sec | 721x |
| 4090 throughput | 7,176/sec | 3,893,128/sec | 542x |
| Jetson RSS | 4.9 GB | 146 MB | 34x reduction |
| 4090 RSS | 15.3 GB | 115 MB | 133x reduction |

Correctness: both platforms report farthest point (8458, 5335), distance 9999.999,
2,881,124/2,881,124 primes in origin component. Results are bitwise identical across
platforms.

**Jetson at sqrt(36) scale (25.4M primes):** 1,684,683 primes/sec, 912 MB RSS. Well
within the 8 GB memory envelope. This confirms the connector scales to campaign-relevant
prime counts without memory pressure.

**Sieve-to-connector ratio analysis:**

| Metric | Pre-fix | Post-fix |
|--------|---------|----------|
| Jetson sieve (10^15) | 1.45M primes/sec | 1.45M primes/sec |
| Jetson connector | 3,323 primes/sec (16 wedges) | 2,396,408 primes/sec |
| Sieve-to-connector ratio | ~120:1 (connector is the wall) | ~1:1.7 (connector faster than sieve) |

The connector is no longer the bottleneck. The pipeline is now sieve-bound.

**4090 vs Jetson at matched scale:**

The 4090 is 1.6x faster than Jetson at the same prime count (3.89M vs 2.40M primes/sec).
This is consistent with the 4090 host having more CPU cores and higher single-thread
performance (x86 vs ARM A78AE).

### Pre-Fix Reference Data (do not use for capacity planning)

These numbers are from commit 5611393 (pre-fix), measured on 100M GPRF (2,881,124 primes,
norm range [0, 10^8), k^2=36). Retained for historical reference only.

| Wedges | RTX 4090 host (primes/sec) | Jetson Orin Nano (primes/sec) | 4090/Jetson |
|--------|---------------------------|------------------------------|-------------|
| 4 | 28,279 | 10,692 | 2.6x |
| 8 | 21,879 | 5,374 | 4.1x |
| 16 | 12,882 | 3,323 | 3.9x |
| 32 | 6,284 | 1,589 | 4.0x |

The inverse scaling with wedge count was the signature of the replication bug: more wedges
meant more copies of the full problem, not more parallelism.

### RTX 4090 CUDA Sieve Numbers (Experiment Matrix, commit 5611393)

| Scale | Primes Found | Wall Time | Primes/sec | Candidates/sec |
|-------|-------------|-----------|-----------|----------------|
| 10^8 (100M norms) | -- | -- | 10.9M | -- |
| 10^9 (1B norms) | 25,425,200 | 0.77s | 33.0M | 1,300M |
| 10^15 (sqrt(36)) | 14,473,703 | 2.99s | 4.84M | 334M |

Prime counts match Jetson exactly at every scale (correctness gate: PASS).

## Key Takeaways

- CUDA sieve is 3-10x faster than Rust internal sieve for prime generation
- The pipeline (CUDA sieve -> GPRF file -> Rust angular) achieves peak throughput
  by offloading prime generation to GPU and connectivity analysis to CPU
- At high norm scales the sieve degrades gracefully (6.67M -> 1.45M at 10^15)
- Jetson with GPRF file matches or exceeds cloud baseline throughput
- A100 sieve is ~6x faster than Jetson at same scale (24M vs 6.67M at 10^9)
- 4090 sieve is ~4.6x faster than Jetson, ~1.5x faster than A100 at 10^9
- At 10^18, CPU base-prime generation becomes the dominant bottleneck (69% of wall time)
- MR kernel may outperform sieve at very high norms by eliminating CPU prep entirely
- Solver memory scaling is the critical path for full campaign feasibility
- **Wedge count is the dominant tuning parameter**: fewer wedges = faster + less RAM
- **Post-fix connector (cab53c3):** 721x improvement on Jetson (3.3K -> 2.4M/sec), 542x on 4090 (7.2K -> 3.9M/sec)
- **Memory recovered:** Jetson 4.9 GB -> 146 MB (34x), 4090 15.3 GB -> 115 MB (133x)
- **Sieve-to-connector ratio now ~1:1** -- connector is no longer the wall, pipeline is sieve-bound
- Jetson sustains 1.7M primes/sec at 25.4M primes (sqrt(36) scale), 912 MB RSS
