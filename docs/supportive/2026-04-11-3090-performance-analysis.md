---
title: RTX 3090 Performance Analysis — Multi-Kernel Pipeline Baseline
date: 2026-04-11
engine: coordinator
type: benchmark
status: complete
refs:
  - tile_cuda_multi_kernel/src/main.cu
  - tile_cuda_multi_kernel/src/kernel_mr.cu
  - tile_cuda_multi_kernel/src/kernel_sieve.cu
  - tile_cuda_multi_kernel/src/kernel_uf.cu
  - tile_cuda_multi_kernel/src/kernel_face_encode.cu
  - tile_cuda_multi_kernel/src/kernel_compact.cu
  - docs/supportive/2026-04-10-multi-kernel-architecture.md
  - docs/supportive/2026-04-10-profiling-baseline.md
---

# RTX 3090 Performance Analysis

**Date:** 2026-04-11
**Hardware:** RTX 3090 (SM 8.6, Ampere GA102, 82 SMs, 10,496 CUDA cores)
**Baseline reference:** Jetson Orin Nano (SM 8.7, 8 SMs, 3,431 tiles/s at 2000 tiles)
**Pipeline:** 5-kernel multi-kernel architecture (K1 Sieve -> K2 MR -> K3 Compact -> K4 UF -> K5 FaceEncode)

This document captures the RTX 3090 baseline results for the multi-kernel pipeline and the analysis that informed the decision to optimize on RTX 4090.

## 1. Throughput Scaling by Batch Size

Benchmark at origin (608000000, 608000000), RTX 3090:

| Batch Size | tiles/s | ms/tile | Scaling vs Jetson |
|-----------|---------|---------|-------------------|
| 100 | 26,675 | 0.0375 | 7.8x |
| 2,000 | 66,828 | 0.0150 | 19.5x |
| 5,000 | 69,802 | 0.0143 | 20.3x |
| 10,000 | 78,984 | 0.0127 | 23.0x |
| 20,000 | 72,290 | 0.0138 | 21.1x |

Jetson Orin Nano baseline: 3,431 tiles/s at 2,000 tiles (register-tuned multi-kernel pipeline).

The 3090 reaches ~20x Jetson throughput, not the ~10x that SM count alone (82/8 = 10.25x) would predict. The excess comes from higher clock (~1.8 GHz vs ~1.0 GHz), wider memory bus (384-bit GDDR6X vs 64-bit LPDDR5), and more L2 cache (6 MB vs 1 MB).

## 2. Per-Kernel Breakdown (2,000 tiles)

| Kernel | 3090 (ms) | Jetson (ms) | Speedup | 3090 % | Jetson % |
|--------|-----------|-------------|---------|--------|----------|
| K1 Sieve | 3.75 | 98.2 | 26.2x | 12.5% | 16.4% |
| K2 MR | 18.6 | 323.9 | 17.4x | 62.2% | 54.0% |
| K3 Compact | 0.11 | 1.9 | 17.3x | 0.4% | 0.3% |
| K4 UF | 6.5 | 161.5 | 24.8x | 21.8% | 26.9% |
| K5 FaceEncode | 0.95 | 14.6 | 15.4x | 3.2% | 2.4% |
| **Total** | **29.9** | **600.1** | **20.1x** | | |

K2 MR remains the dominant bottleneck. Its share increases from 54% on Jetson to 62% on 3090 because K1 and K4 scale better (26.2x and 24.8x respectively, vs K2's 17.4x).

## 3. Occupancy on 3090

| Kernel | Regs/Thread | Blocks/SM | Occupancy |
|--------|-------------|-----------|-----------|
| K1 Sieve | 30 | 5 | 93.8% |
| K2 MR | 44 | 4 | 75.0% |
| K3 Compact | 21 | 5 | 93.8% |
| K4 UF | 34 | 5 | 93.8% |
| K5 FaceEncode | 40 | 5 | 93.8% |

K2 MR achieves 4 blocks/SM on 3090, up from 3 on Jetson. The 3090's larger register file (65,536 regs/SM vs Orin's 65,536 but with different allocation granularity) allows one more concurrent block. The 44-register cap was originally Orin-tuned; 5 blocks/SM would be possible at <=44 regs on the 3090 but K2's natural register usage prevents it.

## 4. INT32 Scaling Logic

The workload scales with INT32 throughput, not FP32 TFLOPS. This is the key insight for GPU selection.

**Why INT32 dominates:** Montgomery multiplication uses `__umul64hi` which decomposes to 4x `IMAD.WIDE.U32` (schoolbook 2x2 with 32-bit limbs). There is no native INT64 multiply on sm_86, sm_87, or sm_89. Every 64-bit multiply in `mont_mul_gpu` — and there are 5 per Montgomery multiplication — becomes 4 integer multiply-add instructions on the INT32 pipeline.

**INT32 pipeline structure:** Both Ampere GA102 (3090) and Ada AD102 (4090) split their 128 CUDA cores/SM as 64 FP32-only + 64 FP32/INT32 dual-use units. Only 64 INT32 ops/SM/cycle are available regardless of FP32 load.

**INT32 throughput by GPU:**

| GPU | SMs | INT32/cycle/SM | Clock (boost) | INT32 TOPS |
|-----|-----|---------------|---------------|------------|
| RTX 3090 | 82 | 64 | ~1.8 GHz | ~9.4 |
| RTX 4090 | 128 | 64 | ~2.4 GHz | ~19.6 |

Expected 4090 scaling: ~2.1x raw INT32 advantage over 3090. With better L2 caching (see section 7), effective speedup may exceed 2.1x.

## 5. GPU Economics (tiles/s per dollar)

