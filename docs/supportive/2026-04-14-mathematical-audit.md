---
title: Mathematical Soundness Audit
date: 2026-04-14
engine: codex
type: audit
status: complete
refs:
  - docs/tile_spec.md
  - docs/grid_spec.md
  - docs/tile_operations.md
  - docs/tile_internals_cuda.md
  - docs/compositor_spec.md
  - docs/campaign_spec.md
  - tile_cuda_multi_kernel/src/kernel_sieve.cu
  - tile_cuda_multi_kernel/src/kernel_mr.cu
  - tile_cuda_multi_kernel/src/kernel_compact.cu
  - tile_cuda_multi_kernel/src/kernel_uf.cu
  - tile_cuda_multi_kernel/src/kernel_face_encode.cu
  - tiles-compositor/src/compositor.cpp
  - tiles-compositor/src/grid.cpp
  - tiles-compositor/src/campaign.cpp
---

# Mathematical Soundness Audit — tiles-maxxing Gaussian Moat Pipeline

## Final Verdict

**FAIL** for paper-grade claims in the current repository state.

The main CUDA/compositor geometry is substantially better than the pre-fix state, and I did not find a concrete arithmetic bug in the sieve, compaction, or L/R row-shift logic after the `decode_group_id` and `r-q` fixes. But there are still correctness-breaking gaps:

1. **Overflow/malformed tiles are deleted, not preserved** during campaign execution. `campaign.cpp` replaces overflow or malformed TileOps with empty tiles instead of the spec-mandated extended reprocessing path, which makes any run with `overflow_count > 0` mathematically unsound for MOAT claims.
2. **K5 can silently truncate face-prime data** before poisoning. If a face exceeds `MAX_FACE_PRIMES_PER_FACE`, the kernel clamps the count instead of setting overflow, so downstream port clustering can run on corrupted input.
3. **The diagonal/octant proof is not closed in code.** The specs now rely on “all generated sub-diagonal tiles are processed” for cross-diagonal coverage, but the compositor still applies a dead-tile predicate and skips fully sub-diagonal tiles. That is a direct implementation/spec mismatch on the mathematical argument that justifies one-octant sufficiency.

Because of those gaps, the observed non-monotone alternation and the 950M early-SPANNING anomaly are not fully explained away by the codebase.

## What I Checked

- Read the authoritative specs: `docs/tile_spec.md`, `docs/grid_spec.md`, `docs/tile_operations.md`, `docs/tile_internals_cuda.md`, `docs/compositor_spec.md`, `docs/campaign_spec.md`.
- Traced the executable paths in:
  - `tile_cuda_multi_kernel/src/kernel_sieve.cu`
  - `tile_cuda_multi_kernel/src/kernel_mr.cu`
  - `tile_cuda_multi_kernel/src/kernel_compact.cu`
  - `tile_cuda_multi_kernel/src/kernel_uf.cu`
  - `tile_cuda_multi_kernel/src/kernel_face_encode.cu`
  - `tiles-compositor/src/compositor.cpp`
  - `tiles-compositor/src/grid.cpp`
  - `tiles-compositor/src/campaign.cpp`
  - `tiles-compositor/src/tileop_parse.cpp`
- Cross-checked relevant headers/constants in:
  - `tile_cuda_multi_kernel/include/gpu_constants.cuh`
  - `tile_cuda_multi_kernel/include/gpu_math.cuh`
  - `tiles-compositor/include/grid.h`
  - `tiles-compositor/include/tileop_parse.h`
- Read supporting analysis where it directly bore on correctness:
  - `docs/supportive/2026-04-10-mr-edge-analysis-codex.md`
  - `docs/supportive/2026-04-13-k36-campaign-postmortem.md`
  - `tiles-compositor/docs/supportive/2026-04-12-compositor-logic.md`

## A. CUDA Kernels

### A1. `kernel_sieve.cu`

**PASS**

