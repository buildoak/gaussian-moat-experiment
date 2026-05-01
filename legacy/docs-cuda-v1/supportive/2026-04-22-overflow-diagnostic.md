---
title: R80M CUDA Overflow Diagnostic
date: 2026-04-22
engine: codex
type: investigation
status: complete
refs: [tiles-maxxing/cuda-campaign-v2-sqrt-36/src/host_driver.cpp]
---

# R80M CUDA Overflow Diagnostic

## Run

- Instance: vast.ai ID 35423989, RTX 4090
- Build: `K_SQ=36`, `CMAKE_CUDA_ARCHITECTURES=89`
- Command: `./campaign_main_cuda --k-sq=36 --r-inner=80000000 --r-outer=80008192 --region full-octant --out /root/overflow_diag_R80M.snapshot.bin --chunk-size=200000`
- Result: `RUN_EXIT:0`, `VERDICT: SPANNING`
- Active tiles: 8,166,667
- CUDA K1-K5 time: 117.899 s
- Total time: 209.912 s

## Overflow Counters

| Counter | Count | Rate |
| --- | ---: | ---: |
| `k1_cand_overflow_count` | 0 | 0.0000% |
| `k4_prime_overflow_count` | 0 | 0.0000% |
| `k4_group_overflow_count` | 427,297 | 5.2322% |
| `k5_port_overflow_count` | 0 | 0.0000% |

All observed overflow is K4 group remap overflow. K1 candidate capacity and K3/K4 prime capacity are not the source in this run.

## First 10 Overflow Tiles

| Tile `(i,j)` | K1 candidates | K3 primes | K4 group count | K5 ports `[I,O,L,R]` | Type |
| --- | ---: | ---: | ---: | --- | --- |
| `(0,312509)` | 5,605 | 2,364 | 128 | `[0,0,0,0]` | `k4_group` |
| `(0,312510)` | 5,626 | 2,415 | 128 | `[0,0,0,0]` | `k4_group` |
| `(0,312520)` | 5,649 | 2,447 | 128 | `[0,0,0,0]` | `k4_group` |
| `(1,312505)` | 5,560 | 2,424 | 128 | `[0,0,0,0]` | `k4_group` |
| `(1,312507)` | 5,575 | 2,483 | 128 | `[0,0,0,0]` | `k4_group` |
| `(1,312510)` | 5,612 | 2,497 | 128 | `[0,0,0,0]` | `k4_group` |
| `(1,312520)` | 5,582 | 2,431 | 128 | `[0,0,0,0]` | `k4_group` |
| `(2,312528)` | 5,589 | 2,510 | 128 | `[0,0,0,0]` | `k4_group` |
| `(4,312506)` | 5,523 | 2,501 | 128 | `[0,0,0,0]` | `k4_group` |
| `(5,312504)` | 5,633 | 2,523 | 128 | `[0,0,0,0]` | `k4_group` |

## Notes

The reported K4 group count is `128` on these tiles because `kernel_uf_v2` sets overflow when dense remap needs label `129`; the exported `max_label` value saturates at the last assigned valid label. The instrumentation therefore classifies K4 group overflow from the K4 remap overflow flag after excluding K1 and prime-count overflow.

K5 port counts are zero for these tiles because K5 receives the K4 remap overflow flag and emits an overflow TileOp before face extraction/packing.
