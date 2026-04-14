---
title: CUDA Optimization Proposals After Fresh Profiling
date: 2026-04-10
engine: codex
type: design-note
status: complete
refs:
  - docs/supportive/2026-04-10-profiling-baseline.md
  - tile-cuda/src/tile_kernel.cu
  - tile-cuda/include/gpu_face_encode.cuh
  - tile-cuda/include/gpu_math.cuh
  - tile-cuda/include/gpu_sieve.cuh
  - tile-cuda/include/gpu_union_find.cuh
  - tile-cuda/include/gpu_compact.cuh
---

# CUDA Optimization Proposals After Fresh Profiling

## 1. Profiling summary

The optimization priority has changed materially:

- Phase 4+5 is the real bottleneck at **44.19%** of total cycles, not 1%.
- Phase 1c Miller-Rabin is still important at **25.85%**, but it is no longer the only thing that matters.
- Phase 1b scatter is **12.06%** and is paying for a second full sieve pass.
- Phase 3 union-find is **11.76%** and is doing more recomputation than it needs.
- Phase 1a sieve count is only **5.80%**, so sieve-only arithmetic work such as Barrett is no longer the top lever by itself.

The practical conclusion is simple: the next wins are structural, not arithmetic-only.

## 2. Phase-by-phase bottleneck analysis

### Phase 4+5: face extraction + encoding

Relevant code:

- `process_tiles_kernel()` launches the face path at `tile-cuda/src/tile_kernel.cu:217-223`.
- `extract_faces_gpu_parallel()` is at `tile-cuda/include/gpu_face_encode.cuh:144-280`.
- `prune_dead_ends_gpu()` is at `tile-cuda/include/gpu_face_encode.cuh:282-370`.
- `encode_tileop_gpu()` is at `tile-cuda/include/gpu_face_encode.cuh:372-418`.

Observed root causes:

- Only `warp_id < FACES_PER_PASS` participates in the expensive extraction loop (`tile-cuda/include/gpu_face_encode.cuh:173-205`). With `FACES_PER_PASS=2`, just 2 warps out of 9 do the heavy work for each pass.
- Each active lane scans **all primes for every boundary coordinate** (`tile-cuda/include/gpu_face_encode.cuh:176-205`). That is `O(faces * TILE_POINTS * prime_count)`, with repeated decode and membership checks.
- Port discovery is serialized onto `lane == 0` (`tile-cuda/include/gpu_face_encode.cuh:209-264`).
- Dead-end pruning and group assignment are serialized onto `warp_id == 4 && lane == 0` (`tile-cuda/include/gpu_face_encode.cuh:301-367`).
- TileOp encoding rescans `face_data->ports` repeatedly:
  - once for per-face counts (`:387-392`)
  - four times for group bytes (`:409-413`)
  - twice more for `h1` bytes (`:415-417`)
- `decode_prime_pos()` uses `/ SIDE_EXP` and `% SIDE_EXP` (`tile-cuda/include/gpu_face_encode.cuh:20-23`), so the hottest loop is also paying integer division/modulo by 271.

This phase is slow because it combines bad asymptotic work, poor warp utilization, and a long serial tail.

### Phase 1c: Miller-Rabin

Relevant code:

- `mr_test_candidates()` dispatches primality work at `tile-cuda/include/gpu_sieve.cuh:77-105`.
- `is_prime_gpu()` and Montgomery helpers are in `tile-cuda/include/gpu_math.cuh:37-158`.

Observed root causes:

- The hot work is the Montgomery multiply chain in `mont_powmod_gpu()` and `miller_rabin_witness_mont_gpu()` (`tile-cuda/include/gpu_math.cuh:87-120`), not the old helper-based `%` path.
- There are still avoidable `%` operations in Montgomery setup and conversion:
  - `mont_compute_r2()` starts with `1ULL % m` (`:56-61`)
  - `mont_init_gpu()` does `1ULL % ctx.m` and `(ctx.m - 1ULL) % ctx.m` (`:72-76`)
  - `mont_to_gpu()` does `a % ctx.m` even though witnesses are already guarded by `a >= ctx.m` (`:79-80`, `:100-107`)
- Trial division still pays 24 `%` operations per candidate (`tile-cuda/include/gpu_math.cuh:134-141`).