- The sieve is applied to the correct quantity: rows are absolute `a = a_lo - COLLAR + tid`, columns start at `b_start = b_lo - COLLAR` ([kernel_sieve.cu:126-141](../../tile_cuda_multi_kernel/src/kernel_sieve.cu)).
- Parity elimination is correct for off-axis Gaussian primes: same-parity `(a,b)` pairs are marked composite via the alternating bit pattern ([kernel_sieve.cu:23-30](../../tile_cuda_multi_kernel/src/kernel_sieve.cu)).
- Split-prime coverage is complete: for each `p == 1 mod 4`, both residue classes `r` and `-r` are marked, with the duplicate skipped only when `r == -r` ([kernel_sieve.cu:31-47](../../tile_cuda_multi_kernel/src/kernel_sieve.cu)).
- Inert-prime coverage is correct: `p == 3 mod 4` only marks `b == 0 mod p` when `p | a` ([kernel_sieve.cu:49-57](../../tile_cuda_multi_kernel/src/kernel_sieve.cu)).
- Barrett reduction usage is consistent with the host-built `(p, root, mu)` tables and the marking helper ([gpu_math.cuh:23-50](../../tile_cuda_multi_kernel/include/gpu_math.cuh), [main.cu:100-149](../../tile_cuda_multi_kernel/src/main.cu)).

Edge note:
- `MAX_CANDIDATES_GPU` is still a hard clamp, not a poison path ([kernel_sieve.cu:145-161](../../tile_cuda_multi_kernel/src/kernel_sieve.cu)). At K_SQ=40 operating radii this is backed by the documented census, so I do not count it as a live bug here.

### A2. `kernel_mr.cu`

**CONCERN**

- The norm is computed correctly as `|a|^2 + |b|^2` for off-axis points ([kernel_mr.cu:63-67](../../tile_cuda_multi_kernel/src/kernel_mr.cu)).
- Axis points are routed through the Gaussian-axis rule, not the norm path ([kernel_mr.cu:56-60](../../tile_cuda_multi_kernel/src/kernel_mr.cu), [gpu_math.cuh:224-234](../../tile_cuda_multi_kernel/include/gpu_math.cuh)).
- The bitmap write path is straightforward and correct if `is_prime_norm_fj64_262k_gpu()` is correct ([kernel_mr.cu:67-69](../../tile_cuda_multi_kernel/src/kernel_mr.cu)).

Concern:
- The repo does not contain an in-tree proof or verifier for the embedded `FJ64_262K_TABLE`; correctness is inherited from the table comment and external provenance (`fj64_262k_table.h:1-2`) plus end-to-end cross-validation, not from a direct in-repo derivation. For a paper-grade claim, that is weaker than I would accept without an independent table verifier or a deterministic reference recheck path.

### A3. `kernel_compact.cu`

**PASS**

- The kernel counts set bits per row, performs an exclusive prefix scan, writes the `(ACTIVE_ROWS+1)` row-boundary array, and scatters prime positions from the bitmap in the same per-row order ([kernel_compact.cu:54-107](../../tile_cuda_multi_kernel/src/kernel_compact.cu)).
- Row boundaries are correct: the last thread writes `row_prefix[ACTIVE_ROWS] = row_prefix[last] + row_count_last` ([kernel_compact.cu:71-73](../../tile_cuda_multi_kernel/src/kernel_compact.cu)).
- I did not find a duplication or loss path in normal operation. The scatter order matches the count order.

### A4. `kernel_uf.cu`

**PASS**

- The backward offset table is generated from the exact predicate `dr^2 + dc^2 <= K_SQ` with the forward half-plane removed; for `K_SQ=40` the compile-time count is 64 ([gpu_constants.cuh:21-31,77,91-92](../../tile_cuda_multi_kernel/include/gpu_constants.cuh), [main.cu:157-178](../../tile_cuda_multi_kernel/src/main.cu)).
- Union-find only scans backward neighbors, so every undirected edge is considered exactly once ([kernel_uf.cu:111-130](../../tile_cuda_multi_kernel/src/kernel_uf.cu)).
- The atomicCAS union path is correct for connectivity: roots only ever point toward smaller roots, so cycles cannot form ([kernel_uf.cu:57-75](../../tile_cuda_multi_kernel/src/kernel_uf.cu)).
- The final pass resolves each parent to a root ([kernel_uf.cu:134-137](../../tile_cuda_multi_kernel/src/kernel_uf.cu)).

