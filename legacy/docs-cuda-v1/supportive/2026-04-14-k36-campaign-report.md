---
date: 2026-04-14
type: campaign-report
status: complete
campaign: campaign-sqrt-36
result: MOAT
refs: [docs/supportive/2026-04-13-k36-campaign-postmortem.md, docs/supportive/campaign_sqrt_36_plan.md, docs/supportive/2026-04-14-k40-vs-k36-campaign-delta.md, docs/supportive/2026-04-08-tsuchimura-eviction-relevance.md]
---

# K_SQ=36 Campaign Report

## Result Summary

| Field | Value |
|-------|-------|
| K_SQ | 36 |
| Step distance | sqrt(36) = 6 |
| R | 800,000,000 |
| Verdict | **MOAT** |
| Tiles processed | 79,808,256 |
| Towers processed | 2,209,733 |
| Overflow count | 0 |
| Wall time | 648 s |
| Campaign throughput | ~124K tiles/sec |
| Kernel benchmark throughput | 151K tiles/sec |
| GPU | RTX 4090 (sm_89, 128 SMs, 24 GB) |

K_SQ=36 at R=800M produces a Gaussian moat: no connected path of Gaussian primes (with step distance <= 6) spans the annular region. Zero overflow tiles across all 79.8M tiles processed. This is the first trustworthy K_SQ=36 campaign verdict.

## Prior Art

### Tsuchimura's result

Tsuchimura (2005) proved computationally that sqrt(26) suffices for a Gaussian moat for sufficiently large R. Our project extends the search to larger step distances where moat/spanning transitions are more nuanced.

### Tsuchimura's eviction optimization

Tsuchimura's prime eviction technique keeps the working set of primes bounded during traversal. Our TileOp architecture fully subsumes this at the prime level: the CUDA kernel identifies connected components within a tile, emits a fixed-size TileOp transfer operator, and immediately discards raw prime data. The global compositor never sees or stores a raw Gaussian prime. The eviction *principle* (discarding data behind the sweep window) remains relevant for compositor-level optimization (streaming TileOps rather than materializing the full array), but the prime-level technique is structurally unnecessary in our pipeline. (Source: `docs/supportive/2026-04-08-tsuchimura-eviction-relevance.md`)

### K_SQ=40 baseline

The K_SQ=40 pipeline was verified at scale before K36 work began:

- R=600M: SPANNING at tower 931 (instant).
- R=875M: SPANNING at ~660K towers (188.8s, 27% traversal).
- R=900M: MOAT (756.5s). First moat in the K_SQ=40 R-sweep.
- R=950M: SPANNING (7.6s) -- non-monotonic island.
- R=975M through R=1,125M: MOAT across all values.
- Full sweep: 68 runs, 55 unique R values, zero overflows across ~1.1 billion tiles.

(Source: `docs/supportive/2026-04-13-r-sweep-results.md`, `AGENTS.md`)

## Problem Statement

K_SQ=36 is structurally harder than K_SQ=40 for the GPU pipeline. The difficulty is counter-intuitive: a *smaller* step distance produces *more* ports per face, not fewer.

### Why more ports at smaller K_SQ

K_SQ=36 has COLLAR=6 (ceil(sqrt(36))=6) versus COLLAR=7 for K_SQ=40. The connectivity threshold is tighter: two primes must be within distance 6 to connect, versus ~6.32 for K_SQ=40. With tighter connectivity, union-find merges fewer primes into the same component. More components survive dead-end pruning. More distinct groups appear on each face.

At R=80M (the April 13 attempt), prime density in the sieve domain is approximately 1/ln(R^2) ~ 1/36.4. For a 257x257 tile, that is roughly 1,810 primes. With COLLAR=6, face regions contain ~6x257 = 1,542 lattice points per face, yielding ~42 face primes per face on average. With K_SQ=36 connectivity, these cluster into more ports than the GPU caps allowed.

### The overflow problem

The GPU-side per-face caps were tuned for K_SQ=40 geometry:

