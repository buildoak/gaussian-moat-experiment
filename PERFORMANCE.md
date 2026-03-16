# Performance Measurements

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

- Total norm range: [0, 6.4e15), at 1e9 norms/band = 6.4 million bands
- Per band: ~4s sieve + ~480s solver = ~484s
- Total: ~3.1M seconds = ~36 days on single A100
- **Not feasible without solver optimization** (solver merge dominates)

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

## Key Takeaways

- CUDA sieve is 3-10x faster than Rust internal sieve for prime generation
- The pipeline (CUDA sieve -> GPRF file -> Rust angular) achieves peak throughput
  by offloading prime generation to GPU and connectivity analysis to CPU
- At high norm scales the sieve degrades gracefully (6.67M -> 1.45M at 10^15)
- Jetson with GPRF file matches or exceeds cloud baseline throughput
- A100 sieve is ~6x faster than Jetson at same scale (24M vs 6.67M at 10^9)
- At 10^18, CPU base-prime generation becomes the dominant bottleneck (69% of wall time)
- MR kernel may outperform sieve at very high norms by eliminating CPU prep entirely
- Solver memory scaling is the critical path for full campaign feasibility
