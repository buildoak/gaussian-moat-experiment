---
title: CUDA v2 GPU Slab Size Budget Experiment
date: 2026-04-23
engine: codex
type: benchmark
status: complete
refs: [tiles-maxxing/cuda-campaign-v2-sqrt-36/include/cuda_campaign/host_driver.h, tiles-maxxing/cuda-campaign-v2-sqrt-40/include/cuda_campaign/host_driver.h]
---

# CUDA v2 GPU Slab Size Budget Experiment

## Run Context

- Host: vast.ai instance `35468412`, `ssh -p 28412 root@ssh6.vast.ai`
- GPU: NVIDIA GeForce RTX 4090, driver `580.126.09`, 24,564 MiB VRAM
- Build: `tiles-maxxing/cuda-campaign-v2-sqrt-36`, top-level remote build dir `/workspace/gaussian-moat-cuda/build-k36`
- Geometry: `K_SQ=36`, `R_inner=85,000,000`, `R_outer=85,008,192`, `--region full-octant`
- Active tiles: 8,677,267
- Output target: `/dev/null`
- Chunk-size for the budget table: `--chunk-size=500000`
- Overflow counters: all zero in every run
- Verdict: `SPANNING` in every run

The hardcoded `device_budget_bytes` default was found in both:

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/include/cuda_campaign/host_driver.h`
- `tiles-maxxing/cuda-campaign-v2-sqrt-40/include/cuda_campaign/host_driver.h`

The remote source was temporarily patched to print dispatch stats (`dispatch-chunks`, `dispatch-slabs`, `device-slab-tiles`, peak byte counters), and restored after the run. No code changes were committed.

## Results

Delta uses CUDA-only throughput relative to the 8.5 GB / 500K chunk-size row.

| Budget (GB) | Slab size | Num slabs | CUDA time (s) | Throughput (tiles/s) | Delta vs baseline |
|---:|---:|---:|---:|---:|---:|
| 8.5 | 16,783 | 521 | 89.764 | 96,668 | baseline |
| 12 | 19,972 | 451 | 89.285 | 97,186 | +0.54% |
| 16 | 12,074 | 729 | 93.226 | 93,078 | -3.71% |
| 20 | 4,168 | 2,083 | 102.876 | 84,347 | -12.75% |

Chunk-size sanity check:

| Budget (GB) | Chunk size | Dispatch chunks | Slab size | Num slabs | CUDA time (s) | Throughput (tiles/s) |
|---:|---:|---:|---:|---:|---:|---:|
| 8.5 | 200,000 | 44 | 16,783 | 521 | 89.398 | 97,063 |
| 8.5 | 500,000 | 18 | 16,783 | 521 | 89.764 | 96,668 |

Raising `--chunk-size` from 200K to 500K did not increase slab size or reduce slab count at the current 8.5 GB budget. Chunk-size is not the active cap for these runs.

## Interpretation

Bigger budget did not monotonically improve throughput. The best tested CUDA-only throughput was the 12 GB row, but the gain over 8.5 GB was only about 0.5% with 500K chunks, and only about 0.1% compared with the 8.5 GB / 200K continuity run.

The 16 GB and 20 GB rows regressed because of current slab sizing behavior. `dispatch_tile_batch()` computes an initial `device_slab_capacity`, allocates `DeviceWorkspace` at that capacity, then calls `device_slab_tiles_for()` again inside the slab loop. That second call sees reduced free VRAM after the workspace allocation and clamps the actual slab size downward. At high budgets, the first allocation consumes enough VRAM that the later per-slab recomputation becomes smaller than the original 8.5 GB cap.

## Recommendation

Do not raise the default to 16 GB or 20 GB under the current allocator logic. Among the tested values, 12 GB is the best setting, but the gain is too small to justify changing the default without first fixing or simplifying the slab sizing policy.

If this path is tuned further, the next experiment should hold the computed slab capacity stable for the whole dispatch after workspace allocation, or separate "workspace allocation capacity" from "actual per-dispatch slab size" so larger budgets cannot self-throttle through the second `cudaMemGetInfo()` query.
