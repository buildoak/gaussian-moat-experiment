---
title: K5 face encode audit
date: 2026-04-23
engine: codex
type: audit
status: complete
refs:
  - tiles-maxxing/cuda-campaign-v2-sqrt-36/src/kernel_face_encode_v2.cu
  - tiles-maxxing/cuda-campaign-v2-sqrt-36/src/kernel_face_sort_pack.cu
  - tiles-maxxing/cuda-campaign-v2-sqrt-36/include/cuda_campaign/face_encode_buffers.cuh
  - tiles-maxxing/cuda-campaign-v2-sqrt-36/CMakeLists.txt
---

FAIL

K5 is materially overbuilt for the amount of work it needs to do. The implementation is dominated by serial per-tile work, a fixed-cost sort network that ignores the real port counts, and a split-kernel/global-staging design that turns a small per-tile encode into two full GPU passes.

## Top findings

### warning 1: Per-face connectivity is rebuilt serially in thread 0, leaving 287 threads idle for the hottest part of K5

**Evidence**

- `kernel_face_encode_v2` uses one 288-thread block per tile (`kernel_face_encode_v2<<<num_tiles, BLOCK_THREADS>>>`), with `BLOCK_THREADS = 288` and `--maxrregcount=40` (`tiles-maxxing/cuda-campaign-v2-sqrt-36/CMakeLists.txt:67-72`, `include/cuda_campaign/constants.cuh:33`).
- After the warp-local face-strip filter, the expensive phase runs under `if (tid == 0)` only (`src/kernel_face_encode_v2.cu:340-351`).
- That serial phase does:
  - face-local DSU initialization: `175-177`
  - all-pairs adjacency checks: `179-188`
  - root compression: `190-192`
  - root-by-root representative scan with another full scan over `face_count`: `194-234`
- `within_k_sq_packed()` re-decodes both packed positions on every pair test (`95-104`), so the serial all-pairs loop repeatedly pays packed-coordinate decode cost.

**Why it is slow**

- This is effectively a CPU algorithm embedded in one CUDA lane. During `build_face_dsu_and_reps()`, 8 of 9 warps are idle, and then 287 of 288 threads are idle.
- The complexity is at least quadratic in face-strip prime count, and representative extraction adds another scan over the same face-local set.
- The packed position decode is recomputed inside the nested loops instead of being cached once per face candidate.

**Proposed fix**

- Move face-local DSU/port extraction to a warp-cooperative or face-cooperative implementation:
  - one warp per face builds a compact face-local list in shared memory,
  - decode `(row, col)` or `(h, p_perp)` once per face candidate,
  - do local connectivity with warp-wide ballots/shared-memory union or a bounded local BFS/union over the compact face list.
- Better: feed K5 more directly from K4-visible-root data so K5 only groups already-known face-strip members, instead of rebuilding connectivity from scratch from raw prime positions.

**Estimated speedup**

- Conservative K5-only: `1.5x` to `2.2x`
- End-to-end pipeline: roughly `1.13x` to `1.25x` if K5 is really ~29% of GPU time

**Quick win vs refactor**

- Quick win: predecode face-strip coordinates into shared memory before the all-pairs loop.
- Deeper refactor: replace the thread-0 DSU path entirely with a cooperative per-face algorithm or K4-to-K5 metadata handoff.

### warning 2: The sort/pack phase uses a fixed 256-element bitonic sort per face, even though real counts are much smaller

**Evidence**

- `kernel_face_sort_pack` is a second standalone kernel (`src/kernel_face_sort_pack.cu:52-151`, launched from `host_driver.cpp:568-573` and `1111-1117`).
- For each face, it loads into `sort_scratch[NUM_FACES][SORT_CAPACITY]` with `SORT_CAPACITY = 256` (`11-13`, `64`).
- Every face is padded with sentinels to 256 entries (`110-113`) and then run through a full bitonic network over all 256 slots (`116-133`), regardless of `count`.
- The project context says observed port counts are far below this budget; the TileOp budget is `192` total ports, and typical observed maxima are around `~75`.

**Why it is slow**

- Bitonic sort does the same compare/swap schedule for tiny inputs and near-capacity inputs. A face with 6 representatives pays almost the same sort cost as a face with 200.
- Only 4 warps do useful sorting work (`warp < NUM_FACES` at `103`), so 5 warps in the 288-thread block are idle through the kernel's main phase.
- The kernel exists only to sort and copy labels into `TileOp`, so launch overhead and intermediate global-memory traffic are paid for a workload that is often very small.