| Constant | K_SQ=40 value | K_SQ=36 needed |
|----------|--------------|----------------|
| MAX_FACE_PORTS_GPU | 32 | 48 |
| MAX_TOTAL_PORTS_GPU | 128 | 192 |
| TILEOP_SIZE | 128 | 256 |
| TILEOP_PAYLOAD_BYTES | 125 | 253 |

At K_SQ=40, overflow rates were under 0.01%. At K_SQ=36 with the K40 caps, 22.4% of tiles overflowed -- a configuration bug, not a rare edge case.

### Port fragmentation

The fragmentation cascades through three GPU caps:

1. `MAX_FACE_PRIMES_PER_FACE = 256`: if any single face exceeds 256 primes, the count is clamped and excess primes are lost (in the old code, silently).
2. `MAX_FACE_PORTS_GPU = 32` per face, `MAX_TOTAL_PORTS_GPU = 128` total: if any face exceeds 32 ports or total exceeds 128, `scratch->overflow` is set.
3. `TILEOP_PAYLOAD_BYTES = 125`: even with valid group counts, if total packed data exceeds 125 bytes, overflow is triggered.

Additionally, the 128-byte TileOp record could not accommodate the denser port topology. The payload budget formula `r_cnt = (125 - o_cnt - i_cnt - 2*l_cnt) / 2` left too little room for R-face ports when the other faces were already dense.

## Failure History

### April 13 campaign attempt

**Parameters:** R=80,015,782, K_SQ=36, RTX 4090 (vast.ai), burst-size=4096.

**Grid:** 221,039 towers, ~8M total tiles.

**Outcome:** Killed at 36% completion (tower 80,000 of 221,039) after 24 minutes.

**Three critical issues:**

1. **O(N^2) compositor scaling.** `has_spanning()` called after every tower, performing linear scans over `inner_members_` and `outer_members_` vectors that grow with every tower. Per-5K-tower batch time grew from 7.2s (0-5K) to 199.3s (75-80K). GPU throughput was constant at 155K tiles/sec; the bottleneck was CPU-side compositor. Extrapolated completion: ~3.5 hours. Expected time from GPU throughput alone: ~5 minutes.

2. **22.4% overflow tile rate.** 651,413 overflow/malformed tile warnings in 80,000 towers (~2.9M tiles). Overflows began at tower 1, row 0 -- immediate onset, not gradual. Every overflow tile was replaced with an empty TileOp, silently deleting its connectivity information.

3. **TileOp parse failure.** Earlier attempt crashed with `FATAL parse_counts: off_I=3 off_L=0 off_R=0 bytes=[03 00 00 00] budget=125`. The byte pattern `[03, 00, 00, 00]` is consistent with a partially-initialized TileOp where byte[0] was written but bytes[1-2] were left at zero. The BUG-5 fix in campaign.cpp (lines 556-587) catches malformed offset triples and replaces them with empties.

**Verdict reliability:** No verdict from the pipeline could be trusted at K_SQ=36 in that state. MOAT verdicts were unreliable due to 22% data loss. SPANNING verdicts would have been reliable (empty tiles only remove connections, never add them) but unlikely to be reached given the extent of data loss.

### Timing degradation table (April 13)

| Towers processed | Time per 5K batch (s) | ms/tower | Overhead vs baseline |
|---|---|---|---|
| 0-5K | 7.2 | 1.4 | 1.0x |
| 5-10K | 13.1 | 2.6 | 1.9x |
| 15-20K | 29.5 | 5.9 | 4.2x |
| 30-35K | 58.5 | 11.7 | 8.4x |
| 45-50K | 105.5 | 21.1 | 15.1x |
| 60-65K | 147.7 | 29.5 | 21.1x |
| 70-75K | 181.0 | 36.2 | 25.9x |
| 75-80K | 199.3 | 39.9 | 28.5x |

## Architecture Decisions

The implementation plan (`docs/supportive/campaign_sqrt_36_plan.md`) identified two incompatible 256-byte formats in the docs and three options for the TileOp size bump. The decisions made:

### Decision 1: Blanket 256B TileOp (Option B)

