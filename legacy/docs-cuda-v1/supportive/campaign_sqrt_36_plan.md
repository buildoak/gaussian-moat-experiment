---
title: campaign_sqrt_36 implementation plan
date: 2026-04-14
engine: codex
type: design-note
status: complete
refs: [docs/supportive/2026-04-13-k36-campaign-postmortem.md, docs/supportive/2026-04-13-sqrt40-600m-campaign-plan.md, docs/tile_spec.md, docs/tile_operations.md, docs/compositor_spec.md, docs/campaign_spec.md, tiles-maxxing/tile_cuda_multi_kernel/include/gpu_constants.cuh, tiles-maxxing/tile_cuda_multi_kernel/include/gpu_types.cuh, tiles-maxxing/tile_cuda_multi_kernel/src/kernel_face_encode.cu, tiles-maxxing/tile_cuda_multi_kernel/src/kernel_uf.cu, tiles-maxxing/tile_cuda_multi_kernel/src/main.cu, tiles-maxxing/tile-cpp/include/constants.h, tiles-maxxing/tile-cpp/include/types.h, tiles-maxxing/tile-cpp/src/encode.cpp, tiles-maxxing/tiles-compositor/include/types.h, tiles-maxxing/tiles-compositor/src/campaign.cpp, tiles-maxxing/tiles-compositor/src/tileop_parse.cpp]
---

# campaign_sqrt_36 implementation plan

Date: 2026-04-14

## Objective

Make the `K_SQ=36` campaign path trustworthy end-to-end by fixing the April 13 overflow failure mode without regressing the verified `K_SQ=40` path.

## Decision Boundary

There are two incompatible 256-byte formats in the docs:

- `docs/tile_spec.md:342-371` describes a symmetric `TileOp_wide`.
- `docs/compositor_spec.md:343-350` and the live parser in [tileop_parse.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/tileop_parse.cpp:17) assume a dynamic packed record with the same 3-byte header and a `253`-byte payload.

This plan follows the second form because:

- the user explicitly asked to verify the current `3-byte header / 125-byte payload` accounting;
- the compositor already supports `TILEOP_EXT_SIZE=256` and `TILEOP_EXT_PAYLOAD_BYTES=253` in [types.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/include/types.h:13).

## 1. TileOp Size Bump (128 -> 256 bytes)

### Current accounting

Verified standard record:

- `docs/tile_spec.md:155-185`: `128` total, `3` header, `125` payload.
- [gpu_constants.cuh](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/include/gpu_constants.cuh:61): `TILEOP_SIZE = 128`
- [gpu_constants.cuh](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/include/gpu_constants.cuh:62): `TILEOP_HEADER_BYTES = 3`
- [gpu_constants.cuh](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/include/gpu_constants.cuh:63): `TILEOP_PAYLOAD_BYTES = 125`
- [tile-cpp/constants.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/include/constants.h:39) mirrors the same values.

### Extended accounting

Use:

- `TILEOP_SIZE = 256`
- `TILEOP_HEADER_BYTES = 3`
- `TILEOP_PAYLOAD_BYTES = 253`

Affected files:

- GPU: [gpu_constants.cuh](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/include/gpu_constants.cuh:61), [gpu_types.cuh](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/include/gpu_types.cuh:10), [kernel_face_encode.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_face_encode.cu:391), [main.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/main.cu:280)
- C++ reference: [constants.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/include/constants.h:39), [types.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/include/types.h:11), [encode.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/src/encode.cpp:182)
- Compositor: [types.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/include/types.h:12), [tileop_parse.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/tileop_parse.cpp:17), [campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/campaign.cpp:55)

Formula changes:

- current: `r_cnt = (125 - o_cnt - i_cnt - 2*l_cnt) >> 1`
- extended: `r_cnt = (253 - o_cnt - i_cnt - 2*l_cnt) >> 1`

Code sites:

- [kernel_face_encode.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_face_encode.cu:416)
- [encode.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/src/encode.cpp:199)
- [tileop_parse.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/tileop_parse.cpp:51)

### Recommendation

Do not globally replace the standard 128-byte record. Keep `128B` for the hot path and use the `256B` form only for overflow reprocessing. That matches `docs/campaign_spec.md:1128-1141` and avoids doubling all campaign I/O.

