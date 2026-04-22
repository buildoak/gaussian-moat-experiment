---
title: RTX 4090 K36 Moat Boundary Sweep
date: 2026-04-22
engine: codex
type: report
status: complete
refs: [tiles-maxxing/cuda-campaign-v2-sqrt-36, methodology/lemmas_v2/campaign-blueprint.md]
---

# RTX 4090 K36 Moat Boundary Sweep

## Setup

- Instance: vast.ai ID 35378303, `ssh8.vast.ai:18302`
- GPU: NVIDIA GeForce RTX 4090, compute capability 8.9
- Code: commit `92b3c9a` (`Conservatively propagate K1 sieve overflow`)
- Build: `cmake -S . -B build-k36-sm89 -DK_SQ=36 -DCMAKE_CUDA_ARCHITECTURES=89 -DCMAKE_BUILD_TYPE=Release`
- Campaign command shape:
  `campaign_main_cuda --k-sq=36 --r-inner 80000000 --r-outer <R_outer> --region full-octant --chunk-size 200000`
- Overflow count method: generated snapshot scan over TileOp byte 228, counting `OVERFLOW_BIT`.

## Build And Tests

Build completed successfully with CUDA 12.4.131 and GCC 11.4.0.

`ctest --test-dir build-k36-sm89 --output-on-failure` passed 10/10 tests, including `test_k1_overflow_spanning`.

## Sweep Results

| R_inner | R_outer | Verdict | Overflow TileOps | Active tiles | CUDA K1-K5 (s) | Compositor (s) | Snapshot (s) | Total (s) |
|---:|---:|---|---:|---:|---:|---:|---:|---:|
| 80,000,000 | 80,008,192 | SPANNING | 427,297 | 8,166,667 | 97.078 | 9.228 | 5.055 | 113.436 |
| 80,000,000 | 80,010,000 | SPANNING | 524,727 | 9,900,665 | 118.224 | 11.217 | 6.202 | 137.834 |
| 80,000,000 | 80,012,000 | SPANNING | 632,736 | 11,818,291 | 141.520 | 13.442 | 7.545 | 164.829 |
| 80,000,000 | 80,014,000 | SPANNING | 740,836 | 13,736,240 | 164.915 | 15.600 | 8.663 | 191.580 |
| 80,000,000 | 80,015,000 | SPANNING | 794,745 | 14,695,213 | 176.465 | 16.728 | 9.205 | 204.846 |
| 80,000,000 | 80,015,500 | SPANNING | 821,771 | 15,174,755 | 182.340 | 17.268 | 9.617 | 211.707 |
| 80,000,000 | 80,015,782 | SPANNING | 836,896 | 15,444,921 | 185.706 | 17.613 | 9.766 | 215.590 |

## Result

The requested verification gate did not pass.

- `R_outer=80,008,192` returned `SPANNING`, as expected for the R=80M checkpoint.
- The same run had `overflow_count=427,297`, not the required `overflow_count=0`.
- Every boundary sweep radius, including `R_outer=80,015,782`, returned `SPANNING` with a large overflow count.

Because overflow TileOps force a conservative SPANNING result, this sweep cannot locate a trustworthy SPANNING-to-MOAT transition. The boundary result at `80,015,782` is not usable as evidence against the Tsuchimura moat boundary; it is overflow-contaminated.

## Unexpected

The new K1 overflow regression test passes, but production full-octant R=80M still emits hundreds of thousands of overflow TileOps. This means the fix is conservative, but the production capacity/overflow path is still active at the checkpoint where the gate expected zero overflow.
