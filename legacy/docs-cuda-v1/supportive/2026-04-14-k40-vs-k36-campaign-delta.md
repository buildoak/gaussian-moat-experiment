---
date: 2026-04-14
type: delta
status: complete
campaigns: [campaign-sqrt-40, campaign-sqrt-36]
---

# Campaign Delta: sqrt(40) vs sqrt(36)

## Summary

campaign-sqrt-36 doubles the TileOp record to 256 bytes and raises the per-face GPU port caps to handle the denser port topology that K_SQ=36's tighter connectivity produces. campaign-sqrt-40 is the verified baseline; campaign-sqrt-36 is the corrected successor targeting the April 13 overflow failure.

---

## Constants Delta

All values confirmed from source files. Derived values (COLLAR, SIDE_EXP, LAST_WORD_VALID_BITS, NUM_BACKWARD_OFFSETS) are compile-time constexpr in `gpu_constants.cuh`.

| Constant | K_SQ=40 | K_SQ=36 | Rationale |
|---|---|---|---|
| `K_SQ` | 40 | 36 | connectivity threshold (distance²) |
| `COLLAR` | 7 | 6 | ceil(sqrt(K_SQ)) |
| `SIDE_EXP` | 271 | 269 | TILE_POINTS + 2*COLLAR = 257 + 14 or 257 + 12 |
| `LAST_WORD_VALID_BITS` | 15 | 13 | SIDE_EXP % 32 |
| `NUM_BACKWARD_OFFSETS` | 64 | 56 | backward offset count in K_SQ disc |
| `TILEOP_SIZE` | 128 | 256 | bytes per TileOp record |
| `TILEOP_PAYLOAD_BYTES` | 125 | 253 | TILEOP_SIZE − 3 header bytes |
| `MAX_FACE_PORTS_GPU` | 32 | 48 | per-face port cap in K5 |
| `MAX_TOTAL_PORTS_GPU` | 128 | 192 | sum of all four face port caps |
| `BITMAP_WORDS_PER_ROW` | 9 | 9 | (SIDE_EXP+31)/32 = 9 for both (271→9, 269→9) |
| `BITMAP_WORDS` | 2439 | 2421 | ACTIVE_ROWS × BITMAP_WORDS_PER_ROW |

Constants identical in both campaigns: `TILE_SIDE=256`, `TILE_POINTS=257`, `SPLIT_PRIMES_COUNT=609`, `INERT_PRIMES_COUNT=619`, `SIEVE_LIMIT=10000`, `SIEVE_SQRT=100`, `MAX_PRIMES_GPU=2560`, `MAX_PORTS_GPU=256`, `MAX_FACE_PRIMES_GPU=900`, `MAX_FACE_PRIMES_PER_FACE=256`, `MAX_GROUPS_GPU=127`, `FACES_PER_PASS=2`, `TILEOP_HEADER_BYTES=3`, `EMPTY_OFFSET=3`, `OVERFLOW_SENTINEL=0xFF`, `BLOCK_THREADS=288`, `WARP_SIZE=32`, `NUM_FACES=4`, `NUM_MR_WITNESSES=7`, `NUM_TRIAL_PRIMES=24`, `MAX_CANDIDATES_GPU=6144`.

---

## Structural Changes

### gpu_types.cuh

K40 static assert: `static_assert(sizeof(TileOp) == TILEOP_SIZE, "TileOp must stay 128 bytes")`.

K36 replaces this with three asserts:
- `static_assert(sizeof(TileOp) == 256, "TileOp must be 256 bytes")`
- `static_assert(TILEOP_SIZE == TILEOP_HEADER_BYTES + TILEOP_PAYLOAD_BYTES, "TILEOP_SIZE must equal header + payload")` — not present in K40
- `static_assert(MAX_TOTAL_PORTS_GPU >= 4 * MAX_FACE_PORTS_GPU, ...)` — not present in K40

The K36 assert set is more complete and enforces the header+payload accounting identity at compile time.

### tiles-compositor/include/types.h

