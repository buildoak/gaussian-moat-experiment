---
title: CUDA Kernel Optimization Session — 1,102 to 2,818 tiles/s
date: 2026-04-10
engine: claude
type: report
status: complete
refs:
  - tile-cuda/src/tile_kernel.cu
  - tile-cuda/include/gpu_face_encode.cuh
  - tile-cuda/include/gpu_sieve.cuh
  - tile-cuda/include/gpu_math.cuh
  - tile-cuda/include/gpu_constants.cuh
  - tile-cuda/src/main.cu
  - docs/supportive/2026-04-10-profiling-baseline.md
---

# CUDA Kernel Optimization Session

**Session:** a913f1dc
**Date:** 2026-04-10
**Duration:** 4h 56m
**Hardware:** Jetson Orin Nano (SM 8.7, 8 SMs, 8 GB)
**HEAD commit:** 5997aa16ed8255de9f49fc9daf8e66b7e57d498f

## Summary

Four architectural rewrites to the single-tile CUDA kernel, measured at 1,000 tiles
at origin (608000000, 608000000). Throughput: 1,102 --> 2,818 tiles/s (2.56x improvement).
Mac Mini 12-core CPU baseline is ~1,000 tiles/s, making the final Jetson number ~2.8x
the CPU reference.

## Starting State

- 1,102 tiles/s on Jetson (first stable multi-tile benchmark after buffer fixes)
- 4 blocks/SM, 75% occupancy, 46 regs/thread
- Phase 4/5 (face extraction + port encoding) consumed 44% of cycles
- Phase 1c (Miller-Rabin) consumed 26%
- Phase 1b (double-pass sieve scatter) consumed 12%
- MAX_PRIMES_GPU=2560, FACES_PER_PASS=2 (already landed before profiling)

## Optimization 1: 2-Face-Per-Pass Extraction

**Commit:** `75abb7c` — feat(tile-cuda): 2-face-per-pass extraction, N=2560 for 4-block occupancy
**What:** Replaced 4-face slot table with FACES_PER_PASS=2 algorithm. Reduced shared memory
footprint to fit 4 blocks/SM. Set MAX_PRIMES_GPU=2560.
**Why:** Original 4-face extraction required too much shared memory, limiting occupancy to
2 blocks/SM. The 2-face-per-pass approach halves shared memory while preserving correctness.
**Impact on occupancy:** Recovered 4 blocks/SM (75% occupancy, up from ~37%).
**Throughput:** Established the 1,102 tiles/s baseline after buffer fixes in `b5aeeb4`.

Supporting commits:
- `e0abe49` — reduce MAX_PRIMES_GPU from 3072 to 2304 (pre-occupancy tuning)
- `2044250` — parallelize face extraction to remove per-thread stack spills
- `6f9a233` — benchmark harness with warmup, batched dispatch, occupancy reporting
- `b5aeeb4` — fix multi-tile benchmark OOB in batch launch path

## Optimization 2: Per-Prime Face Extraction (O(P) Algorithm)

**Commit:** `7c0e2d1` — perf(tile-cuda): per-prime face extraction, O(P) replacing O(B*P) brute force
**What:** Replaced brute-force CUDA face extraction with the per-prime algorithm from the
C++ reference. Scans primes once, builds a compact face-prime list, sorts by h coordinate,
and detects ports with the same gap-test logic as C++.
**Why:** Profiling showed phase 4/5 at 44% of cycles. The brute-force approach iterated
all boundary cells for each prime. The per-prime approach is O(P) -- each prime generates
at most 2 face entries per pass.
**Phase 4/5 cycles:** 44% --> ~2% of total
**Throughput:** 1,102 --> 1,538 tiles/s (+40%)
**Side effects:** 4 blocks/SM preserved, shared memory slightly reduced. Bit-identical
TileOps on canonical smoke tiles.

## Optimization 3: Single-Pass Sieve + Barrett Reduction

**Commit:** `0e7c1dc` — perf(tile-cuda): Barrett sieve reduction + single-pass atomicAdd scatter
**What:** Two changes in one commit:
1. Replaced double-pass count-then-scatter sieve with single-pass atomic reservation
   (`atomicAdd` on a shared counter, direct write to candidate buffer).
2. Applied Barrett reduction to all modular arithmetic in the sieve path, eliminating
   expensive `__cuda_sm20_rem_u64` library calls.
**Why:** Phase 1b (scatter) was 12% of cycles and had 13x variance (908K to 11.7M cycles).
The double pass was architecturally wasteful. Barrett reduction targets the ~4,900
`euclidean_mod_gpu()` calls per `sieve_row()` invocation.
**Phase 1a+1b combined:** Eliminated scatter phase entirely; sieve is now single-pass.
**Throughput:** 1,538 --> 1,739 tiles/s (+13%)
**Registers:** Dropped from 46 to 43/thread. 4 blocks/SM preserved.