This phase is expensive because every surviving candidate runs a full serial modexp chain. It is compute-bound, and only low-risk arithmetic cleanup is obvious from the current code.

### Phase 1b: scatter

Relevant code:

- The two-pass sieve/scatter section is `tile-cuda/src/tile_kernel.cu:121-158`.
- `sieve_row()` is `tile-cuda/include/gpu_sieve.cuh:14-41`.
- `scatter_survivors()` is `tile-cuda/include/gpu_sieve.cuh:58-75`.
- `block_exclusive_scan()` is `tile-cuda/include/gpu_compact.cuh:18-36`.

Observed root causes:

- The kernel runs `sieve_row()` twice per active row: once to count (`tile-cuda/src/tile_kernel.cu:121-127`), then again to scatter (`:144-149`).
- The second pass is separated by a block-wide prefix scan over `cand_prefix` (`:141`), so the row-local bitmap cannot stay live without risking the register cliff.
- Phase 1b variance in the profiling note matches this design: more survivors mean more scatter work after already paying for a second full sieve.

This phase is slow because it duplicates the expensive row sieve and adds a synchronization-heavy scan just to reserve output space.

### Phase 3: union-find

Relevant code:

- `build_components_gpu()` is `tile-cuda/include/gpu_union_find.cuh:49-89`.
- `gpu_uf_index_union_find()` is `tile-cuda/include/gpu_union_find.cuh:14-25`.
- `compact_row()` builds `prime_pos` at `tile-cuda/include/gpu_compact.cuh:58-108`.

Observed root causes:

- Only 32 threads participate (`tile-cuda/src/tile_kernel.cu:207-209`), so 256 threads are idle during the whole phase.
- For every backward neighbor hit, `gpu_uf_index_union_find()` recomputes the in-row dense index by popcounting earlier words (`tile-cuda/include/gpu_union_find.cuh:16-24`).
- `prime_pos` is stored as `row * SIDE_EXP + col` (`tile-cuda/include/gpu_compact.cuh:96-99`), so both union-find and face extraction repeatedly decode it with division/modulo by 271.

This phase is slow because it mixes low thread utilization with repeated prefix recomputation and unnecessary decode cost.

## 3. Proposed optimizations ranked by leverage

### 1. Rewrite Phase 4+5 around prime-centric face classification and direct face scans

- Targets: Phase 4+5, **44.19%** of cycles.
- Root cause:
  - Current extraction is boundary-centric and re-reads all primes for every `h`.
  - Most warps are idle.
  - Port extraction, pruning, and encoding all end in serial scans.
- Proposed fix:
  - Replace the inner extraction loop with a **prime-centric pass**:
    - each thread processes `for (i = tid; i < bounded_prime_count; i += BLOCK_THREADS)`
    - decode row/col once
    - classify that prime against the active face pass
    - write directly into a compact per-face boundary scratch structure
  - Keep one pass at 2 faces if needed for shared-memory margin, but make all 288 threads participate in classifying primes.
  - Replace `FaceCellGPU[2][257]` with a tighter representation for the active pass:
    - `roots[2][257][COLLAR]`
    - `mask[2][257]`
  - After classification, give one warp per face the ordered face scan to detect port starts. That scan is inherently ordered, but it is only `257 x 7` cells per face and should not be preceded by a full `prime_count` rescan.
  - Preserve exact face-order semantics by keeping the final port list generation in canonical face/h/depth order.
- Expected impact:
  - This removes the dominant `O(faces * TILE_POINTS * prime_count)` work and replaces it with roughly `O(prime_count + faces * TILE_POINTS * COLLAR)`.
  - Rough estimate: **2.5x to 3.0x faster for Phase 4+5**.
  - Total-kernel impact: about **1.36x to 1.43x overall** by itself.
- Occupancy risk:
  - Shared memory: manageable. A 2-face scratch with `uint16_t roots[2][257][7]` plus `uint32_t masks[2][257]` is about `9.3 KB`, only about `1.0 KB` above the current `FaceCellGPU` footprint. That still stays below the current Phase-1 overlay ceiling.
  - Registers: moderate risk. The rewrite activates all threads and adds per-thread classification state, but it also removes the long nested loops and repeated decode state from the active warps. This must be checked against the 56-register cliff.
