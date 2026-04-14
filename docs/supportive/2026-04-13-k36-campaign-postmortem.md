# K_SQ=36 Campaign Post-Mortem — 2026-04-13

## Summary

We attempted a K_SQ=36 (step distance sqrt(36)=6) campaign at R=80,015,782 on an RTX 4090 (vast.ai) to verify against the Tsuchimura moat bound. The run was killed at 36% completion (tower 80,000 of 221,039) after 24 minutes. Three critical issues surfaced: (1) O(N^2) compositor scaling caused by linear-scan spanning checks called after every tower ingestion, making what should have been a 5-minute GPU-bound campaign project to 3.5+ hours; (2) a 22% overflow tile rate (651K overflows in ~2.9M tiles) caused by GPU-side per-face port caps that are too tight for K_SQ=36 geometry; (3) a TileOp parse failure from malformed offset triples reaching the compositor. The campaign verdict is unreliable. All three issues must be fixed before re-attempt.

## Timeline

1. **Campaign launch.** `campaign 80015782 --k-sq 36 --cuda --burst-size 4096 --progress-interval 5000` started on vast.ai RTX 4090 instance. Grid computed: 221,039 towers, ~8M total tiles.
2. **Tower 0 processed (C++ path).** Tower j=0 handled by CPU per campaign spec S6 (on-axis sieve correction). Completed in under 1 second.
3. **CUDA burst processing begins.** Towers 1-221,038 dispatched in bursts of 4,096 to `tile_kernel_multi`.
4. **Overflow warnings flood stderr from tower 1 onward.** Overflow and malformed tiles replaced with empty TileOps. Rate: ~22% of all tiles.
5. **Progressive slowdown observed.** Per-5K-tower batch time grows linearly: 7.2s at 0-5K, 199.3s at 75-80K. GPU throughput remains constant (155K tiles/sec); bottleneck is CPU-side compositor.
6. **Run killed at tower 80,000 (36%).** Extrapolated time to completion: ~3.5 hours. Expected time based on GPU throughput alone: ~5 minutes.
7. **Earlier attempt had crashed** with `FATAL parse_counts: off_I=3 off_L=0 off_R=0 bytes=[03 00 00 00] budget=125` -- a TileOp with violated offset ordering reaching the compositor parser.

## Findings

### Finding 1: O(N^2) Compositor Scaling

**Evidence:**

| Towers processed | Time per 5K batch (s) | ms/tower |
|---|---|---|
| 0-5K | 7.2 | 1.4 |
| 5-10K | 13.1 | 2.6 |
| 15-20K | 29.5 | 5.9 |
| 30-35K | 58.5 | 11.7 |
| 45-50K | 105.5 | 21.1 |
| 60-65K | 147.7 | 29.5 |
| 70-75K | 181.0 | 36.2 |
| 75-80K | 199.3 | 39.9 |

Per-tower cost grows linearly with tower count, making total campaign time O(N^2).

**Root cause: `has_spanning()` called after every tower, performing linear scans over `inner_members_` and `outer_members_`.**

The hot path is `compositor.cpp` lines 59-73 (`has_spanning()`) and its alias `check_spanning_incremental()` (line 77-79, which just calls `has_spanning()`). This function is called after every single tower ingestion in the campaign loop (`campaign.cpp` line 609).

The function does:

```cpp
bool Compositor::has_spanning() {
    std::unordered_set<uint32_t> inner_roots;
    inner_roots.reserve(inner_members_.size());
    for (uint32_t member : inner_members_) {          // O(|inner_members_|)
        inner_roots.insert(find(member));              // find() is amortized O(alpha(n))
    }
    for (uint32_t member : outer_members_) {           // O(|outer_members_|)
        if (inner_roots.count(find(member)) > 0U) {
            return true;
        }
    }
    return false;
}
```

Both `inner_members_` and `outer_members_` are append-only vectors that grow with every tower:
- `inner_members_` accumulates from `collect_inner_boundary()` (line 463-548) -- every tower's row-0 I-face groups plus staircase L-face groups are pushed.
- `outer_members_` accumulates from `collect_outer_boundary_ingest()` (line 550-599) -- every tower's top-row O-face groups plus height-difference L-face groups are pushed.

After processing T towers with G average boundary groups per tower:
- `inner_members_` has ~T*G entries
- `outer_members_` has ~T*G entries
- Each `has_spanning()` call costs O(T*G) to iterate + O(T*G) hash set operations
- Called T times total: O(T^2 * G)