K40: `TILEOP_SIZE=128`, `TILEOP_EXT_SIZE=256`, `TILEOP_PAYLOAD_BYTES=125`, `TILEOP_EXT_PAYLOAD_BYTES=253`. Two distinct size constants.

K36: `TILEOP_SIZE=256`, `TILEOP_EXT_SIZE=256`, `TILEOP_PAYLOAD_BYTES=253`, `TILEOP_EXT_PAYLOAD_BYTES=253`. The standard and extended sizes are now unified — the 256-byte format is the standard format. `TILEOP_EXT_SIZE` and `TILEOP_EXT_PAYLOAD_BYTES` remain as aliases with identical values.

### tile-cpp/include/constants.h and types.h

`TILEOP_SIZE`: 128 (K40) → 256 (K36). `TILEOP_PAYLOAD_BYTES`: derived via `TILEOP_SIZE - TILEOP_HEADER_BYTES`, so 125 → 253. `types.h` comment updated from `128 bytes` to `256 bytes`. Logic otherwise identical.

### main.cu — campaign mode output format comment

K40: `// Output: raw TileOp bytes only — total_tiles * 128 bytes, tower-major order.`
K36: `// Output: raw TileOp bytes only — total_tiles * 256 bytes, tower-major order.`

No code change — the output record size is derived from `sizeof(TileOp)` at compile time. The comment update reflects the new record size.

### main.cu — dump mode output format comment

K40: `// uint8_t tileop[128]`
K36: `// uint8_t tileop[128]` — the dump format comment was not updated in K36. This is a documentation inconsistency; the actual dump writes `sizeof(TileOp)` bytes which is 256 in K36.

### campaign.cpp — stream protocol comment

K40: `// [raw TileOp data: num_tiles * 128 bytes]`
K36: `// [raw TileOp data: num_tiles * 256 bytes]`

K40: `// Read raw TileOp output from CUDA campaign — just total_tiles * 128 bytes.`
K36: `// Read raw TileOp output from CUDA campaign — just total_tiles * 256 bytes.`

`TileOp` struct comment in campaign.cpp: K40 says `TILEOP_SIZE = 128`, K36 says `TILEOP_SIZE = 256`.

### extract_tileops.py

K40: `TILEOP_SIZE=128`, `TILEOP_PAYLOAD_BYTES=125`, `CUDA_RECORD_SIZE=148` (20 bytes coord+prime_count + 128 tileop), struct format `<qqI128s`.

K36: `TILEOP_SIZE=256`, `TILEOP_PAYLOAD_BYTES=253`, `CUDA_RECORD_SIZE=276` (20 bytes + 256 tileop), struct format `<qqI256s`.

Docstring: `"128-byte TileOps from 148-byte CUDA binary records"` → `"256-byte TileOps from 276-byte CUDA binary records"`.

Parser `parse_counts` logic is identical; the `payload_budget` default changes from 125 to 253.

---

## K5 Poison Fix (K36-only)

Location: `kernel_face_encode.cu`, function `extract_faces_gpu_parallel_k5`, after the per-face prime accumulation loop.

**K40 behavior (lines 221-226):**
```cpp
if (tid < NUM_FACES) {
    if (face_prime_counts[tid] > MAX_FACE_PRIMES_PER_FACE) {
        face_prime_counts[tid] = MAX_FACE_PRIMES_PER_FACE;
    }
}
```
Face prime overflow is silently clamped. Excess primes are dropped. No overflow flag is set. Downstream port clustering proceeds on the truncated list — result is incorrect but no sentinel is emitted.

**K36 behavior (lines 221-227):**
```cpp
if (tid < NUM_FACES) {
    if (face_prime_counts[tid] > MAX_FACE_PRIMES_PER_FACE) {
        scratch->overflow = 1u;
    }
    face_prime_counts[tid] = min(face_prime_counts[tid],
                                 static_cast<uint32_t>(MAX_FACE_PRIMES_PER_FACE));
}
```
Face prime overflow sets `scratch->overflow = 1u` **before** clamping. This propagates to `prune_dead_ends_gpu_k5` which sets `group_count = MAX_GROUPS_GPU + 1` and returns early. `encode_tileop_gpu_k5` then emits `OVERFLOW_SENTINEL` (0xFF) for all bytes.