- Implementation complexity: **3-day** for a first correct version, **week+** if combined with deeper fusion of pruning/encoding in the same patch.

### 2. Collapse the serial tail in Phase 4+5 by fusing prune/group/encode state

- Targets: remaining serial work inside Phase 4+5, still on the same **44.19%** bucket.
- Root cause:
  - `prune_dead_ends_gpu()` zeroes and rebuilds `group_entries`, then rescans raw ports to emit `face_data` (`tile-cuda/include/gpu_face_encode.cuh:289-367`).
  - `encode_tileop_gpu()` then rescans `face_data` seven more times (`:387-417`).
  - The whole back half effectively runs on a single lane.
- Proposed fix:
  - Keep per-face port counts as you build the raw port list.
  - Assign groups once, then emit directly into `TileOp` staging buffers in face order instead of materializing and zeroing all `MAX_PORTS_GPU` entries in `FaceDataGPU`.
  - If keeping `FaceDataGPU` for safety during the first rewrite, at minimum:
    - store `face_counts` during emission
    - remove `count_ports_by_face()`
    - replace `append_face_groups()`/`append_face_h1()` rescans with a single linear pass that writes both group and `h1` bytes to their final offsets
  - If the current open-address hash is retained, grow it to a power-of-two table to replace `% MAX_GROUPS_GPU` in `find_group_entry()` with a mask.
- Expected impact:
  - If done as a minimal pass on the current structure: **1.15x to 1.30x faster for Phase 4+5**.
  - If done after the phase-4 rewrite above, it should still shave another **10% to 20%** off the reduced face/encode time.
  - Total-kernel impact as a standalone cleanup: about **1.06x to 1.11x overall**.
- Occupancy risk:
  - Shared memory: low to moderate. Direct TileOp staging can stay within the current `FaceDataGPU` budget.
  - Registers: low. This is mostly control-flow and dataflow simplification.
- Implementation complexity: **1-day** for the minimal no-rescan version, **3-day** if paired with new group-table layout.

### 3. Replace the two-pass sieve with single-pass row reservation

- Targets: Phase 1b, **12.06%** of cycles.
- Root cause:
  - The kernel computes `ws[9]` twice because output reservation depends on a separate prefix scan.
- Proposed fix:
  - Use the row-reservation plan already outlined in `docs/supportive/2026-04-10-barrett-sieve-plan.md`:
    - row thread computes `ws` once
    - counts survivors
    - reserves space with one shared `atomicAdd`
    - scatters immediately
  - Clamp the final raw candidate count once after all row threads finish.
- Expected impact:
  - This removes the second full `sieve_row()` call and the block-wide prefix-scan cost.
  - Rough estimate: **2.2x to 2.6x faster for Phase 1b**.
  - Total-kernel impact: about **1.08x to 1.11x overall**.
- Occupancy risk:
  - Shared memory: positive. The existing plan reduces total block footprint from `37,680 B` to about `35,376 B`.
  - Registers: low, because `ws[9]` still dies within the row thread and does not need to live across a barrier.
- Implementation complexity: **1-day**.

### 4. Add a row-word prefix table for union-find and stop decoding `prime_pos` with division

- Targets: Phase 3, **11.76%** of cycles, with some spillover benefit into Phase 4+5.
- Root cause:
  - `gpu_uf_index_union_find()` loops over previous bitmap words for every neighbor hit.
  - `prime_pos` decode uses divide/modulo by 271 in both Phase 3 and Phase 4+5.
- Proposed fix:
  - Change `compact_row()` to store `prime_pos` as `(row << 16) | col` instead of `row * SIDE_EXP + col`.
  - Update union-find and face extraction decode sites to use bit shifts and masks.
  - During compaction, build a small shared `row_word_prefix[ACTIVE_ROWS][BITMAP_WORDS_PER_ROW + 1]` table so dense index lookup becomes:
    - `row_prefix[row] + row_word_prefix[row][full_words] + popc(partial_word)`
  - Use that table in `gpu_uf_index_union_find()`.
- Expected impact:
  - Packed row/col decode is a near-free cleanup and should help both Phase 3 and Phase 4+5.
  - The row-word prefix table removes repeated mini-scans from the UF hot loop.
  - Rough estimate: **1.5x to 1.8x faster for Phase 3**.
  - Total-kernel impact: about **1.06x to 1.09x overall**.