The plan recommended keeping 128B for the hot path and using 256B only for overflow reprocessing via `ExtendedTileSideTable`. The actual implementation chose Option B: blanket 256B for all K36 TileOps. This unifies TILEOP_SIZE and TILEOP_EXT_SIZE at 256, eliminating the extended-path branch in compositor logic entirely.

**Rationale:** At K36 geometry, overflow is not a rare edge case but a structural property. Maintaining two record sizes adds complexity for a path that would be exercised on a large fraction of tiles. The 2x I/O increase (256B vs 128B per tile) is modest: at 8.2M tiles, that is ~2.1 GB vs ~1.1 GB -- well within 4090 memory and disk throughput.

### Decision 2: Geometry-derived caps

Port caps were raised to geometry-derived values rather than arbitrary doublings:

| Constant | K40 | K36 | Multiplier |
|----------|-----|-----|-----------|
| MAX_FACE_PORTS_GPU | 32 | 48 | 1.5x |
| MAX_TOTAL_PORTS_GPU | 128 | 192 | 1.5x |
| TILEOP_PAYLOAD_BYTES | 125 | 253 | 2.02x |

MAX_FACE_PRIMES_PER_FACE was kept at 256 (unchanged). MAX_GROUPS_GPU was kept at 127 -- the 7-bit structural limit imposed by the L/R h1 bit-steal encoding. If K36 ever exceeds 127 groups, that is a format redesign trigger, not a cap-tuning task.

### Decision 3: K5 poison fix

The face-prime overflow in K5 (`kernel_face_encode.cu`) was changed from silent truncation to explicit overflow poisoning. In K40, if face primes exceeded MAX_FACE_PRIMES_PER_FACE, the count was silently clamped and excess primes dropped. Downstream port clustering proceeded on the truncated list -- result was incorrect but no sentinel was emitted.

In K36, face prime overflow now sets `scratch->overflow = 1u` before clamping. This propagates through `prune_dead_ends_gpu_k5` (which sets `group_count = MAX_GROUPS_GPU + 1` and returns early) to `encode_tileop_gpu_k5` (which emits `OVERFLOW_SENTINEL` for all bytes). No silent corruption is possible.

### Decision 4: Codebase split

The `tiles-maxxing/` directory was split into `campaign-sqrt-40/` (verified K40 baseline) and `campaign-sqrt-36/` (K36 surgical changes). This preserves the verified K40 codebase as an immutable reference while allowing K36-specific modifications without risking K40 regression.

### Decisions NOT made

- K36-conditional `#if K_SQ_VAL <= 36` cap gating was proposed in the plan but not implemented. The caps are set directly in the K36 branch.
- TileOp_wide reprocessing via C++ host path (spec S4.7, `ExtendedTileSideTable`) was not implemented. The blanket 256B approach made it unnecessary.
- Incremental spanning check was not implemented as part of K36. The O(N^2) fix (incremental reachability tracking with inner/outer bitmasks per UF root) was already committed separately (`bb10035`), before the K36 campaign work.

## Changes Made

All changes in commit `2b54b6d` ("k36: 256B TileOp, raised caps, K5 overflow poison fix"). Nine files changed across three components.

### GPU kernel: `tile_cuda_multi_kernel/include/gpu_constants.cuh`

| Constant | Before | After |
|----------|--------|-------|
| MAX_FACE_PORTS_GPU | 32 | 48 |
| MAX_TOTAL_PORTS_GPU | 128 | 192 |
| TILEOP_SIZE | 128 | 256 |
| TILEOP_PAYLOAD_BYTES | 125 | 253 |

All other constants unchanged. Derived constants (COLLAR=6, SIDE_EXP=269, NUM_BACKWARD_OFFSETS=56, LAST_WORD_VALID_BITS=13) verified by compile-time static_assert at line 93-94.

### GPU kernel: `tile_cuda_multi_kernel/include/gpu_types.cuh`

