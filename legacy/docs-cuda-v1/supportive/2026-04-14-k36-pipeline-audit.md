---
title: K_SQ=36 Pipeline Audit
date: 2026-04-14
engine: codex/gpt-5.4
type: audit
status: complete
campaign: campaign-sqrt-36
refs:
  - docs/tile_spec.md
  - docs/tile_operations.md
  - tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/include/gpu_constants.cuh
  - tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_face_encode.cu
  - tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp
  - tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/tileop_parse.cpp
---

# Verdict

FAIL

The `K_SQ=36` pipeline still has one correctness bug that can change the campaign verdict, plus one parser hardening gap that can admit malformed 256-byte TileOps as if they were valid records. The local verification surface for the widened TileOp format is also stale: the parser/encoder tests fail as written, so they are not currently proving correctness.

## What I Checked

- Specs: `docs/tile_spec.md`, `docs/tile_operations.md`, `docs/grid_spec.md`
- CUDA path: `tile_cuda_multi_kernel/include/gpu_constants.cuh`, `src/kernel_sieve.cu`, `src/kernel_mr.cu`, `src/kernel_compact.cu`, `src/kernel_uf.cu`, `src/kernel_face_encode.cu`, `src/main.cu`
- C++ reference: `tile-cpp/include/constants.h`, `include/types.h`, `src/process_tile.cpp`, `src/compact.cpp`, `src/union_find.cpp`, `src/face_extract.cpp`, `src/prune.cpp`, `src/encode.cpp`
- Compositor path: `tiles-compositor/src/campaign.cpp`, `src/compositor.cpp`, `src/tileop_parse.cpp`
- Existing evidence: `docs/supportive/2026-04-13-k36-campaign-postmortem.md`
- Local verification:
  - `cd tiles-maxxing/campaign-sqrt-36/tiles-compositor && cmake -DCMAKE_BUILD_TYPE=Release -DK_SQ=36 .. && cmake --build . -j4 && ./test_tileop`
  - `cd tiles-maxxing/campaign-sqrt-36/tile-cpp && cmake -DCMAKE_BUILD_TYPE=Release -DK_SQ=36 .. && cmake --build . -j4 && ./test_face_encode`

## CRITICAL

### 1. Overflow and malformed K_SQ=36 tiles are still downgraded to empty tiles, which can force false `MOAT` verdicts

**Evidence**