At 221K towers with even modest G (say 10-20 groups per boundary), the member vectors reach millions of entries. Each spanning check constructs a fresh `unordered_set`, iterates the full inner vector, then iterates the full outer vector. At tower 80K, this means ~80K * G hash insertions + ~80K * G hash lookups per check.

Additionally, each `find(member)` call (line 62, 67) traverses the union-find tree. While path compression (line 233-237) keeps individual `find()` nearly O(1) amortized, the sheer volume of calls compounds: at tower 80K, each spanning check may invoke `find()` hundreds of thousands of times.

**Impact:** Campaign wall time is dominated by this CPU-side check, not GPU computation. The GPU processes 155K tiles/sec regardless; the compositor check turns a 5-minute GPU campaign into a 3.5-hour CPU-bound slog.

**Proposed fix (three options, increasing complexity):**

1. **Reduce check frequency.** Call `check_spanning_incremental()` every N towers (e.g., every 1000) instead of every tower. Spanning detection is delayed by at most N towers, but total spanning-check cost drops from O(T^2) to O(T^2/N). Simple, low-risk, ~1000x faster if N=1000.

2. **Incremental root tracking.** Maintain persistent `inner_roots_` and `outer_roots_` sets. On each `unite()`, update the root sets incrementally. On each new boundary member addition, check membership immediately. Spanning check becomes O(new_members_per_tower) instead of O(all_members). Requires careful bookkeeping when union-find merges change roots.

3. **Epoch-based spanning with dirty bits.** Track which union-find roots were modified since last check. Only re-resolve members whose roots may have changed. Most expensive to implement but gives true O(delta) per check.

Option 1 is the pragmatic first fix. The theoretical cost of spanning-every-1000 is that we process up to 999 extra towers past the actual spanning point before detecting it, which is irrelevant for moat verdicts (we need all towers anyway) and negligible for spanning verdicts.

### Finding 2: 22% Overflow Tile Rate

**Evidence:** 651,413 overflow/malformed tile warnings in 80,000 towers (~2.9M tiles processed). That is 22.4% of all tiles replaced with empty TileOps. Overflows begin from tower 1, row 0 -- not a gradual onset at high tower indices.

**Root cause: GPU-side per-face caps are too tight for K_SQ=36 geometry at R=80M.**

The overflow is triggered by three cascading caps in the GPU encode kernel (`kernel_face_encode.cu`):

1. **`MAX_FACE_PRIMES_PER_FACE = 256`** (gpu_constants.cuh line 55). During face extraction (line 207-208), each face prime is atomically appended to a per-face list. If any single face exceeds 256 primes, the count is silently clamped (line 222-224) and the excess primes are lost. This truncation corrupts the downstream port clustering.

2. **`MAX_FACE_PORTS_GPU = 32` per face, `MAX_TOTAL_PORTS_GPU = 128` total** (gpu_constants.cuh lines 56-57). During port extraction (line 261), if any face exceeds 32 ports or total exceeds 128, `scratch->overflow` is set (lines 278, 291). This triggers the overflow sentinel in the encode step.

3. **`MAX_GROUPS_GPU = 127`** (gpu_constants.cuh line 58). If unique groups exceed 127 (the 7-bit group-ID cap imposed by the L/R h1 bit-steal encoding), the entire TileOp is poisoned (line 402-407).

4. **Payload budget: `o_cnt + i_cnt + 2*l_cnt + 2*r_cnt > 125`** (line 416). Even with valid group counts, if total packed data exceeds 125 bytes, overflow is triggered.

**Why 22% at K_SQ=36?**

K_SQ=36 (step distance 6) has a smaller collar than K_SQ=40 (COLLAR=6 vs 7). This means the connectivity threshold is tighter: two primes must be within distance 6 to connect, versus distance ~6.32 for K_SQ=40. Counter-intuitively, a **smaller** K_SQ produces **more** ports per face, not fewer. The reason: with tighter connectivity, union-find merges fewer primes into the same component. More components survive dead-end pruning. More distinct groups appear on each face.

At R=80M, the prime density in the sieve domain is approximately `1/ln(R^2) ~ 1/36.4`. For a 257x257 tile, that is roughly 1,810 primes. With COLLAR=6, face regions contain ~6*257 = 1,542 lattice points per face, yielding ~42 face primes per face on average. With the tighter K_SQ=36 connectivity, these cluster into more ports. The 32-port-per-face GPU cap is regularly exceeded.