## 2. GPU Cap Adjustments for K_SQ=36

Set in [gpu_constants.cuh](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/include/gpu_constants.cuh:55):

- `MAX_FACE_PRIMES_PER_FACE: 256 -> 512`
- `MAX_FACE_PORTS_GPU: 32 -> 64`
- `MAX_TOTAL_PORTS_GPU: 128 -> 256`

Make them `#if K_SQ_VAL <= 36` conditional. Do not raise them universally.

### MAX_GROUPS_GPU = 127

Keep it at `127`.

Why:

- `docs/tile_spec.md:84-100` and `docs/tile_operations.md:570-571` make the 7-bit group limit structural.
- GPU poison path: [kernel_face_encode.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_face_encode.cu:399)
- C++ poison path: [encode.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/src/encode.cpp:185)
- Compositor defensive guard: [compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/compositor.cpp:316)

If K36 still exceeds `127` groups after the port and payload fixes, that is a format redesign, not a constant tweak.

### Mandatory K5 correctness fix

At [kernel_face_encode.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_face_encode.cu:196), face primes beyond `MAX_FACE_PRIMES_PER_FACE` are currently dropped and then clamped at lines `222-223`. Change that to explicit overflow poisoning. Silent truncation is unsound.

## 3. CUDA Kernel Adjustments

### K1 sieve

- File: [kernel_sieve.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_sieve.cu)
- Change: none

### K2 MR

- File: [kernel_mr.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_mr.cu)
- Change: none

### K3 compact

- File: [kernel_compact.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_compact.cu)
- Change: none

### K4 UF

- File: [kernel_uf.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_uf.cu:74)
- Change: none for TileOp size or shared memory

### K5 face encode

File: [kernel_face_encode.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_face_encode.cu:196)

Mandatory changes:

1. explicit poison on face-prime overflow at lines `196-224`
2. cap raises at lines `260-291`
3. payload-budget change at lines `416-420`
4. output width change if the worker chooses a direct `256B` output record

### Shared memory and occupancy

Current K5 shared memory is about:

- face primes: `4 * 256 * 8 = 8192 B`
- counters: `16 B`
- `FaceScratchGPU`: about `2052 B`
- `FaceDataGPU`: `1032 B`
- total: about `11,292 B`

With `512 / 64 / 256` caps:

- face primes: `16,384 B`
- counters: `16 B`
- `FaceScratchGPU`: about `3076 B`
- `FaceDataGPU`: `1032 B`
- total: about `20,508 B`

Implication on RTX 4090:

- `BLOCK_THREADS = 288` still fixes thread geometry;
- K5 shared memory likely drops from a `5 blocks/SM` regime to `4 blocks/SM`;
- benchmark after the change via [main.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/main.cu:219).

## 4. C++ Compositor / Campaign Runner Changes

### campaign.cpp

Current behavior is mathematically unsound:

- stream path replaces overflow/malformed tiles with empties at [campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/campaign.cpp:743)
- burst path does the same at [campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/campaign.cpp:913)
- pure C++ path does the same at [campaign.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/campaign.cpp:1013)

Required change:

- replace “overflow -> empty” with “overflow -> reprocess via C++ into extended TileOp -> populate `ExtendedTileSideTable` -> pass non-null `ext` into `compositor.ingest_tower(...)`”.

Implementation sketch:

1. scan each burst output for overflow or malformed tiles
2. record `(flat_idx, a_lo, b_lo)` for each one
3. rerun those tiles through `process_tile(...)`
4. encode them as `256B` extended TileOps
5. store them in `ExtendedTileSideTable::extended_ops`
6. call `compositor.ingest_tower(j, tower_buf, &ext)`

### Compositor

The compositor already has the hooks:

- extended storage: [types.h](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/include/types.h:39)
- mixed-width lookup: [compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/compositor.cpp:285)
- mixed-width payload budget: [compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/compositor.cpp:294)
- parser support for `256B`: [tileop_parse.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/tileop_parse.cpp:37)

Minimal compositor change: none, if the runner fills the side table with dynamic-packed `256B` records.

### Reporting

Split the current `overflow_count` into:

- `overflow_sentinel_count`
- `malformed_count`
- `extended_reprocessed_count`
- `unresolved_overflow_count`