**Why K40 doesn't need this fix:** At K_SQ=40, COLLAR=7 gives face regions of ~7×257 = 1799 lattice points. At representative R values (≥600M), prime density ≈ 1/ln(R²) ≈ 1/41.5. Average face primes ≈ 43, well below MAX_FACE_PRIMES_PER_FACE=256. The per-face overflow path is never reached in practice. The silent clamp was not exercised by any known K40 run.

**Why K36 needs it:** COLLAR=6 but tighter connectivity (K_SQ=36 vs 40) means fewer primes merge per component. Port count per face increases. The April 13 run saw 22.4% overflow rate starting at tower 1 with the old caps. Even with raised caps (MAX_FACE_PORTS_GPU: 32→48, MAX_TOTAL_PORTS_GPU: 128→192), face prime overflow remains a reachable code path that must emit a valid sentinel rather than produce silently corrupt output.

---

## TileOp Layout

The encoding scheme (3-byte header, dynamic packed payload) is identical in both campaigns. What changes is the payload budget.

**Header (3 bytes, same in both):**
- `bytes[0]` = `off_I` = 3 + o_cnt
- `bytes[1]` = `off_L` = 3 + o_cnt + i_cnt
- `bytes[2]` = `off_R` = 3 + o_cnt + i_cnt + l_cnt

**Group region (immediately after header, same in both):**
- bytes[3 .. off_I−1]: O-face group bytes (1 byte each)
- bytes[off_I .. off_L−1]: I-face group bytes (1 byte each)
- bytes[off_L .. off_R−1]: L-face group bytes (1 byte each, bit 7 = h1 MSB)
- bytes[off_R .. off_R+r_cnt−1]: R-face group bytes (1 byte each, bit 7 = h1 MSB)

**h1 region:**
- bytes[h_start .. h_start+l_cnt−1]: L-face h1 low bytes
- bytes[h_start+l_cnt .. h_start+l_cnt+r_cnt−1]: R-face h1 low bytes

**r_cnt derivation:**
- K40: `r_cnt = (125 − o_cnt − i_cnt − 2*l_cnt) / 2`
- K36: `r_cnt = (253 − o_cnt − i_cnt − 2*l_cnt) / 2`

**Maximum r_cnt with no other ports:**
- K40: `r_cnt_max = 125 / 2 = 62`
- K36: `r_cnt_max = 253 / 2 = 126`

**Overflow check (encode_tileop_gpu_k5 line 418):**
```cpp
if (o_cnt + i_cnt + 2 * l_cnt + 2 * r_cnt > TILEOP_PAYLOAD_BYTES)
```
Threshold is 125 (K40) or 253 (K36). The rest of the encoding function is byte-for-byte identical.

---

## Memory Impact

### K5 Shared Memory per Block

Computed from struct definitions:

| Component | K40 | K36 | Notes |
|---|---|---|---|
| `face_prime_lists` | 8,192 B | 8,192 B | 4 faces × 256 primes × 8 B/prime |
| `face_prime_counts` | 16 B | 16 B | uint32_t[4] |
| `FaceScratchGPU` | 2,052 B | 2,564 B | see breakdown below |
| `FaceDataGPU` | 1,032 B | 1,032 B | 256 ports × 4 B + 2 ints |
| **Total K5 smem** | **11,292 B** | **11,804 B** | |

`FaceScratchGPU` breakdown:
- K40: 128 raw_ports × 8 B + 127 group_entries × 8 B + 4×2 B + 2+2 B = 1024 + 1016 + 8 + 4 = 2052 B
- K36: 192 raw_ports × 8 B + 127 group_entries × 8 B + 4×2 B + 2+2 B = 1536 + 1016 + 8 + 4 = 2564 B

Both campaigns remain well under the 48 KB shared memory limit. The 512 B increase in K5 smem is negligible for occupancy. On RTX 4090 (48 KB/SM), at 288 threads/block, K5 block limit is still set by register pressure, not shared memory.