**Is 22% expected?** No. At K_SQ=40 and similar R values, overflow rates are under 0.01%. The 22% rate at K_SQ=36 indicates the GPU caps were tuned for K_SQ=40 geometry and are structurally inadequate for K_SQ=36. This is a configuration bug, not a rare edge case.

**Impact on verdict reliability:** Every overflow tile is replaced with an empty TileOp, meaning its connectivity information is silently deleted. With 22% of tiles carrying no connectivity data, the moat/spanning verdict is meaningless. A "MOAT" verdict could be an artifact of lost connections. A "SPANNING" verdict would be reliable (false positives are impossible -- connections are only deleted, never fabricated), but is unlikely to be reached given the massive data loss.

**Proposed fix:**

1. **Raise GPU caps for K_SQ=36.** In `gpu_constants.cuh`, increase:
   - `MAX_FACE_PRIMES_PER_FACE`: 256 -> 512 (or K_SQ-conditional)
   - `MAX_FACE_PORTS_GPU`: 32 -> 64
   - `MAX_TOTAL_PORTS_GPU`: 128 -> 256
   - Verify shared memory budget remains within 48KB/block limit after increases.

2. **Make caps K_SQ-conditional at compile time.** Use `#if K_SQ_VAL <= 36` blocks to select larger caps for smaller K_SQ. This avoids penalizing K_SQ=40 performance.

3. **Implement the reprocessing fallback.** The spec (tile_operations.md S8.2) states: "Overflow tiles are reprocessed by the C++ host path into 256-byte extended TileOps (TileOp_wide)." This fallback is not implemented -- the campaign currently replaces overflows with empties. Implementing the TileOp_wide reprocessing path would handle residual overflows that exceed even raised caps.

### Finding 3: TileOp Parse Failure

**Evidence:** Earlier campaign attempt crashed with:
```
FATAL parse_counts: off_I=3 off_L=0 off_R=0 bytes=[03 00 00 00] budget=125
```

**Root cause: A TileOp with off_I=3, off_L=0, off_R=0 violates the ordering invariant `off_I <= off_L <= off_R`.**

The `parse_counts()` function in `tileop_parse.cpp` (lines 26-35) enforces:
```cpp
assert(counts.off_I >= TILEOP_HEADER_BYTES);   // off_I >= 3
assert(counts.off_I <= counts.off_L);            // off_I <= off_L
assert(counts.off_L <= counts.off_R);            // off_L <= off_R
```

The failing tile has `off_I=3` (valid -- equals TILEOP_HEADER_BYTES), but `off_L=0` and `off_R=0`. This violates `off_I(3) <= off_L(0)`.

This byte pattern `[03, 00, 00, 00]` cannot come from the encoder. The C++ encoder (`encode.cpp` line 203-205) always sets:
```cpp
tileop.bytes[0] = TILEOP_HEADER_BYTES + o_cnt;     // >= 3
tileop.bytes[1] = TILEOP_HEADER_BYTES + o_cnt + i_cnt;  // >= bytes[0]
tileop.bytes[2] = TILEOP_HEADER_BYTES + o_cnt + i_cnt + l_cnt;  // >= bytes[1]
```

The GPU encoder (`kernel_face_encode.cu` lines 423-425) has identical logic.

The pattern `[03, 00, 00, 00]` looks like a partially-initialized TileOp where byte[0] was written (EMPTY_OFFSET=3) but bytes[1] and [2] were left at zero. This is consistent with:

- **A CUDA memory corruption bug** where a TileOp buffer is partially overwritten by an adjacent tile's computation. The GPU allocates `N * TILEOP_SIZE` contiguous bytes; if a kernel writes past its tile's 128-byte boundary, it corrupts the next tile.
- **A race condition in the multi-kernel pipeline** where the K5 (face encode) kernel writes the output before K4 (union-find) has finished, resulting in partial data.
- **An uninitialized buffer read** where the host reads the output file before the GPU has flushed all writes.

The campaign code added a BUG-5 fix (campaign.cpp lines 556-587) that catches malformed offset triples and replaces them with empties. The specific check `b0 < TILEOP_HEADER_BYTES || b0 > b1 || b1 > b2 || b2 > TILEOP_SIZE` would catch `[03, 00, 00, 00]` (since 3 > 0). The earlier crash occurred before this fix was in place.

**Proposed fix:**

1. The BUG-5 guard in campaign.cpp already handles this defensively (replace with empty). This is adequate as a runtime safety net.
2. Root-cause the GPU-side corruption. Add `cudaDeviceSynchronize()` + error checks between kernel launches to rule out async hazards. Verify that the output buffer is fully zeroed before each burst.
3. Add a CUDA `memset` of the output buffer before each K5 launch so partially-written tiles show as `[00,00,00,...]` (detectable as malformed) rather than carrying stale data from a previous burst.