## Optimization 4: Sinclair 7-Base MR Witnesses + Trial Division Removal

**Commit:** `5997aa1` — perf(tile-cuda): Sinclair 7-base MR witnesses, delete redundant trial division
**What:** Two changes:
1. Replaced the incorrect Jiang/Deng witness set with the verified Sinclair 7-base
   deterministic set: {2, 325, 9375, 28178, 450775, 9780504, 1795265022}.
   Deterministic for all n < 2^64.
2. Removed redundant trial division in the MR path. Sieve survivors have already been
   tested against small primes during the sieve phase; re-testing in MR is wasted work.
**Why:** The earlier 7-witness Jiang/Deng claim was wrong for this range -- a latent
correctness bug. The Sinclair set is the verified replacement. Trial division removal
is safe because the sieve already excludes composites divisible by small primes.
**Phase 1c cycles (mean):** 11.38M --> 5.53M (2.06x reduction)
**Throughput:** 1,739 --> 2,818 tiles/s (+62%)
**Registers:** 43/thread, 4 blocks/SM preserved.

## Final Kernel Configuration

| Parameter | Value |
|-----------|-------|
| BLOCK_THREADS | 288 |
| Registers/thread | 43 |
| Blocks/SM | 4 |
| Occupancy | 75% (1,152/1,536 threads) |
| MAX_PRIMES_GPU | 2560 |
| MAX_CANDIDATES_GPU | 6144 |
| FACES_PER_PASS | 2 |
| MR witnesses | Sinclair 7-base: {2, 325, 9375, 28178, 450775, 9780504, 1795265022} |
| MR trial division | Removed (sieve handles small factors) |
| Sieve layout | Single-pass atomic scatter + Barrett reduction |
| Face extraction | Per-prime O(P), 2 faces per pass |

## Performance Arc

| State | Throughput | Delta | Commit |
|-------|-----------|-------|--------|
| Post-occupancy recovery + buffer fix | 1,102 tiles/s | baseline | 75abb7c + b5aeeb4 |
| + per-prime face extraction | 1,538 tiles/s | +40% | 7c0e2d1 |
| + Barrett sieve + single-pass | 1,739 tiles/s | +13% | 0e7c1dc |
| + Sinclair witnesses + no trial div | 2,818 tiles/s | +62% | 5997aa1 |
| **Total improvement** | **2.56x** | | |

## Known Issues

- **178 cross_compare mismatches** on a broader 1K-tile validation set. Pre-existing,
  not a regression from this session. The trace indicates a harness/domain mismatch
  rather than a kernel correctness bug. Canonical smoke tiles still match byte-for-byte
  between CUDA and C++ reference.

## Next Optimization Targets

1. **Phase 3 union-find.** Now the next largest non-MR slice after face extraction and
   sieve were optimized. CAS atomics and thread utilization are the investigation axes.

2. **Hash-based MR witness selection (Forisek/Jancina).** PAPER-OPS-028 dispatched for
   extraction. A hash-selected single witness could replace the 7-base set for most
   candidates, dropping MR cost by up to 7x for the common case.

3. **CPU/GPU overlap.** Host-side tile coordinate generation and result readback can
   overlap with kernel execution. Double-buffering the tile coordinate array would
   hide transfer latency.

4. **Further sieve refinement.** Barrett precomputation is currently per-launch on host.
   Caching across tiles with the same base-prime set could eliminate redundant work.

## Commit Log (Session Scope)

```
6f9a233 2026-04-10 16:25 tile-cuda: add benchmark harness with warmup, batched dispatch, and occupancy reporting
2044250 2026-04-10 16:49 Parallelize face extraction to remove per-thread stack spills
e0abe49 2026-04-10 17:27 tile-cuda: reduce MAX_PRIMES_GPU from 3072 to 2304
75abb7c 2026-04-10 18:13 feat(tile-cuda): 2-face-per-pass extraction, N=2560 for 4-block occupancy
b5aeeb4 2026-04-10 18:50 fix(tile-cuda): multi-tile benchmark OOB in batch launch path
7c0e2d1 2026-04-10 20:59 perf(tile-cuda): per-prime face extraction, O(P) replacing O(B*P) brute force
5c1dfb0 2026-04-10 21:04 docs: profiling baseline, optimization proposals, census tools
0e7c1dc 2026-04-10 21:20 perf(tile-cuda): Barrett sieve reduction + single-pass atomicAdd scatter
5997aa1 2026-04-10 22:53 perf(tile-cuda): Sinclair 7-base MR witnesses, delete redundant trial division
```