### Per-tile GPU Buffer Sizes (Phase 1 / Phase 2)

| Buffer | K40 | K36 | Notes |
|---|---|---|---|
| `d_cand_list` | 24,576 B | 24,576 B | 6144 × 4 B, dominant Phase 1 cost |
| `d_bitmap` | 9,756 B | 9,684 B | 2439 vs 2421 words × 4 B |
| `d_output` | 128 B | 256 B | TileOp per tile |
| Phase 1 total | ~33.5 KB/tile | ~33.5 KB/tile | d_coords + d_cand_list + d_total_cands + d_bitmap |
| Phase 2 total | ~15.7 KB/tile | ~15.8 KB/tile | remaining buffers |

The TileOp size increase (128 B) is immaterial relative to d_cand_list (24 KB).

---

## Known Trade-offs

**K36 gains vs K40:**
- Larger payload budget (253 vs 125 bytes): r_cnt_max doubles from 62 to 126, accommodating the denser port topology at smaller K_SQ.
- MAX_FACE_PORTS_GPU raised from 32 to 48: reduces GPU overflow rate for K36 geometry.
- MAX_TOTAL_PORTS_GPU raised from 128 to 192: consistent with 4 × 48.
- K5 poison on face-prime overflow: produces a detectable OVERFLOW_SENTINEL instead of silently corrupt output.
- Compositor TILEOP_SIZE unification: `TILEOP_SIZE == TILEOP_EXT_SIZE == 256`, no extended-path branch needed in compositor logic.

**K36 costs vs K40:**
- Output I/O: `total_tiles × 256 B` vs `total_tiles × 128 B` — doubles raw TileOp I/O on disk and in the stream protocol pipe. At 221K towers × ~37 tiles/tower ≈ 8.2M tiles, that is ~2.1 GB vs ~1.1 GB.
- CUDA record size in extract_tileops.py: 276 B vs 148 B per record.
- `d_output` allocation: 256 B vs 128 B per tile (128 B difference across chunk; at CHUNK_SIZE=20K tiles, that is 2.56 MB vs 1.28 MB — negligible).
- Port cap increase: MAX_TOTAL_PORTS_GPU 128→192 increases FaceScratchGPU by 512 B. No measurable occupancy impact.

**Remaining overflow risk in K36:**
- MAX_FACE_PRIMES_PER_FACE is still 256 (unchanged). The K5 fix ensures this now produces an OVERFLOW_SENTINEL rather than corrupt output, but tiles with >256 face primes still result in overflow records that require C++ reprocessing via the ExtendedTileSideTable path.
- MAX_GROUPS_GPU remains 127 — the 7-bit structural limit. Tiles with >127 groups poison unconditionally in both campaigns.

---

## Validation Status

**K40 (campaign-sqrt-40):**
- Verified baseline from 2026-04-13: R=600M run returned SPANNING with <0.01% overflow rate.
- All static_asserts pass at compile time for K_SQ=40.
- Known-good configuration.

**K36 (campaign-sqrt-36):**
- Implements fixes identified in 2026-04-13 postmortem and campaign_sqrt_36_plan.md.
- K5 poison fix: prevents silent truncation; verified by code inspection.
- Cap raises: MAX_FACE_PORTS_GPU 32→48 and MAX_TOTAL_PORTS_GPU 128→192 should substantially reduce the 22.4% overflow rate observed April 13, but no post-fix campaign run data exists yet.
- No small-batch validation run (1K–10K tiles) recorded to confirm overflow rate drop.
- Compositor-side reprocessing path (ExtendedTileSideTable for residual overflow tiles) confirmed present in compositor types.h but C++ reprocessing dispatch in campaign.cpp not confirmed implemented.
- O(N²) compositor spanning-check issue (Finding 1 from postmortem): not addressed in this campaign snapshot — fix is in campaign.cpp's call frequency to `check_spanning_incremental()`, independent of K_SQ.
- No post-fix campaign verdict available.