Note:
- `atomic_find_root()`'s path-splitting CAS is ineffective because it CASes against `r` rather than the current parent ([kernel_uf.cu:48-52](../../tile_cuda_multi_kernel/src/kernel_uf.cu)). That is a performance bug, not a correctness bug, because the traversal still reaches the root.

### A5. `kernel_face_encode.cu`

**CONCERN**

Correct pieces:
- Face-zone membership is consistent with the 257x257 shared-boundary convention:
  - `FACE_I`: `tile_row in [0, COLLAR-1]`
  - `FACE_O`: `tile_row in [TILE_SIDE-COLLAR+1, TILE_SIDE]`
  - `FACE_L`: `tile_col in [0, COLLAR-1]`
  - `FACE_R`: `tile_col in [TILE_SIDE-COLLAR+1, TILE_SIDE]`
  ([kernel_face_encode.cu:28-54](../../tile_cuda_multi_kernel/src/kernel_face_encode.cu)).
- `h`/`depth` encoding is correct by face, and `encode_group_byte()` only steals bit 7 on L/R faces ([kernel_face_encode.cu:56-88](../../tile_cuda_multi_kernel/src/kernel_face_encode.cu)).
- The 127-group limit is enforced at encode time; tiles with `group_count > 127` are poisoned ([kernel_face_encode.cu:399-407](../../tile_cuda_multi_kernel/src/kernel_face_encode.cu)).
- The decode-group-id fix is correctly mirrored in compositor code: I/O faces use raw bytes; only L/R faces call `decode_group_id()` ([compositor.cpp:503-512,601-610](../../tiles-compositor/src/compositor.cpp), [tileop_parse.h:30-35](../../tiles-compositor/include/tileop_parse.h)).

Concern:
- **Silent per-face prime truncation exists.** Face-prime appends beyond `MAX_FACE_PRIMES_PER_FACE` are dropped (`slot < MAX_FACE_PRIMES_PER_FACE`), and then the count is clamped back to 256 without setting `scratch->overflow` ([kernel_face_encode.cu:207-224](../../tile_cuda_multi_kernel/src/kernel_face_encode.cu)). That means port clustering can run on a truncated face-prime list and emit a normal-looking TileOp rather than a poison sentinel.
- This is already documented as a real correctness break at K_SQ=36 (`2026-04-13-k36-campaign-postmortem.md:92-99`).

For K_SQ=40:
- I did not find evidence that the 256-face-prime cap is exceeded at operating radii, so I am not escalating this to FAIL for the current K40 record. But the absence of a poison path makes K5 mathematically fragile.

## B. Compositor

### B6. `compositor.cpp`

**CONCERN**

#### I/O matching

- The direction is correct: `FACE_O` of row `r` matches `FACE_I` of row `r+1` ([compositor.cpp:348-379](../../tiles-compositor/src/compositor.cpp)).
- But the implementation silently uses `min(o_cnt, i_cnt)` instead of enforcing equality ([compositor.cpp:371-379](../../tiles-compositor/src/compositor.cpp)).
- The spec explicitly treats I/O count equality as an invariant worth asserting ([compositor_spec.md:555-579](../../docs/compositor_spec.md)).

Impact:
- If one side is malformed but still parseable, the compositor quietly drops unmatched shared-boundary groups, causing false MOATs instead of failing loudly.

#### L/R matching

- The `r-q` and `r-q-1` row mapping is now correct ([compositor.cpp:423-489](../../tiles-compositor/src/compositor.cpp)).
- The `h1` predicates are also correct:
  - primary: `hl == hr + f` ([compositor.cpp:446-449](../../tiles-compositor/src/compositor.cpp))
  - secondary: `hl + (S-f) == hr` ([compositor.cpp:482-485](../../tiles-compositor/src/compositor.cpp))
