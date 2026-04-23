CONDITIONAL

# Full Pipeline Production Profiling Audit

Date: 2026-04-23  
Target: vast.ai instance `35468412`, RTX 4090 `sm_89`  
Binary: `tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k36/campaign_main_cuda`  
Command: `--k-sq=36 --r-inner=85000000 --r-outer=85008192 --region full-octant`

## Verdict

The real production run does **not** reproduce the reported 3x gap. With real snapshot output, the campaign processed `8,677,267` tiles in `109.140 s`, or `79.5K tiles/s`.

The actual bottleneck in the measured production path is GPU kernel execution, not grid build, compositor, disk I/O, or host-side launch overhead. Nsight Systems measured `87.354 s` of CUDA kernel time, which is `80.0%` of end-to-end wall time. The biggest kernels are `kernel_mr` and `kernel_face_encode_v2`.

Counter-level Nsight Compute and Linux `perf cycles` were blocked by host/container permissions, so warp stalls, dynamic instruction counts, L1/L2/DRAM throughput, and CPU callgraph samples are not available from this run.

## Findings

### warning: NCU hardware counters are blocked

`ncu --set full --target-processes all -o /workspace/profiles/full-kernel-profile ...` executed the full campaign but returned `ERR_NVGPUCTRPERM`.

Evidence: `/workspace/profiles/logs/ncu-full.log`:

```text
==ERROR== ERR_NVGPUCTRPERM - The user does not have permission to access NVIDIA GPU Performance Counters
==WARNING== No kernels were profiled.
```

Impact: no measured warp stalls, dynamic instructions executed, memory throughput, or cycle-attributed instruction mix. Static SASS/resource data below is a fallback, not a replacement for NCU counters.

### warning: `perf` is blocked by `perf_event_paranoid=4`

`perf record -e cycles -g --call-graph dwarf ...` failed even as root inside the container:

```text
No permission to enable cycles event.
```

`sysctl -w kernel.perf_event_paranoid=1` was ignored because `/proc/sys` is read-only. Impact: no Linux CPU callgraph. CPU attribution below comes from explicit `__rdtsc()` instrumentation plus Nsight Systems runtime traces.

### note: the "full campaign = 33K tiles/s" premise is not true for this build/command

Measured production output run:

| Metric | Value |
|---|---:|
| Active tiles | 8,677,267 |
| Real wall time | 109.718 s |
| Program total | 109.140 s |
| Throughput | 79.5K tiles/s |
| Verdict | SPANNING |
| Output | `/workspace/output-r85m.bin`, 2.1 GiB |

The measured gap is kernel-only `99.3K tiles/s` versus production `79.5K tiles/s`, about `1.25x`, not `3x`.

## Production Timeline

Real output command:

```bash
time ./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=85000000 \
  --r-outer=85008192 \
  --region full-octant \
  --out /workspace/output-r85m.bin
```

| Phase | Time | Wall % | Phase throughput |
|---|---:|---:|---:|
| Grid init | 2.520 s | 2.3% | 3.44M tiles/s |
| CUDA K1-K5 dispatch | 89.449 s | 82.0% | 97.0K tiles/s |
| Compositor | 12.230 s | 11.2% | 709.5K tiles/s |
| Snapshot write | 4.471 s | 4.1% | 1.94M tiles/s |
| Total | 109.140 s | 100.0% | 79.5K tiles/s |

Shell `time`: `real 1m49.718s`, `user 1m42.586s`, `sys 0m7.172s`.

## CPU Tick Breakdown

Estimated TSC rate from program total: `1.995 GHz`.

Top-level ticks:

| Phase | Ticks | Approx seconds | Total % |
|---|---:|---:|---:|
| Dispatch total | 178,486,688,932 | 89.449 s | 82.0% |
| Compositor total | 24,404,897,182 | 12.232 s | 11.2% |
| Snapshot | 8,921,596,510 | 4.471 s | 4.1% |
| Grid build | 5,029,613,338 | 2.521 s | 2.3% |
| Enumerate/sort active tiles | 920,963,178 | 0.462 s | 0.4% |

