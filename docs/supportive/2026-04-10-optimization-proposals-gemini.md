---
title: CUDA Kernel Optimization Proposals
date: 2026-04-10
engine: gemini
type: design-note
status: complete
---

# Profiling Summary

Fresh profiling of the Orin Nano (sm_87, 8 SMs) at 1,020 MHz (1,131 tiles/sec) reveals a dramatic departure from the theoretical cycle budget:
- **Phase 4+5 (Face Extraction & Encoding):** 44.19% of cycles (theoretical: 1%).
- **Phase 1c (Miller-Rabin):** 25.85% (theoretical: 45%).
- **Phase 1b (Sieve Scatter):** 12.06% (theoretical: 2%).
- **Phase 3 (Union-Find):** 11.76% (theoretical: 17%).
- **Occupancy Constraints:** The kernel is bounded by 46 registers/thread and ~37.6KB shared memory to maintain the target 4 blocks/SM.

# Phase-by-Phase Bottleneck Analysis & Proposals

## 1. Phase 4+5 (Face Extraction + Port Encoding) — 44% of cycles
- **Root Cause:**
  1. **O(N * M) Redundant Work:** `extract_faces_gpu_parallel` assigns each thread a point `h` along a face. Each thread then loops over *all* primes (`bounded_prime_count`, up to 2560), calling `decode_prime_pos` and `face_membership` to check if a prime lands on its face coordinate.
  2. **Serial Bottlenecks:** Lane 0 executes a serial loop over `TILE_POINTS` (257) to assemble ports. Thread 0 executes `encode_tileop_gpu`, doing sequential pointer chasing and byte appending.
- **Proposed Fix: Perimeter Bitmap Traversal & Warp-Parallel Encoding**
  - **Algorithm:** Stop iterating over the linear `prime_pos` list. The tile perimeter directly corresponds to fixed coordinates in the `bitmap`. Threads should directly read the `bitmap` (already in shared memory) at the collar depths for their assigned face and `h` coordinate. This makes finding face primes an O(1) lookup per point.
  - **Parallelization:** Use warp-level primitives (`__ballot_sync`, warp prefix sum) across the face to extract active ports instead of the lane-0 serial loop. Parallelize `encode_tileop_gpu` across the warp to scatter bytes directly into the `TileOp` output buffer.
- **Expected Impact:** ~80-90% reduction in Phase 4+5 cycles (saving ~11.5M cycles, or ~40% of total kernel time).
- **Risk to Occupancy:** Very low. Directly looking up the bitmap removes the need for the `face_cells` array in shared memory, potentially *saving* ~8KB (`FACES_PER_PASS * 257 * 16 bytes`) of shared memory.
- **Implementation Complexity:** 3-day effort. Requires a moderate rewrite of `gpu_face_encode.cuh`.

## 2. Phase 1b (Sieve Scatter) — 12% of cycles
- **Root Cause:** `process_tiles_kernel` executes the exact same `sieve_row` logic twice per active row: once to count survivors, and again to scatter them after a block-wide prefix sum. This doubles the compute cost of the sieve.
- **Proposed Fix: Single-Pass Sieve with Row-level Atomics**
  - Adopt Option B from the Barrett/Sieve design plan.
  - Each thread computes `sieve_row` once, counts survivors, and uses a single `atomicAdd` on a block-shared `total_cands` counter to reserve a contiguous chunk in `cand_list`.
  - The thread immediately scatters survivors into its reserved chunk.
- **Expected Impact:** Eliminates the second sieve pass entirely, saving ~3M cycles (~10% of total kernel time).
- **Risk to Occupancy:** *Improves* occupancy margin. Eliminates `cand_counts` and `cand_prefix` from shared memory (saving ~2.3KB).
- **Implementation Complexity:** 1-day effort. Localized to the Phase 1a/1b block in `tile_kernel.cu`.

## 3. Phase 3 (Union-Find Component Detection) — 12% of cycles
- **Root Cause:** In `build_components_gpu`, threads check up to 64 backward offsets per prime. If a neighbor is present, `gpu_uf_index_union_find` is called to resolve its `prime_pos` index. This function currently executes a linear loop over `BITMAP_WORDS_PER_ROW` (9 words), calling `__popc` on the shared memory bitmap. With ~2500 primes × ~10 valid neighbors × 9 words, this generates massive shared memory read traffic and instruction bloat.
- **Proposed Fix: Fast Binary Search within Row Bounds**
  - We already compute `row_prefix` (the starting index for primes in each row). The columns of primes in that row are contiguous and sorted in the `prime_pos` array.
  - Replace the bitmap popcount loop with a binary search for `col` within the small subarray: `prime_pos[row_prefix[nr] ... row_prefix[nr+1]-1]`.
  - Since an average row contains only ~10 primes, the binary search takes ~3-4 iterations of pure register/L1 reads, skipping the shared memory bitmap entirely.
- **Expected Impact:** ~40-50% reduction in Phase 3 cycles (saving ~1.5M cycles).
- **Risk to Occupancy:** None.
- **Implementation Complexity:** 1-day effort. Localized to `gpu_uf_index_union_find` in `gpu_union_find.cuh`.

## 4. Phase 1c (Miller-Rabin) — 26% of cycles
- **Root Cause:** While the core Montgomery multiplication is well-optimized, there are redundant 64-bit modulo operations (`% ctx.m`) in the context initialization and witness loops. Additionally, the pre-MR trial division loop uses expensive 64-bit `%` for 24 fixed primes.
- **Proposed Fix: Targeted Modulo Cleanup & Trial Division Barrett**
  - Implement Barrett reduction for the 24 fixed trial primes (`c_trial_primes`).
  - Strip redundant `%` operators in Montgomery setup (e.g., `1ULL % m` → `1ULL`) as detailed in the Barrett design plan.
  - Do *not* replace the core Montgomery modexp with Barrett, as the modulus changes per candidate.
- **Expected Impact:** ~10-15% reduction in Phase 1c cycles (saving ~1M cycles).
- **Risk to Occupancy:** Negligible. The small trial prime Barrett multipliers (24 × 4 bytes) fit easily in constant memory.
- **Implementation Complexity:** 1-day effort. Localized to `gpu_math.cuh`.

# Recommended Implementation Order

1. **Single-Pass Sieve (Phase 1b):** Quickest win. Reduces cycles and shrinks the shared memory footprint, locking in occupancy safety for subsequent changes.
2. **Union-Find Binary Search (Phase 3):** High-leverage, isolated algorithmic change that eliminates millions of shared memory reads without complex warp synchronization.
3. **Face Extraction Bitmap Traversal (Phase 4+5):** The largest bottleneck. The shared memory freed by the Single-Pass Sieve provides extra safety margin for any warp-level structures needed here.
4. **Miller-Rabin Modulo Cleanup (Phase 1c):** Clean up the remaining scalar inefficiencies without risking the correctness of the larger architectural changes.

# Combined Expected Throughput Estimate

- **Baseline Cycles:** ~28.8M
- **Phase 4+5 Savings:** ~11.5M cycles
- **Phase 1b Savings:** ~3.0M cycles
- **Phase 3 Savings:** ~1.5M cycles
- **Phase 1c Savings:** ~1.0M cycles
- **Total Projected Savings:** ~17.0M cycles
- **New Expected Cycles:** ~11.8M cycles per tile
- **New Expected Throughput:** ~2,750 tiles/sec (a **~2.4x speedup** from the 1,131 tiles/sec baseline).