Static assert changed from `static_assert(sizeof(TileOp) == TILEOP_SIZE, "TileOp must stay 128 bytes")` to three asserts:
- `static_assert(sizeof(TileOp) == 256, "TileOp must be 256 bytes")`
- `static_assert(TILEOP_SIZE == TILEOP_HEADER_BYTES + TILEOP_PAYLOAD_BYTES, ...)` -- header+payload accounting identity
- `static_assert(MAX_TOTAL_PORTS_GPU >= 4 * MAX_FACE_PORTS_GPU, ...)` -- capacity invariant

### GPU kernel: `tile_cuda_multi_kernel/src/kernel_face_encode.cu`

K5 face-prime overflow fix at lines 221-227. Before:

```cpp
if (face_prime_counts[tid] > MAX_FACE_PRIMES_PER_FACE) {
    face_prime_counts[tid] = MAX_FACE_PRIMES_PER_FACE;
}
```

After:

```cpp
if (face_prime_counts[tid] > MAX_FACE_PRIMES_PER_FACE) {
    scratch->overflow = 1u;
}
face_prime_counts[tid] = min(face_prime_counts[tid],
                             static_cast<uint32_t>(MAX_FACE_PRIMES_PER_FACE));
```

Overflow flag is set *before* clamping. Propagation chain: `scratch->overflow` -> `prune_dead_ends_gpu_k5` (group_count = MAX_GROUPS_GPU + 1, early return) -> `encode_tileop_gpu_k5` (group_count > 127 -> all bytes = OVERFLOW_SENTINEL).

### GPU kernel: `tile_cuda_multi_kernel/src/main.cu`

Comment-only changes in `run_campaign()` and stream protocol documentation: `128 bytes` -> `256 bytes` at lines 540, 691. No code logic changes -- output record size is derived from `sizeof(TileOp)` at compile time.

### C++ reference: `tile-cpp/include/constants.h`

`TILEOP_SIZE`: 128 -> 256. `TILEOP_PAYLOAD_BYTES` is derived as `TILEOP_SIZE - TILEOP_HEADER_BYTES`, so 125 -> 253 automatically.

### C++ reference: `tile-cpp/include/types.h`

TileOp struct comment: `128 bytes` -> `256 bytes`. No code change.

### Compositor: `tiles-compositor/include/types.h`

`TILEOP_SIZE`: 128 -> 256. `TILEOP_PAYLOAD_BYTES`: 125 -> 253. `TILEOP_EXT_SIZE` remains 256 (was already 256). `TILEOP_EXT_PAYLOAD_BYTES` remains 253. Standard and extended sizes are now unified.

### Compositor: `tiles-compositor/src/campaign.cpp`

Four comment updates: `128 bytes` -> `256 bytes` at the TileOp struct comment, the raw TileOp read function, stream protocol documentation, and burst output read section. No code logic changes -- all sizes derived from `TILEOP_SIZE`.

### Python tools: `tiles-compositor/tools/extract_tileops.py`

| Constant | Before | After |
|----------|--------|-------|
| TILEOP_SIZE | 128 | 256 |
| TILEOP_PAYLOAD_BYTES | 125 | 253 |
| CUDA_RECORD_SIZE | 148 | 276 |
| CUDA_RECORD_STRUCT | `<qqI128s` | `<qqI256s` |
| Docstring | "128-byte TileOps from 148-byte CUDA binary records" | "256-byte TileOps from 276-byte CUDA binary records" |

CUDA_RECORD_SIZE = 20 bytes (int64 a_lo + int64 b_lo + uint32 prime_count) + 256 bytes TileOp = 276.

## Validation

### Campaign results (R=800M)

| Metric | Value |
|--------|-------|
| R | 800,000,000 |
| Verdict | MOAT |
| Total tiles | 79,808,256 |
| Total towers | 2,209,733 |
| Overflow tiles | 0 |
| Malformed tiles | 0 |
| Wall time | 648 s |
| Campaign throughput | ~124K tiles/sec |

Zero overflow across all 79.8M tiles. The raised caps (MAX_FACE_PORTS_GPU: 32->48, MAX_TOTAL_PORTS_GPU: 128->192) and expanded payload budget (253 vs 125 bytes) completely eliminated the 22.4% overflow rate observed in the April 13 attempt.

### K_SQ=40 baseline comparison