The real correctness gate is `unresolved_overflow_count == 0`.

## 5. Build System

Actual repo state:

- CUDA uses [tile_cuda_multi_kernel/Makefile](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/Makefile:1), not CMake
- tile-cpp uses [tile-cpp/CMakeLists.txt](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/CMakeLists.txt:1)
- compositor uses [tiles-compositor/CMakeLists.txt](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/CMakeLists.txt:1)

Required changes:

- CUDA: no CMake file to edit; keep `make K_SQ=36`, add K36 conditional caps in `gpu_constants.cuh`
- tile-cpp: existing `cmake .. -DK_SQ=36` is sufficient; add new sources only if the worker splits extended encoding out of `encode.cpp`
- compositor: existing `cmake .. -DK_SQ=36` is sufficient; add new campaign helper source only if needed

Do not fork whole kernels by `#if K_SQ_VAL == 36`. Gate only the caps.

## 6. Verification Plan

Before campaign runs:

1. Build `tile-cpp` with `-DK_SQ=36` and run `test_face_encode`, `test_compact_uf`, `test_e2e`.
2. Build `tiles-compositor` with `-DK_SQ=36` and run `test_tileop`, `test_compositor`, `test_compositor_real`.
3. Build CUDA with `make K_SQ=36`.

Tests to add:

- tile-cpp: `256B` extended encode/decode, `payload == 253` edge case, normal `128B` overflow that succeeds in `256B`, and `group_count = 128` poison
- compositor: `parse_counts(..., TILEOP_EXT_PAYLOAD_BYTES)`, mixed-width ingest, and regression that no overflow tile reaches `assert_not_overflow()`
- campaign: burst with at least one overflow tile that ends with `extended_reprocessed_count > 0` and `unresolved_overflow_count == 0`

Suggested first campaign-grade test:

- `K_SQ=36`
- `R >= 800,000,000`
- off-axis coordinates only
- a few thousand tiles, enough to exercise the K36 geometry without running a full octant

Pass criteria:

- zero unresolved overflows
- zero malformed offset triples
- zero empty-tile substitution for overflow

K40 regression:

- rerun the known-good CUDA `R=600,000,000`, `K_SQ=40` early-SPAN check from `2026-04-13-sqrt40-600m-campaign-plan.md`
- expect `SPANNING` and zero overflows

## 7. Risk Assessment

Primary risks:

1. K5 remains unsound if face-prime truncation is not changed into explicit poison.
2. A universal `256B` standard TileOp would double I/O and buffer traffic for little gain.
3. K5 occupancy will drop for K36 after the cap raise and must be re-measured.
4. The docs currently describe two incompatible `256B` formats.
5. Any remaining overflow -> empty substitution can create false MOAT results.

### MAX_GROUPS_GPU = 127

This limit is load-bearing:

- encoding semantics: [tile_spec.md](/Users/otonashi/thinking/building/gaussian-moat-cuda/docs/tile_spec.md:84)
- GPU poison path: [kernel_face_encode.cu](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile_cuda_multi_kernel/src/kernel_face_encode.cu:399)
- compositor defensive guard: [compositor.cpp](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tiles-compositor/src/compositor.cpp:316)

Conclusion:

- keep `MAX_GROUPS_GPU = 127`;
- treat any surviving `>127` case as a format-redesign trigger, not a cap-tuning task.

## Recommended Order

1. Fix K5 correctness:
   - explicit poison on face-prime overflow
   - K36 cap raises to `512 / 64 / 256`
2. Implement extended-tile reprocessing in `campaign.cpp`.
3. Add `256B` extended encode support in `tile-cpp` using `3 + 253` accounting.
4. Add parser and mixed-width tests.
5. Run a small K36 operating-point validation burst.
6. Re-run the K40 regression.

## Bottom Line

The April 13 K36 failure is not just “caps too small”. It is:

- undersized K5 caps,
- a silent K5 truncation path,
- and a campaign runner that deletes overflow connectivity instead of escalating to `256B` extended records.

The minimum credible repair is:

- raise K36 K5 caps to `512 / 64 / 256`,
- keep `MAX_GROUPS_GPU = 127`,
- implement the mixed-width overflow path the compositor already expects,
- and require `unresolved_overflow_count == 0` before trusting any K36 verdict.