- This matches the derivation in `2026-04-12-compositor-logic.md`.

#### Inner/outer boundary marking

- The incremental reachability bookkeeping is correct as far as implemented: `mark_inner`, `mark_outer`, and `unite` OR the reach bits on the current roots and set `spanning_detected_` when a root has both bits ([compositor.cpp:248-283](../../tiles-compositor/src/compositor.cpp)).
- The I/O decode-group fix is correctly applied in boundary collectors: FACE_I and FACE_O now use raw group bytes ([compositor.cpp:503-512,601-610](../../tiles-compositor/src/compositor.cpp)).

Remaining concern:
- The code relies heavily on `is_tile_dead()` to skip diagonal-adjacent tiles in matching and boundary collection ([compositor.cpp:312,351,410,427,462,498,532,562,596,624,654,695,719,766,797](../../tiles-compositor/src/compositor.cpp)). That is in direct tension with the spec’s later “all generated tiles live” cross-diagonal argument; see C8/D11.

### B7. `campaign.cpp`

**FAIL**

- The campaign path does **not** implement the required overflow fallback. It replaces overflow or malformed tiles with empty TileOps:
  - tower 0 CPU path: [campaign.cpp:610-621](../../tiles-compositor/src/campaign.cpp)
  - CUDA stream path: [campaign.cpp:743-770](../../tiles-compositor/src/campaign.cpp)
  - CUDA burst path: [campaign.cpp:913-946](../../tiles-compositor/src/campaign.cpp)
  - C++ path: [campaign.cpp:1013-1037](../../tiles-compositor/src/campaign.cpp)
- The specs require the opposite:
  - “every overflow tile in a burst has a corresponding extended TileOp before compositor ingestion” ([campaign_spec.md:1128-1141](../../docs/campaign_spec.md))
  - “No false moats ... All overflow tiles are resolved via extended TileOps” ([campaign_spec.md:1141-1142](../../docs/campaign_spec.md))

Impact:
- Replacing an overflow tile with an empty tile deletes connectivity. That cannot create a false SPAN, but it **can create a false MOAT**.
- Therefore any campaign result with `overflow_count > 0` is not trustworthy.

Additional note:
- This does not by itself explain the suspicious 950M early-SPANNING anomaly, because deleting edges biases toward MOAT, not SPANNING.

## C. Grid Geometry

### C8. Annular tiling completeness

**CONCERN**

What looks sound:
- Tower placement and delta computation are internally consistent in code ([grid.cpp:47-69,73-126](../../tiles-compositor/src/grid.cpp)).
- Variable tower height addresses the known fixed-32 false-SPANNING problem near 45 degrees ([grid_spec.md:78-84](../../docs/grid_spec.md)).

The problem:
- The mathematical justification for one-octant sufficiency now depends on **processing sub-diagonal tiles**:
  - `campaign_spec.md` says “Sub-diagonal tiles are processed by CUDA and provide connectivity via standard face matching” ([campaign_spec.md:232-234](../../docs/campaign_spec.md)).
  - `grid_spec.md` says “All tiles in generated towers — including sub-diagonal tiles — are processed” ([grid_spec.md:644-648](../../docs/grid_spec.md)).
- The implementation contradicts that by applying a dead-tile predicate and skipping fully sub-diagonal tiles:
  - dead-tile predicate: [grid.h:14-18](../../tiles-compositor/include/grid.h)
  - compositor skips dead tiles in matching/boundary collection throughout: [compositor.cpp](../../tiles-compositor/src/compositor.cpp)

That is not a cosmetic spec drift. It changes the actual graph being composed near the diagonal. I cannot prove from the current code that all cross-diagonal paths needed for the moat argument survive this skip rule.

### C9. Tower `j=0` edge case

**PASS**

- Tower `j=0` is processed by the C++ reference path in campaign mode ([campaign.cpp:597-624](../../tiles-compositor/src/campaign.cpp)), which avoids the axis-degenerate CUDA case.
- L/R matching is never attempted for `j=0` (`match_lr_with_previous` returns immediately when `j <= 0`) ([compositor.cpp:393-397](../../tiles-compositor/src/compositor.cpp)).
- Inner boundary collection correctly excludes the y-axis L-face as annulus boundary by returning when `j <= 0` after processing the row-0 I-face ([compositor.cpp:495-519](../../tiles-compositor/src/compositor.cpp)).