The K_SQ=40 R-sweep (68 runs, 55 R values) produced zero overflows across ~1.1 billion tiles. The K_SQ=36 campaign at R=800M matches this zero-overflow standard.

### Correctness gate

The plan specified `unresolved_overflow_count == 0` as the minimum correctness gate. The campaign achieved this: zero overflow tiles, zero malformed offset triples, zero empty-tile substitution for overflow.

## Performance Profile

### Throughput

| Metric | Value |
|--------|-------|
| Kernel benchmark (GPU only) | 151K tiles/sec |
| Campaign effective rate | ~124K tiles/sec |
| Utilization (campaign/benchmark) | ~82% |

The 18% gap between kernel benchmark and campaign effective rate is consistent with compositor overhead observed in K40 campaigns. The K40 pipeline showed a 14% gap (134K vs 155K tiles/sec) at 28K tower burst sizes. The slightly wider gap for K36 is expected: 256-byte TileOps double the pipe I/O and compositor parse volume per tile.

### Comparison with K_SQ=40

| Metric | K_SQ=40 (R=900M) | K_SQ=36 (R=800M) |
|--------|-------------------|-------------------|
| Tiles | 89,784,166 | 79,808,256 |
| Towers | 2,486,214 | 2,209,733 |
| Wall time | 756.5 s | 648 s |
| Overflows | 0 | 0 |
| Verdict | MOAT | MOAT |
| TileOp size | 128 B | 256 B |
| Raw TileOp I/O | ~11.5 GB | ~20.5 GB |

K36 at R=800M has fewer tiles/towers than K40 at R=900M (smaller grid at smaller R), resulting in a shorter wall time despite the 2x TileOp size increase.

### K5 shared memory impact

| Component | K40 | K36 |
|-----------|-----|-----|
| face_prime_lists | 8,192 B | 8,192 B |
| face_prime_counts | 16 B | 16 B |
| FaceScratchGPU | 2,052 B | 2,564 B |
| FaceDataGPU | 1,032 B | 1,032 B |
| **Total K5 smem** | **11,292 B** | **11,804 B** |

The 512 B increase comes from `FaceScratchGPU.raw_ports`: 192 raw ports x 8 B = 1,536 B (K36) vs 128 x 8 B = 1,024 B (K40). Both campaigns remain well under the 48 KB shared memory limit. On RTX 4090, the K5 block limit is set by register pressure (BLOCK_THREADS=288), not shared memory. No measurable occupancy impact from the cap raise.

### Per-tile GPU buffer sizes

| Buffer | K40 | K36 | Notes |
|--------|-----|-----|-------|
| d_cand_list | 24,576 B | 24,576 B | 6144 x 4 B, dominant Phase 1 cost |
| d_bitmap | 9,756 B | 9,684 B | 2439 vs 2421 words x 4 B |
| d_output | 128 B | 256 B | TileOp per tile |

The TileOp size increase (+128 B/tile) is immaterial relative to d_cand_list (24 KB/tile). At CHUNK_SIZE=20K tiles, d_output is 5.12 MB (K36) vs 2.56 MB (K40) -- negligible in 24 GB GPU memory.

## Open Questions

### 1. Byte-to-byte verification against C++

The campaign used the CUDA path exclusively. The C++ reference implementation (`tile-cpp/`) has been updated to 256B TileOps but no cross-validation run has confirmed byte-identical output between the CUDA and C++ encoders for K36 geometry. For K40, this verification was done at scale (300K tiles, byte-identical). A K36 equivalent is needed.

### 2. K40 backport of K5 poison fix

The K5 face-prime overflow silent truncation exists in the K40 codebase (`campaign-sqrt-40/`). At K40 geometry, this path is never reached in practice (average face primes ~43 vs cap 256), but the silent corruption is latent. Backporting the poison fix to K40 would make both codebases sound for all reachable paths.

### 3. Further radius extensions

R=800M is a single data point. The K40 R-sweep revealed non-monotonic SPANNING/MOAT transitions (R=950M SPANNING despite MOATs at R=925M and R=975M). A K36 R-sweep from 400M to 800M+ would characterize the transition boundary for sqrt(36) connectivity.

