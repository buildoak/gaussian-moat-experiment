---
title: R=85M CUDA v2 Profiling
date: 2026-04-23
engine: codex
type: benchmark
status: complete
refs: [tiles-maxxing/cuda-campaign-v2-sqrt-36]
---

# R=85M CUDA v2 Profiling

## Scope

Mission target was `K_SQ=36`, `R_inner=80,000,000`, `R_outer=85,000,000` on a vast.ai RTX 4090.

A completed full-octant run at these radii is not practical with the current driver: the annulus is approximately 5 billion tiles, and `campaign_main_cuda` materializes both `active_tiles` and `tileops` before compositing. That implies memory far beyond the instance and runtime on the order of tens of hours at current throughput. Profiling therefore used representative contiguous interior regions at the requested radii:

- 200,000-tile R=85M region for baseline and Nsight Systems.
- 20,000-tile R=85M region for Nsight Compute attempts.

Artifacts are in `tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/profiling-2026-04-23/`.

Instance: vast.ai `35468412`, RTX 4090, CUDA 12.4 PyTorch image, SSH `ssh6.vast.ai:28412`. Instance intentionally left running for phase 2.

## Build and Verification

Build used:

```bash
cmake -S . -B build-k36 -G Ninja \
  -DK_SQ=36 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES=89 \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/workspace/deps/json
cmake --build build-k36 -j$(nproc)
ctest --test-dir build-k36 --output-on-failure
```

The shallow `nlohmann/json` checkout was supplied through CMake's FetchContent source override because the default full-history clone was slow on the instance. Source code was not patched.

CTest result: 10/10 passed, total test time 68.63s.

## Baseline Timing

200K-tile R=85M region:

| Phase | Time |
|---|---:|
| grid-init | 13.731s |
| CUDA K1-K5 dispatch | 2.865s |
| compositor | 0.277s |
| snapshot | 0.134s |
| total | 17.017s |

Throughput:

- CUDA K1-K5 dispatch: 69.8K tiles/s.
- Pipeline after grid-init: 61.0K tiles/s.

Overflow counters were all zero.

## Nsight Systems Result

Nsight Systems on the same 200K-tile region measured 2.051s of total GPU kernel time across 11 slabs, or 97.5K tiles/s kernel-only.

Kernel time split:

| Kernel | Total | Share |
|---|---:|---:|
| `kernel_mr` | 784.4ms | 38.2% |
| `kernel_face_encode_v2` | 596.4ms | 29.1% |
| `kernel_uf_v2` | 343.1ms | 16.7% |
| `kernel_sieve` | 301.7ms | 14.7% |
| `kernel_face_sort_pack` | 14.4ms | 0.7% |
| `kernel_compact` | 11.1ms | 0.5% |

Memcpy is not the bottleneck:

- D2H time: 2.41ms, 55.8 MB.
- H2D time: 0.26ms, 5.33 MB.

CUDA API overhead is material:

- `cudaMalloc`: 214.0ms across 232 calls.
- `cudaFree`: 236.5ms across 232 calls.
- `cudaHostAlloc` + `cudaFreeHost`: 100.0ms across 12 calls.
- Synchronization calls dominate API wait time, mostly reflecting GPU work completion.

## Nsight Compute

`ncu --set full` was attempted on the 20K-tile R=85M region. It could not collect kernel metrics because the container lacks permission for NVIDIA hardware performance counters:

```text
ERR_NVGPUCTRPERM - The user does not have permission to access NVIDIA GPU Performance Counters
```

`LaunchStats`/`Occupancy` sections hit the same permission wall. No `.ncu-rep` was produced.

Fallback static resource usage from `cuobjdump --dump-resource-usage`:

| Kernel | Registers | Shared | Stack |
|---|---:|---:|---:|
| `kernel_sieve` | 28 | 8 B | 40 B |
| `kernel_mr` | 46 | 0 B | 0 B |
| `kernel_compact` | 21 | 0 B | 0 B |
| `kernel_uf_v2` | 38 | 0 B | 0 B |
| `kernel_face_encode_v2` | 35 | 0 B | 0 B |
| `kernel_face_sort_pack` | 40 | 8201 B | 0 B |

All kernels launch with `BLOCK_THREADS=288`.

## Interpretation

The cycle eaters are MR and face encoding. Together they consume 67.3% of measured GPU kernel time. UF and sieve are secondary but still meaningful at 31.4% combined. Compact and sort/pack are noise.

The gap to the legacy 155K benchmark has two layers:

- Kernel-only current rate is 97.5K tiles/s, about 63% of 155K.
- Dispatch-level current rate is 69.8K tiles/s, about 45% of 155K.

The difference between 97.5K kernel-only and 69.8K dispatch-level is mostly host/device orchestration: repeated per-slab allocation/free, stats readbacks, forced phase synchronization, and pinned allocation setup. The 200K run used 11 slabs, so the driver is paying that orchestration cost repeatedly.

Likely optimization opportunities:

1. Reuse phase buffers across slabs instead of allocating/freeing every slab. This directly targets about 450ms in `cudaMalloc`/`cudaFree` on the 200K run.
2. Reduce synchronous per-slab diagnostics readbacks in production mode. The overflow/stat reads are useful, but currently force host-visible data movement and sequencing every slab.
3. Pipeline slabs so phase 1 of slab N+1 overlaps later work/readback for slab N where dependencies permit.
4. Attack `kernel_mr` first for pure GPU wins. It is the largest single kernel bucket and has the highest static register use at 46 registers.
5. Audit `kernel_face_encode_v2` for per-prime repeated work and atomics/serialization. It is now almost as expensive as MR and is new v2-specific work.

## Open

For true Nsight Compute memory/occupancy/bottleneck metrics, phase 2 needs an instance/host configured with unrestricted NVIDIA performance counters. The current vast.ai host blocks them even as root inside the container.
