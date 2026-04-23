---
title: CUDA v2 R85M Realistic Batch Kernel Profile
date: 2026-04-23
engine: codex
type: benchmark
status: complete
refs: [tiles-maxxing/cuda-campaign-v2-sqrt-36]
---

# CUDA v2 R85M Realistic Batch Kernel Profile

## Run Context

- Host: vast.ai `ssh -p 28412 root@ssh6.vast.ai`
- GPU: NVIDIA GeForce RTX 4090, driver 580.126.09, 24,564 MiB
- Build: `tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k36/campaign_main_cuda`
- Geometry: `K_SQ=36`, `R_inner=85,000,000`, `R_outer=85,008,192`, `--region full-octant`
- Active tiles: 8,677,267
- Output target: `/dev/null`
- Overflow counters: all zero in every run
- Verdict: `SPANNING` in every run
- Nsight Systems: `/opt/nvidia/nsight-compute/2024.1.1/host/target-linux-x64/nsys`

The old `pipeprofile` tmux session was killed before these runs. A fresh `pipeprofile` session produced the logs under `/workspace/profiles/2026-04-23-r85m-realistic/`.

## Throughput

Throughput below uses the shell wall timer around the full command, so it includes grid build, CUDA K1-K5, compositor ingest, and snapshot emission to `/dev/null`. The CUDA-only throughput uses the program's `cuda-k1-k5` timing.

| Chunk size (tiles) | Wall time (s) | End-to-end throughput (tiles/s) | CUDA K1-K5 time (s) | CUDA-only throughput (tiles/s) |
|---:|---:|---:|---:|---:|
| 50,000 | 108.655 | 79,861 | 89.496 | 96,957 |
| 100,000 | 108.819 | 79,740 | 89.657 | 96,783 |
| 200,000 | 109.063 | 79,562 | 89.641 | 96,800 |

Batch size from 50K to 200K did not materially change throughput. The standard 200K batch is within 0.4% of the 50K batch end-to-end, and within 0.2% CUDA-only.

## Kernel Breakdown

Command:

```bash
nsys profile --stats=true --force-overwrite=true \
  -o /workspace/profiles/2026-04-23-r85m-realistic/nsys_r85m_chunk_200000 \
  ./build-k36/campaign_main_cuda \
    --k-sq=36 \
    --r-inner=85000000 \
    --r-outer=85008192 \
    --region full-octant \
    --out /dev/null \
    --chunk-size=200000
```

Nsight application timing for this profiled run: `cuda-k1-k5=90.150s`, total command wall `114.187s`. Nsight kernel time sums to 87.537s across 521 chunks.

| Kernel | Total GPU time (s) | % of GPU kernel time | Instances | Avg per instance (ms) |
|---|---:|---:|---:|---:|
| `kernel_mr` | 34.048 | 38.9% | 521 | 65.352 |
| `kernel_face_encode_v2` | 25.133 | 28.7% | 521 | 48.239 |
| `kernel_uf_v2` | 14.436 | 16.5% | 521 | 27.707 |
| `kernel_sieve` | 12.832 | 14.7% | 521 | 24.630 |
| `kernel_face_sort_pack` | 0.632 | 0.7% | 521 | 1.213 |
| `kernel_compact` | 0.457 | 0.5% | 521 | 0.877 |

## Memory Transfer Summary

Nsight Systems did not report per-kernel memory throughput. It only reported aggregate CUDA memcpy summaries:

| Operation | Total time (ms) | Total size (MB) | Approx. transfer rate (GB/s) |
|---|---:|---:|---:|
| Device to host | 105.498 | 2,420.957 | 22.95 |
| Host to device | 11.060 | 208.789 | 18.88 |

Transfer time is not a bottleneck in this profile: combined GPU memcpy time is about 0.116s versus 87.537s of GPU kernel time.

## Bottleneck

The dominant bottleneck is compute inside `kernel_mr`, which accounts for 38.9% of GPU kernel time. The second bottleneck is `kernel_face_encode_v2` at 28.7%. Together they account for 67.6% of GPU kernel time.

`kernel_uf_v2` and `kernel_sieve` are still material, but optimizing either first has a lower ceiling than MR or face encoding. `kernel_face_sort_pack`, `kernel_compact`, and host/device transfer overhead are not first-order targets.

## Recommendation

Prioritize the MR hot path first. It has the largest single-kernel share and is larger than `kernel_face_encode_v2` by about 8.9 seconds over this run. Use Nsight Compute next on `kernel_mr` to separate integer instruction throughput, register pressure, occupancy, and memory effects before making register-tuning changes.

Second priority is the `kernel_face_encode_v2` rewrite or specialization. It is large enough that a structural improvement could move total throughput, but the MR path is the clearer first target from this profile.

Do not prioritize generic register tuning without Nsight Compute evidence. Nsight Systems identifies where time is spent, but not whether register count is the limiter.