- GPU K5 still has hard face-extraction caps: `MAX_FACE_PRIMES_PER_FACE = 256`, `MAX_FACE_PORTS_GPU = 48`, `MAX_TOTAL_PORTS_GPU = 192` in [gpu_constants.cuh](../../tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/include/gpu_constants.cuh#L52), and it poisons on overflow in [kernel_face_encode.cu](../../tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_face_encode.cu#L207), [kernel_face_encode.cu](../../tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_face_encode.cu#L222), [kernel_face_encode.cu](../../tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_face_encode.cu#L279), [kernel_face_encode.cu](../../tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_face_encode.cu#L476).
- Campaign-side handling does not reprocess those tiles; it rewrites them to dead/empty TileOps in all paths:
  - tower 0 C++ path: [campaign.cpp](../../tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp#L610)
  - CUDA stream path: [campaign.cpp](../../tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp#L743)
  - CUDA burst path: [campaign.cpp](../../tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp#L920)
  - non-burst CUDA path: [campaign.cpp](../../tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp#L1013)
- The code comment explicitly admits the sound fallback is missing: “extended reprocessing not yet implemented” at [campaign.cpp](../../tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp#L1021).
- This is not hypothetical. The prior `K_SQ=36` campaign saw `651,413` overflow/malformed replacements (`22.4%`) and concluded the verdict was unreliable in [2026-04-13-k36-campaign-postmortem.md](../supportive/2026-04-13-k36-campaign-postmortem.md#L5) and [2026-04-13-k36-campaign-postmortem.md](../supportive/2026-04-13-k36-campaign-postmortem.md#L86).

**Why It Matters**

Replacing an overflow tile with an empty tile deletes all connectivity crossing that tile. That cannot create a false `SPANNING`, but it absolutely can create a false `MOAT` by severing a real spanning chain. For `K_SQ=36`, this already happened at material rates in production-like runs.

**Proposed Fix**

- Do not convert overflow/malformed tiles to empties in a verdict-producing campaign.
- Implement the promised fallback: reprocess overflow tiles through the host reference encoder into a sound extended record, or fail the campaign hard if sound reprocessing is unavailable.
- Keep the larger `K_SQ=36` caps, but treat cap overflow as a recoverable slow path, not as “no connectivity”.

## MAJOR

### 2. The widened TileOp parser still accepts malformed 256-byte layouts that are not structurally valid

**Evidence**

- `parse_counts()` only enforces `off_I <= off_L <= off_R` and `residual >= 0`, then truncates the remaining budget with `counts.r_cnt = residual / 2` in [tileop_parse.cpp](../../tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/tileop_parse.cpp#L17).
- It never rejects odd residual payloads, so malformed headers can leave one unaccounted byte and still be treated as valid.
- Campaign-side malformed filtering is weaker than it looks. The check at [campaign.cpp](../../tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp#L755) and [campaign.cpp](../../tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp#L930) only validates monotone offsets; `b2 > TILEOP_SIZE` is dead code because `b2` is `uint8_t` and `TILEOP_SIZE` is `256`, which the compiler already warns about.
- This matters because the pipeline has already produced malformed TileOps in practice: the prior `K_SQ=36` run hit `FATAL parse_counts: off_I=3 off_L=0 off_R=0 ...` in [2026-04-13-k36-campaign-postmortem.md](../supportive/2026-04-13-k36-campaign-postmortem.md#L15).

**Why It Matters**

Today the pipeline catches only one class of corruption: non-monotone offsets. If a malformed record has monotone offsets but an invalid residual split, the compositor will derive a bogus `r_cnt`, reinterpret padding as extra R-face slots, and proceed. Because zero-padded slots are treated as ignorable only when the decoded group is zero, this is not a sound corruption boundary; garbage bytes can fabricate or delete boundary groups.

**Proposed Fix**

- Make `parse_counts()` reject odd residual payloads and any layout whose derived `h_start + l_cnt + r_cnt` does not exactly match the encoded layout contract.
- Mirror the same validation in the campaign-side malformed-tile filter before ingestion.
- Replace `b2 > TILEOP_SIZE` with a real structural check based on payload budget and derived counts.

## WARNING

### 3. The current tests do not verify the 256-byte TileOp path; the local parser/encoder tests fail immediately

**Evidence**

- `tiles-compositor/test_tileop` fails under a clean `-DK_SQ=36` build:
  - `test_synthetic_normal_tile` still expects `counts.r_cnt == 57` in [test_tileop.cpp](../../tiles-maxxing/campaign-sqrt-36/tiles-compositor/test/test_tileop.cpp#L55), but the current parser derives a much larger `r_cnt` from the 256-byte layout.
- `tile-cpp/test_face_encode` also fails under a clean `-DK_SQ=36` build:
  - `test_parser_helpers` still expects handcrafted `r_cnt == 1` in [test_face_encode.cpp](../../tiles-maxxing/campaign-sqrt-36/tile-cpp/tests/test_face_encode.cpp#L188), but the current encoder/parser pair derives `65`.

**Why It Matters**

This is not just “missing tests”. The tests that are supposed to defend the widened TileOp semantics are stale and currently red. That means the project has no passing local proof that the parser/encoder/compositor agree on the current 256-byte format.

**Proposed Fix**

- Update the test expectations to the actual 256-byte encoding contract, or change the contract back to the 128-byte semantics if those expectations are intended.
- Add a dedicated negative test that rejects malformed odd-residual headers.
- Add one end-to-end cross-check that feeds a real `K_SQ=36` TileOp from the C++ path through the compositor parser and asserts exact face counts/groups/h1 decoding.

## VERIFIED

- **Shared constants are aligned across the active paths.** CUDA, C++, and compositor all use `TILE_SIDE = 256`, `TILEOP_SIZE = 256`, `TILEOP_HEADER_BYTES = 3`, `EMPTY_OFFSET = 3`, and `OVERFLOW_SENTINEL = 0xFF` in the active `campaign-sqrt-36` tree.
- **The 257x257 shared-boundary convention is implemented.** Both CUDA and C++ face membership logic include `tile_row <= TILE_SIDE` / `tile_col <= TILE_SIDE`, so the shared endpoint row/column is not dropped.
- **Norm arithmetic is wide enough for the intended operating range.** CUDA MR computes `uint64_t norm = ua * ua + ub * ub` in [kernel_mr.cu](../../tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_mr.cu#L58), and the active radius range stays well below `2^63`.
- **The known dead `atomicCAS` in GPU path compression is performance-only.** `atomic_find_root()` in [kernel_uf.cu](../../tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_uf.cu#L46) does not actually split paths, but it still walks to the true root, and the final pass stores `tile_parent[i] = atomic_find_root(...)`, so I did not find a correctness break from that bug.
- **I did not find a diagonal-specific composition bug in the active path.** The current grid/compositor strategy relies on sub-diagonal tiles and ordinary L/R composition, which is consistent with `grid_spec.md`; no extra diagonal stitching code is present, and no contradictory special-casing was found.

## Bottom Line

The `K_SQ=36` pipeline is not yet sound for verdict production. The blocking issue is still the same one seen in the prior campaign: structurally valid overflow is handled by deleting the tile’s connectivity. Even if that were fixed, the malformed-TileOp boundary is still too weak for the widened 256-byte format, and the local tests are not currently validating the live encoding rules.