### 4. MAX_FACE_PRIMES_PER_FACE headroom

MAX_FACE_PRIMES_PER_FACE remains at 256 (unchanged from K40). The K5 fix ensures overflow produces a detectable OVERFLOW_SENTINEL rather than corrupt output, but tiles with >256 face primes still result in overflow records. The campaign achieved zero overflows, indicating 256 is sufficient at R=800M, but higher R values may increase face prime counts. Monitoring this across an R-sweep would confirm headroom.

### 5. MAX_GROUPS_GPU = 127 structural limit

The 7-bit group ID limit (imposed by L/R h1 bit-steal encoding) remains the hard ceiling. The plan stated: "If K36 still exceeds 127 groups after the port and payload fixes, that is a format redesign, not a constant tweak." The campaign achieved zero group-count overflows, confirming 127 is sufficient at R=800M. This should be monitored across an R-sweep.

### 6. Documentation inconsistency

The `main.cu` dump mode comment was not updated from `128` to `256` in the K36 branch (noted in the delta doc). The actual dump writes `sizeof(TileOp)` bytes which is 256, so behavior is correct -- only the comment is stale.

## Appendix: Commit History

| Commit | Date | Description |
|--------|------|-------------|
| `884af27` | 2026-04-13 | Parameterize K_SQ across build system, default 40, support 36 |
| `30722ae` | 2026-04-13 | K_SQ=36 campaign post-mortem: O(N^2) compositor, 22% overflow |
| `bb10035` | 2026-04-13 | Replace O(N^2) spanning check with incremental reachability tracking |
| `16a169f` | 2026-04-14 | Split tiles-maxxing into campaign-sqrt-40 and campaign-sqrt-36 |
| `2b54b6d` | 2026-04-14 | K36: 256B TileOp, raised caps, K5 overflow poison fix |
| `f29c486` | 2026-04-14 | K40 vs K36 campaign delta reference document |

## Appendix: Constants Reference

### K_SQ=36 derived constants (compile-time verified)

| Constant | Value | Derivation |
|----------|-------|-----------|
| K_SQ | 36 | Build parameter |
| COLLAR | 6 | ceil(sqrt(36)) |
| TILE_POINTS | 257 | TILE_SIDE + 1 |
| SIDE_EXP | 269 | TILE_POINTS + 2*COLLAR = 257 + 12 |
| LAST_WORD_VALID_BITS | 13 | SIDE_EXP % 32 = 269 % 32 |
| NUM_BACKWARD_OFFSETS | 56 | Backward offsets in K_SQ=36 disc |
| BITMAP_WORDS_PER_ROW | 9 | (269+31)/32 |
| BITMAP_WORDS | 2,421 | 269 x 9 |

### K_SQ=36 tuned constants

| Constant | K40 value | K36 value | Change |
|----------|-----------|-----------|--------|
| MAX_FACE_PORTS_GPU | 32 | 48 | +50% |
| MAX_TOTAL_PORTS_GPU | 128 | 192 | +50% |
| TILEOP_SIZE | 128 | 256 | +100% |
| TILEOP_PAYLOAD_BYTES | 125 | 253 | +102% |

### Constants identical in both campaigns

TILE_SIDE=256, TILE_POINTS=257, SPLIT_PRIMES_COUNT=609, INERT_PRIMES_COUNT=619, SIEVE_LIMIT=10000, SIEVE_SQRT=100, MAX_PRIMES_GPU=2560, MAX_PORTS_GPU=256, MAX_FACE_PRIMES_GPU=900, MAX_FACE_PRIMES_PER_FACE=256, MAX_GROUPS_GPU=127, FACES_PER_PASS=2, TILEOP_HEADER_BYTES=3, EMPTY_OFFSET=3, OVERFLOW_SENTINEL=0xFF, BLOCK_THREADS=288, WARP_SIZE=32, NUM_FACES=4, NUM_MR_WITNESSES=7, NUM_TRIAL_PRIMES=24, MAX_CANDIDATES_GPU=6144.