**Proposed fix**

- Replace the fixed 256-lane bitonic network with a count-aware strategy:
  - insertion sort or odd-even sort for very small counts,
  - warp-level bitonic only over the next power-of-two >= actual `count`,
  - or stable radix/bin sort on `(h, p_perp, label)` if the coordinate range is tight enough.
- Best option: fuse sort + pack into `kernel_face_encode_v2` and sort the compact representative list in shared memory immediately after extraction.

**Estimated speedup**

- Conservative K5-only: `1.25x` to `1.6x`
- End-to-end pipeline: roughly `1.08x` to `1.14x`

**Quick win vs refactor**

- Quick win: limit the sort network to the smallest power-of-two covering `count`, not always 256.
- Deeper refactor: eliminate the second kernel and sort directly in the encode kernel's shared memory.

### warning 3: K5 spills large intermediates to global memory and rewrites TileOp twice instead of keeping the whole operation on-chip

**Evidence**

- `FaceEncodeDebugBuffers` allocates global arrays for:
  - `d_face_indices` = `[N * 4 * MAX_PRIMES_GPU]`
  - `d_face_roots` = `[N * 4 * MAX_PRIMES_GPU]`
  - `d_face_reps` = `[N * 4 * MAX_PRIMES_GPU]`
  - `d_face_counts`, `d_face_rep_counts`
  (`include/cuda_campaign/face_encode_buffers.cuh:25-38`)
- `kernel_face_encode_v2` writes these buffers, then `kernel_face_sort_pack` rereads `d_face_reps` and `d_face_rep_counts` (`src/kernel_face_encode_v2.cu:311-350`, `src/kernel_face_sort_pack.cu:74-79`, `103-113`).
- TileOp is also cleared in pieces multiple times:
  - full 256-byte clear in `write_flag_tileop()` (`src/kernel_face_encode_v2.cu:11-18`)
  - payload clear in `zero_sort_payload()` (`src/kernel_face_sort_pack.cu:40-49`)
  - full clear again on overflow in `zero_tileop()` (`33-38`, `94-97`)

**Why it is slow**

- The steady-state K5 path pays global-memory write/read traffic for transient data that is only consumed by the immediately following kernel.
- The debug-shaped buffer layout is sized to `MAX_PRIMES_GPU` rather than actual face counts, which is reasonable for diagnostics but a bad default data path.
- The extra kernel boundary prevents keeping face representatives in registers/shared memory and forces another block launch, another synchronization point, and another TileOp write pass.

**Proposed fix**

- Make the current global debug buffers optional, not the production data path.
- In production K5:
  - keep face candidate indices / local roots / representatives in shared memory,
  - sort and pack in the same kernel,
  - write `TileOp` exactly once at the end.
- If diagnostics are needed, add a debug/instrumented build mode that preserves the current global dumps.

**Estimated speedup**

- Conservative K5-only: `1.15x` to `1.35x`
- End-to-end pipeline: roughly `1.04x` to `1.10x`

**Quick win vs refactor**

- Quick win: skip redundant TileOp clears and stop routing production through debug buffers when debug output is disabled.
- Deeper refactor: fuse K5 into one kernel with shared-memory staging only.

## What I checked

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/src/kernel_face_encode_v2.cu`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/src/kernel_face_sort_pack.cu`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/include/cuda_campaign/face_encode_buffers.cuh`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/include/cuda_campaign/face_sort_pack.cuh`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/include/cuda_campaign/tileop.cuh`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/include/cuda_campaign/constants.cuh`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/src/host_driver.cpp`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/CMakeLists.txt`
- `tiles-maxxing/cpp-campaign-v2/src/tileop.cpp` to confirm the GPU path is mirroring the CPU algorithm rather than exploiting GPU-specific structure
- Existing K5 parity tests to confirm correctness coverage exists, but no performance-oriented verification or profiling assertions were found

## Bottom line

K5 is slow for structural reasons, not because of one small bug. The fastest plausible sequence is:

1. Fuse `kernel_face_encode_v2` and `kernel_face_sort_pack`.
2. Remove the fixed 256-element sort network in favor of count-aware sorting on compact per-face lists.
3. Replace the thread-0 face DSU/representative pass with a cooperative implementation, or reuse more K4 metadata so K5 stops recomputing face-local connectivity from raw packed positions.

Taken together, a conservative expectation is that K5 can likely be cut by about `2x`, with the first fused/count-aware rewrite capturing the easiest win and the DSU rewrite delivering the bigger follow-on gain.