Dispatch internals:

| Dispatch subphase | Ticks | Approx seconds | Total % |
|---|---:|---:|---:|
| `cudaEventSynchronize(compute_done)` for stats path | 174,245,876,042 | 87.324 s | 80.0% |
| Host output copy from pinned slots | 1,352,061,816 | 0.678 s | 0.6% |
| Upload constants/FJ64 | 396,276,936 | 0.199 s | 0.2% |
| Stats D2H diagnostic copies | 164,307,826 | 0.082 s | 0.1% |
| Host coord pack | 153,492,030 | 0.077 s | 0.1% |
| Alloc/setup | 140,228,826 | 0.070 s | 0.1% |
| Kernel launch submit | 23,976,462 | 0.012 s | ~0.0% |

Compositor internals:

| Compositor subphase | Ticks | Approx seconds |
|---|---:|---:|
| `compositor.init(grid)` | 8,109,474,012 | 4.064 s |
| `enumerate_column_tiles` loop | 150,785,858 | 0.076 s |
| `compositor.ingest_column` loop | 16,087,936,764 | 8.063 s |

Interpretation: the large dispatch sync is not evidence of CPU work; it is host wait for GPU kernels. The CPU-side costs worth optimizing after kernels are compositor init/ingest and snapshot, but they are secondary.

## Kernel Timeline

Nsight Systems kernel totals:

| Kernel | Launches | Total time | Avg/launch | Kernel % | Regs/thread | Occupancy estimate | Static SASS top opcodes |
|---|---:|---:|---:|---:|---:|---:|---|
| `kernel_mr` | 521 | 33.987 s | 65.234 ms | 38.9% | 46 | ~75% | `IMAD`, `ISETP`, `IADD3`, `SEL`, `BRA` |
| `kernel_face_encode_v2` | 521 | 25.070 s | 48.119 ms | 28.7% | 35 | ~94% | `STG`, `IMAD`, `ISETP`, `IADD3`, `BRA` |
| `kernel_uf_v2` | 521 | 14.409 s | 27.657 ms | 16.5% | 38 | ~94% | `IMAD`, `ISETP`, `IADD3`, `BRA`, `STG` |
| `kernel_sieve` | 521 | 12.802 s | 24.572 ms | 14.7% | 28 | ~94% | `IMAD`, `ISETP`, `BRA`, `IADD3`, `LOP3` |
| `kernel_face_sort_pack` | 521 | 0.631 s | 1.210 ms | 0.7% | 40 | ~94% | `STG`, `LDS`, `ISETP`, `BRA`, `IMAD` |
| `kernel_compact` | 521 | 0.455 s | 0.874 ms | 0.5% | 21 | ~94% | `IMAD`, `IADD3`, `ISETP`, `BRA`, `LEA` |

Total CUDA kernel time: `87.354 s`, or `99.3K tiles/s`.

Occupancy estimate assumes RTX 4090/Ada limits of 1536 resident threads per SM and 64K registers per SM. It is resource-based only; actual achieved occupancy requires NCU.

## Static Instruction Breakdown

Dynamic instructions executed were not available because NCU counters were blocked. Static SASS instruction counts from `cuobjdump --dump-sass`:

| Kernel | Static SASS instructions | Top static instruction types |
|---|---:|---|
| `kernel_mr` | 4,232 | `IMAD:1224`, `ISETP:901`, `IADD3:610`, `SEL:332`, `BRA:253` |
| `kernel_face_encode_v2` | 2,332 | `STG:803`, `IMAD:266`, `ISETP:229`, `IADD3:166`, `BRA:153` |
| `kernel_uf_v2` | 1,456 | `IMAD:329`, `ISETP:216`, `IADD3:164`, `BRA:103`, `STG:93` |
| `kernel_face_sort_pack` | 1,336 | `STG:604`, `LDS:171`, `ISETP:84`, `BRA:84`, `IMAD:69` |
| `kernel_sieve` | 864 | `IMAD:142`, `ISETP:128`, `BRA:98`, `IADD3:71`, `LOP3:69` |
| `kernel_compact` | 392 | `IMAD:43`, `IADD3:40`, `ISETP:32`, `BRA:31`, `LEA:24` |