| GPU | Est. tiles/s | vast.ai $/hr | tiles/s per $ |
|-----|-------------|-------------|---------------|
| RTX 3090 | 66,800 | $0.12 | 557k |
| RTX 4090 | ~138k (est.) | $0.23 | 600k |
| A100 40GB | ~72k (est.) | $0.29 | 249k |
| H100 SXM | ~115k (est.) | $1.55 | 74k |

Consumer GPUs (3090/4090) dominate for integer-heavy workloads. Datacenter GPUs charge premium pricing for HBM bandwidth, tensor cores, and NVLink interconnect — none of which are used by the Montgomery MR pipeline. The A100's INT32 throughput is comparable to the 3090 (108 SMs x 64 INT32/cycle x ~1.4 GHz = ~9.7 TOPS) but at 2.4x the rental cost. The H100 adds more SMs and higher clocks but at 13x the cost of a 3090.

The 4090 is the sweet spot: highest INT32 throughput per dollar, with a 72 MB L2 cache bonus.

## 6. Per-Kernel Scaling Analysis

Why different kernels scale differently from Jetson to 3090:

**K1 Sieve (26.2x):** Barrett reduction is simpler ALU work with short dependency chains. Scales near-linearly with SM count (10.25x) and clock (1.8x), giving ~18x from compute alone. The extra ~1.5x likely comes from the 3090's higher memory bandwidth (936 GB/s vs Orin's ~68 GB/s) benefiting candidate buffer writes and constant memory reads.

**K2 MR (17.4x):** Compute-bound but has deep dependency chains in `mont_mul_gpu` (5 multiplies with carry propagation). Each Montgomery multiplication is a serial chain: `lo = a*b`, `hi = umul64hi(a,b)`, `q = lo*m_inv`, `qm_lo = q*m`, `qm_hi = umul64hi(q,m)`, then add+correct. Cannot fully utilize wide SM dispatch width. Scales with SM count x clock but limited by ILP within each warp.

**K4 UF (24.8x):** The 288-thread parallel `atomicCAS` approach benefits from more SMs reducing contention per SM. With 82 SMs, fewer blocks compete on the same parent array compared to 8 SMs. The contention reduction is super-linear: expected CAS retries drop as contention per SM drops.

**K5 FaceEncode (15.4x):** Complex serial control flow within each block limits scaling. Face extraction uses lane-0 serial port scanning and per-face sorting. Per-block work is constant regardless of SM count — the speedup comes purely from running more blocks in parallel (SM count x clock), with no architectural bonus.

## 7. Key Observations

**K2 MR dominates MORE on 3090 (62% vs 54%).** The other kernels scale better than K2, concentrating the remaining bottleneck in Montgomery multiplication. Optimizing K2 has even higher leverage on 3090/4090 than it did on Jetson.

**L2 cache disparity.** The 3090's L2 cache is 6 MB. The 19 MB bitmap buffer for 2,000 tiles does not fit, forcing DRAM reads for K3 (compact) and K4 (UF) bitmap access. The 4090's L2 cache is 72 MB — the bitmap fits entirely, providing a free latency win for K3 and K4 without code changes.

**Batch size saturation.** At 100 tiles, only 100 blocks across 82 SMs — most SMs run a single block, leaving occupancy at ~20% effective. Throughput climbs 3x from batch=100 to batch=2000, then plateaus. Optimal regime is 10k+ tiles where all SMs are fully occupied across all 5 kernel launches.

**10k to 20k throughput drop.** The 78,984 -> 72,290 tiles/s regression at batch=20k may be measurement noise or L2 cache pressure. At 20k tiles the bitmap buffer is 190 MB, well beyond L2, and the candidate buffer is 480 MB — approaching the 3090's 24 GB VRAM but creating significant DRAM traffic. Further investigation needed to distinguish noise from cache effects.

## 8. Identified Optimization Targets

Ordered by estimated leverage on 3090/4090:

### Target 1: Remove redundant `a % ctx.m` in `mont_to_gpu` (gpu_math.cuh)

Both MR witnesses are always < m (max witness value in FJ64 table is uint16_t, min norm after sieve > 9,409). The modular reduction is a no-op that costs 2x `rem_u64` per candidate. Estimated savings: ~5% of K2 time.

### Target 2: Register re-sweep on target hardware

The 44-register cap for K2 was Orin-tuned. On 3090 with different register file partitioning and SM architecture, the optimal cap may differ. Testing 40, 44, 48, 52, and uncapped to find the 3090 sweet spot. On 4090, repeat the sweep — the larger SM count may favor higher occupancy (lower reg count) over ILP (higher reg count).

### Target 3: Replace `mont_compute_r2` 128-step loop

The R^2 computation uses 128 iterations of `addmod_gpu` (doubling). Replace with `neg_mod` + 1 `mont_mul`: compute `r0 = (0 - m) % 2^64` (one subtract), then `r2 = mont_mul(r0, r0)`. Saves ~128 addmod iterations per candidate. Estimated savings: ~2% of K2 time.

### Target 4: Pipeline overlap — K2 batch N || K4/K5 batch N-1

K2 is purely compute-bound. K4/K5 of the previous batch are independent once K3 completes. Multi-stream pipelining would overlap K2 execution with K4+K5, hiding the 25% non-MR tail. Requires double-buffering inter-kernel buffers and a two-stream launch schedule.

### Target 5: L2 persistence hints for FJ64 table on 4090

The FJ64 lookup table (512 KB) should remain L2-resident across kernel launches. On 4090 with 72 MB L2, use `cudaAccessPolicyWindow` or `__ldg` hints to pin the table in L2, avoiding cold-start misses on each K2 launch. The 3090's 6 MB L2 already keeps the table resident naturally since 512 KB << 6 MB.