- Occupancy risk:
  - Shared memory: the prefix table costs about `271 * 10 * 2 = 5,420 B`.
  - On the current kernel this still fits because Phase 1 remains the max overlay.
  - After the single-pass sieve, this would likely become the max overlay, but the total block footprint still stays under the `42,000 B` target budget.
  - Registers: low.
- Implementation complexity: **3-day** if done together with the prefix table, **1-day** for the packed `prime_pos` cleanup alone.

### 5. Do low-risk Miller-Rabin arithmetic cleanup, not a full MR redesign

- Targets: Phase 1c, **25.85%** of cycles.
- Root cause:
  - The current MR path is already Montgomery-based, so a big structural replacement is not justified by the profiling.
  - There are still redundant `%` operations and fixed-divisor trial divisions that can be simplified.
- Proposed fix:
  - Remove provably redundant `%` sites:
    - `1ULL % m`
    - `1ULL % ctx.m`
    - `(ctx.m - 1ULL) % ctx.m`
    - `a % ctx.m` in `mont_to_gpu()` when `a < ctx.m`
  - Add a small Barrett table for the 24 fixed trial primes and replace `n % p` in the trial division loop.
  - Do not replace the witness-loop Montgomery math unless profiling after the earlier fixes still shows Phase 1c dominating.
- Expected impact:
  - This is a cleanup pass, not a transformational speedup.
  - Rough estimate: **1.08x to 1.15x faster for Phase 1c**.
  - Total-kernel impact: about **1.02x to 1.04x overall**.
- Occupancy risk:
  - Shared memory: none.
  - Registers: low to moderate. The Barrett helper should stay compact; if it inflates register count materially, back it out.
- Implementation complexity: **1-day**.

## 4. Recommended implementation order

1. **Single-pass row reservation for Phase 1b.**
   - Fastest safe win.
   - Low risk to occupancy.
   - Immediately removes a whole extra sieve pass.

2. **Packed `prime_pos` cleanup (`row<<16 | col`).**
   - Very cheap.
   - Helps both union-find and face extraction.
   - Good setup for the larger face rewrite.

3. **Prime-centric Phase 4+5 rewrite.**
   - Highest leverage by far.
   - Do this after the packed-coordinate cleanup so the rewrite does not carry the old decode cost forward.

4. **Fuse the Phase 4+5 serial tail.**
   - Either as the second half of the face rewrite, or immediately after if the first rewrite keeps `FaceDataGPU` for safety.

5. **Union-find row-word prefix table.**
   - Worth doing after the face rewrite, because by then Phase 3 is likely the next-largest remaining non-MR bucket.

6. **MR arithmetic cleanup.**
   - Keep it late.
   - Useful, but no longer the best next move.

## 5. Combined expected throughput estimate

Using conservative mid-range estimates:

- Phase 4+5 rewrite: `2.7x`
- Phase 4+5 serial-tail cleanup on top: additional `1.15x`
- Phase 1b single-pass sieve: `2.4x`
- Phase 3 union-find prefix/decode cleanup: `1.6x`
- Phase 1c MR cleanup: `1.10x`

That gives a plausible total-kernel reduction to roughly **56% to 63%** of the current cycle budget, depending on overlap between the two Phase 4+5 changes.

Practical throughput estimate from the current `1,095 tiles/sec` baseline:

- likely next-stage range: **1,700 to 1,950 tiles/sec**
- first-step range from just items 1, 2, and 3: roughly **1,500 to 1,700 tiles/sec**

The dominant uncertainty is how much of Phase 4+5 is the extraction rescan versus the serial grouping/encoding tail. The code strongly suggests the extraction rescan is the main offender, so that is the correct next optimization target.

## 6. What moved down in priority

The fresh profiling changes the answer from the earlier plan:

- **Sieve Barrett is still valid, but not top priority now.**
  - Phase 1a is only **5.80%**.
  - Single-pass scatter removes more total work than arithmetic cleanup in the count pass.
- **A full Miller-Rabin redesign is not justified yet.**
  - Phase 1c matters, but the measured kernel is no longer MR-dominated.
  - Structural work on Phase 4+5 is the bigger win.
