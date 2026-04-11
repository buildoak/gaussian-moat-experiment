---
title: Multi-Stream Pipeline Overlap — Implementation and Results
date: 2026-04-11
engine: claude
type: benchmark
status: complete
refs: [tile_cuda_multi_kernel/src/main.cu, docs/supportive/2026-04-11-4090-hardware-profiling.md]
---

# Multi-Stream Pipeline Overlap

Implementation and measurement of CUDA multi-stream overlap for the 5-kernel tile processing pipeline on RTX 4090. The hypothesis was that overlapping sub-batches across 2 streams could hide inter-kernel transition costs and improve throughput.

## Implementation

### Design

- 2 CUDA streams, each with its own complete set of intermediate buffers (double-buffered)
- Total tiles divided into 8 sub-batches (4 waves per stream), dispatched round-robin
- Shared FJ64 table (read-only, safe to share across streams)
- Per-sub-batch async H2D transfer of tile coordinates, then 5-kernel pipeline launch

### Double-Buffered Memory

Each stream gets independent allocations for all 10 inter-kernel buffers:
- `d_coords` (16B/tile), `d_cand_list` (24KB/tile), `d_total_cands` (4B/tile)
- `d_bitmap` (9.8KB/tile), `d_row_prefix` (544B/tile), `d_prime_pos` (10KB/tile)
- `d_prime_count` (4B/tile), `d_parent` (5KB/tile), `d_output` (128B/tile)
- `d_prime_counts_out` (4B/tile)

**Per-tile buffer cost:** ~50 KB. At 10K tiles: ~488 MB per stream x 2 streams = ~976 MB total.
GPU memory usage: 554 MB (10K tiles) to 681 MB (20K tiles), well within 24 GB.

### Code Changes

New `ms` command: `./tile_kernel_multi ms <count>` runs multi-stream mode.
Original single-stream mode preserved as `./tile_kernel_multi <count>`.
Patch file: `vast-ai/multi_stream.patch` (390 lines).

Key functions added:
- `StreamBuffers` — per-stream double-buffer management
- `launch_pipeline_stream()` — launches 5 kernels on a specific stream
- `run_bench_multistream()` — full multi-stream benchmark with timing and verification

## Results

### Performance Comparison

| Mode | 10K tiles/s | 20K tiles/s |
|------|------------|------------|
| Single-stream | 139,839 | 153,903 |
| Multi-stream (2 streams) | 141,625 | 142,948 |
| Delta | **+1.3%** | **-7.1%** |

### Why Multi-Stream Didn't Help

The nsys multi-stream timeline reveals the answer:

1. **Each kernel already saturates 128 SMs.** At 1,250 tiles/sub-batch (10K / 8), each kernel launch creates 1,250 thread blocks. With 4-5 blocks/SM capacity, 128 SMs can run 512-640 blocks concurrently. The 1,250 blocks need 2-3 full waves to process. There are no idle SMs for a second stream to exploit.

2. **Inter-kernel gaps are negligible (2-3 us).** The CUDA runtime achieves near-zero launch overhead between sequential kernels on the same stream. Multi-stream cannot reduce gaps that are already <0.01% of runtime.

3. **Smaller sub-batches reduce per-kernel efficiency.** Splitting 10K tiles into 8x1,250 loses the amortization benefits of large kernel launches. The kernel startup/tail overheads per sub-batch accumulate.

4. **At 20K tiles, multi-stream is actively harmful.** The sub-batch splitting reduces the grid size per launch, reducing steady-state SM utilization. The timing overhead of managing 2 streams and 8 sub-batches outweighs any overlap benefit.

### Observed Stream Overlap

The nsys timeline confirms that streams DO overlap in execution time:
- Stream 13's K2 overlaps with Stream 14's K1 (typical overlap: 0.5-2.1 ms per wave)
- Stream 13's K5 overlaps with Stream 14's K3-K4 (typical overlap: 0.3-1.8 ms per wave)

But this overlap is **time-sharing, not space-sharing**. Both streams compete for the same 128 SMs, creating interference rather than parallel speedup. The GPU scheduler interleaves warps from both streams, but total work done per SM-cycle is unchanged.

### Detailed Multi-Stream Timeline (10K tiles, first two waves)

| Kernel | Stream | Start (ms) | End (ms) | Duration (ms) |
|--------|--------|-----------|---------|---------------|
| K1 sieve | 13 | 1.845 | 4.600 | 2.755 |
| K1 sieve | 14 | 3.383 | 5.910 | 2.527 |
| K2 MR | 13 | 5.322 | 11.181 | 5.859 |
| K2 MR | 14 | 9.106 | 16.013 | 6.907 |
| K3 compact | 13 | 14.050 | 14.718 | 0.667 |
| K4 UF | 13 | 14.718 | 17.655 | 2.937 |
| K3 compact | 14 | 16.714 | 16.852 | 0.138 |
| K4 UF | 14 | 16.852 | 19.209 | 2.357 |
| K5 face | 13 | 18.903 | 19.392 | 0.489 |
| K5 face | 14 | 19.259 | 20.262 | 1.004 |

Note: K2 on stream 14 takes 6.907 ms vs 5.859 ms on stream 13 (+18%) — the interference from concurrent execution on stream 13 slows down the second stream's heavy kernel.

## Correctness Verification

- Smoke test output matches exactly: tile (600000000,600000000) prime_count=2218, tile (699999744,400000000) prime_count=2254
- No overflow sentinels in any run (10K and 20K tiles)
- Prime counts and tileop bytes consistent across single-stream and multi-stream modes

## What Further Overlap Is Possible

1. **None worthwhile at current batch sizes.** At 10K+ tiles, the GPU is fully saturated. Multi-stream overlap only helps when individual kernels under-utilize the GPU (fewer blocks than SMs).

2. **Host-device overlap is already optimal.** Memory transfers are <0.1 ms total. Async transfers in multi-stream mode provide no measurable benefit.

3. **Cross-pipeline overlap (multiple tile batches):** For production runs processing millions of tiles, overlapping the D2H output transfer of batch N with kernel execution of batch N+1 could save the ~70 us D2H time per batch. Negligible per batch but cumulative over thousands of batches.

4. **Persistent kernels:** Converting K2 (55% of runtime) to a persistent kernel that stays resident and pulls work from a queue would eliminate all launch overhead and enable finer-grained work distribution across SMs. This is a more promising architectural change than multi-stream.

## Conclusion

Multi-stream overlap is a **dead end** for this pipeline at 10K+ batch sizes on 4090. The pipeline is fully SM-saturated — every kernel generates enough thread blocks to fill all 128 SMs. The optimization opportunity lies in reducing total compute (sieve extension to reduce K2 candidates) or architectural changes (persistent kernels), not in execution scheduling.