## Memory And Runtime Observations

NCU L1/L2/DRAM throughput was unavailable.

Nsight Systems memcpy totals:

| Copy direction kind | Calls | Bytes | Time |
|---|---:|---:|---:|
| D2H | 3,647 | 2,420,957,493 | 0.106 s |
| H2D | 528 | 208,788,784 | 0.011 s |

CUDA runtime top entries:

| Runtime call | Calls | Time |
|---|---:|---:|
| `cudaEventSynchronize` | 1,042 | 87.350 s |
| `cudaMemcpyToSymbol` | 6 | 0.122 s |
| `cudaMemcpy` | 3,127 | 0.082 s |
| `cudaHostAlloc` | 6 | 0.067 s |
| `cudaLaunchKernel` | 3,126 | 0.018 s |
| `cudaMemcpyAsync` | 1,042 | 0.006 s |

OS runtime top entries from Nsight Systems:

| OS call | Calls | Time |
|---|---:|---:|
| `poll` | 1,053 | 104.582 s |
| `pthread_cond_timedwait` | 207 | 103.514 s |
| `pthread_cond_wait` | 2 | 0.859 s |
| `ioctl` | 697 | 0.230 s |
| `writev` | 271,164 | 0.100 s |

The OS call totals can overlap and should not be summed as wall time. They confirm host blocking/wait behavior, not CPU computation.

## Where Is The Gap?

For the measured production command, there is no 3x end-to-end gap.

The measured losses relative to kernel-only are:

| Component | Added time |
|---|---:|
| Non-kernel CUDA dispatch/runtime overhead | about 2.1 s |
| Grid build + active tile enumeration/sort | about 3.0 s |
| Compositor | 12.2 s |
| Snapshot | 4.5 s |

The biggest non-kernel component is the compositor at `12.2 s`; it explains most of the gap between `99.3K` kernel-only and `79.5K` production throughput. Disk output is not the bottleneck at this scale.

If a separate run measured `33K tiles/s`, that measurement likely used a different binary, command, build, host load, chunk sizing, or an older path. It is not reproduced by the current production binary on instance `35468412`.

## Recommendations

1. Optimize `kernel_mr` first. It is `38.9%` of all GPU time and has the largest static integer-control footprint. Counter-enabled NCU is needed before making low-level changes.
2. Optimize `kernel_face_encode_v2` second. It is `28.7%` of GPU time and static SASS is store-heavy (`STG:803`), so review tileop/face debug writes and whether all debug arrays are required in production.
3. Split production overflow diagnostics from normal runs. The current stats path synchronizes every slab and copies diagnostic arrays. It is not the root bottleneck here, but it prevents cleaner overlap and adds instrumentation overhead to every production command.
4. Reduce compositor cost after GPU kernels. `compositor.init` is about `4.1 s`; `ingest_column` is about `8.1 s`. If kernel work improves, this becomes the next bottleneck.
5. Move future profiling to an instance with GPU performance counters and perf events enabled. Required gates: NCU without `ERR_NVGPUCTRPERM`; `perf_event_paranoid <= 1` or equivalent capabilities.

## Artifacts

Remote:

- `/workspace/profiles/logs/ncu-full.log`
- `/workspace/profiles/full-pipeline-nsys.nsys-rep`
- `/workspace/profiles/full-pipeline-nsys.sqlite`
- `/workspace/profiles/logs/nsys-full.log`
- `/workspace/profiles/logs/production-output.log`
- `/workspace/profiles/logs/production-output.time`
- `/workspace/profiles/cuobjdump-resource-usage.txt`
- `/workspace/profiles/sass-opcode-summary.txt`

Local pulled summaries:

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/full-pipeline-audit-2026-04-23/logs/`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/full-pipeline-audit-2026-04-23/cuobjdump-resource-usage.txt`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/full-pipeline-audit-2026-04-23/sass-opcode-summary.txt`