## Verdict Reliability Assessment

**No verdict from this pipeline can be trusted at K_SQ=36 in its current state.**

- **MOAT verdicts are unreliable.** With 22% of tiles replaced by empties, connectivity paths that would prove spanning may be silently severed. A "MOAT" verdict could be a false negative caused by data loss.
- **SPANNING verdicts would be reliable** (empty tiles only remove connections, never add them), but are unlikely to be reached given the extent of data loss.
- **Even if overflows were fixed, the O(N^2) scaling makes full campaigns infeasible.** At 221K towers, the compositor would spend hours on spanning checks alone, regardless of GPU throughput.

The pipeline must be fixed on both axes before K_SQ=36 campaigns produce trustworthy results.

## Recommended Fix Order

1. **[Critical] Raise GPU caps for K_SQ=36.** Increase `MAX_FACE_PRIMES_PER_FACE`, `MAX_FACE_PORTS_GPU`, `MAX_TOTAL_PORTS_GPU` in `gpu_constants.cuh`. This is the highest-impact fix: it eliminates 22% data loss. Validate by running a small batch (1000 tiles) and confirming overflow rate drops below 0.1%. Ideally make these compile-time conditional on K_SQ_VAL.

2. **[Critical] Reduce spanning check frequency.** Change `check_spanning_incremental()` to fire every N towers (N=1000 or N=5000) instead of every tower. One-line change in campaign.cpp's inner loop. This converts O(N^2) to near-O(N) for the spanning check cost.

3. **[Important] Implement TileOp_wide reprocessing.** For residual overflows that exceed even raised caps, implement the 256-byte extended TileOp path per spec S4.7. Feed extended ops via `ExtendedTileSideTable` which the compositor already supports (the `ext` parameter flows through all compositor methods).

4. **[Defensive] Harden CUDA output buffers.** Zero the output buffer before each K5 kernel launch. Add post-burst validation (offset ordering check) before feeding to compositor. The BUG-5 guard is good but catching corruption at the source is better.

5. **[Long-term] Incremental spanning check.** Replace the rebuild-from-scratch `has_spanning()` with root-set tracking that costs O(new_members) per tower instead of O(all_members). This enables per-tower checking without the O(N^2) penalty if per-tower detection is desired.

## Appendix: Raw Data

### A.1 Timing Table

| Towers processed | Time per 5K batch (s) | ms/tower | Implied compositor overhead |
|---|---|---|---|
| 0-5K | 7.2 | 1.4 | baseline |
| 5-10K | 13.1 | 2.6 | 1.9x |
| 15-20K | 29.5 | 5.9 | 4.2x |
| 30-35K | 58.5 | 11.7 | 8.4x |
| 45-50K | 105.5 | 21.1 | 15.1x |
| 60-65K | 147.7 | 29.5 | 21.1x |
| 70-75K | 181.0 | 36.2 | 25.9x |
| 75-80K | 199.3 | 39.9 | 28.5x |

Extrapolated to 221K towers (by fitting quadratic to cumulative time): ~3.5 hours total.

### A.2 Overflow Statistics

- Towers processed: 80,000 / 221,039
- Tiles processed: ~2,900,000
- Overflow warnings: 651,413
- Overflow rate: 22.4%
- First overflow: tower 1, row 0 (immediate onset)

### A.3 GPU Constants (K_SQ=36 Build)

| Constant | Value | Sufficient? |
|---|---|---|
| COLLAR | 6 | N/A (derived) |
| SIDE_EXP | 269 | N/A (derived) |
| MAX_FACE_PRIMES_PER_FACE | 256 | No -- regularly exceeded |
| MAX_FACE_PORTS_GPU | 32 | No -- regularly exceeded |
| MAX_TOTAL_PORTS_GPU | 128 | Marginal |
| MAX_GROUPS_GPU | 127 | Marginal (7-bit structural limit) |
| TILEOP_PAYLOAD_BYTES | 125 | Tight with high port counts |

### A.4 Parse Failure Log

```
FATAL parse_counts: off_I=3 off_L=0 off_R=0 bytes=[03 00 00 00] budget=125
```

Triggered in `tileop_parse.cpp` line 30-35. Byte pattern consistent with partially-initialized TileOp (EMPTY_OFFSET written to byte[0], rest left at zero).