I did not find a special-case code path in towers `j=1..30` that would obviously fabricate a false SPAN near the y-axis.

## D. Mathematical Correctness

### D10. Tsuchimura upper-bound trick

**PASS**

- The implementation does mark inner-boundary ports unconditionally via row-0 I-face groups and staircase-exposed L-face segments ([compositor.cpp:495-585](../../tiles-compositor/src/compositor.cpp)).
- It marks outer-boundary ports via top-row O-face groups plus staircase-exposed R/L segments ([compositor.cpp:588-812](../../tiles-compositor/src/compositor.cpp)).
- The annulus width premise is satisfied: `W_RADIAL = 8192` is vastly larger than `sqrt(40) ~ 6.32`, so a single step cannot jump across the entire band.

Qualification:
- This passes **assuming** the staircase boundary really is the intended outer/inner annulus proxy. My remaining reservation there is the diagonal/octant issue, not the band width itself.

### D11. Octant symmetry

**CONCERN**

- The current proof strategy is “extended tower generation past `y=x` plus standard face matching; no explicit diagonal stitching” ([grid_spec.md:623-650](../../docs/grid_spec.md)).
- The codebase, however, still distinguishes “dead” sub-diagonal tiles and removes them from composition ([grid.h:14-18](../../tiles-compositor/include/grid.h)).
- `campaign_spec.md` goes further and explicitly says there is **no dead-tile predicate** and that every generated tile is processed ([campaign_spec.md:257-267](../../docs/campaign_spec.md)).

This is the unresolved mathematical gap in the current implementation:
- If the proof of one-octant sufficiency requires sub-diagonal tiles to carry second-octant connectivity, the compositor cannot skip them.
- If the compositor is allowed to skip them, the proof needs to be rewritten around that smaller domain.

I do not have enough evidence to label the one-octant method flatly wrong, but I do have enough to say it is not yet closed to paper standard.

## Per-Item Summary

| Item | Verdict | Notes |
|------|---------|-------|
| A1 Sieve | PASS | Barrett split/inert sieve looks correct. |
| A2 MR | CONCERN | Correct norm path, but FJ64 table is trusted rather than independently verified in-repo. |
| A3 Compact | PASS | Prefix-sum compaction is correct. |
| A4 UF | PASS | Backward offsets and lock-free union logic are connectivity-correct. |
| A5 FaceEncode | CONCERN | Correct face semantics, but silent face-prime truncation exists before poison. |
| B6 Compositor | CONCERN | L/R matching is correct post-fix; I/O matching still silently truncates mismatched counts. |
| B7 Campaign | FAIL | Overflow/malformed tiles are replaced with empty, violating the no-false-moat contract. |
| C8 Grid completeness | CONCERN | Dead-tile skipping conflicts with the “all sub-diagonal tiles live” proof. |
| C9 Tower j=0 | PASS | Axis special-casing looks sound. |
| D10 Tsuchimura bound | PASS | Boundary marking and width assumptions are implemented consistently. |
| D11 Octant symmetry | CONCERN | Mathematical argument still not closed in implementation. |

## Bottom Line

I would trust the current pipeline for **engineering iteration** at K_SQ=40, with the important caveat that `overflow_count` must stay zero and anomalies should still be treated as suspect.

I would **not** trust it yet for a record-grade or paper-grade moat claim. The minimum bar before that is:

1. Implement the extended TileOp fallback instead of empty-tile replacement.
2. Turn K5 face-prime overflow into an explicit poison path, not silent truncation.
3. Resolve the diagonal proof/implementation mismatch: either truly process all generated sub-diagonal tiles, or rewrite and prove the dead-tile-skipping version.
4. Add an invariant check in I/O matching (`o_cnt == i_cnt`) so boundary corruption fails loudly.
