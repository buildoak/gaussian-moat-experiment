---
title: V2 CUDA Optimization Plan
date: 2026-04-23
engine: codex
type: design-note
status: complete
refs:
  - docs/supportive/2026-04-23-r85m-cuda-v2-profiling.md
  - docs/supportive/2026-04-10-cuda-kernel-optimization-spree.md
  - docs/supportive/2026-04-11-4090-tuning-sweep.md
  - tiles-maxxing/cuda-campaign-v2-sqrt-36
  - tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel
---

# V2 CUDA Optimization Plan

## Scope

This note audits the pre-v2 optimization history and identifies the cheapest
stealable optimizations for the validated CUDA v2 campaign path:

`tiles-maxxing/cuda-campaign-v2-sqrt-36/`

Current R=85M representative-region profile:

| Metric | Value |
|---|---:|
| K1-K5 dispatch throughput | 69.8K tiles/s |
| Nsight Systems kernel-only throughput | 97.5K tiles/s |
| Legacy 4090 steady-state benchmark | ~155K tiles/s |
| API allocation/free overhead | ~450ms per 200K tiles |

GPU time is concentrated in `kernel_mr` and `kernel_face_encode_v2`:

| Kernel | Share |
|---|---:|
| `kernel_mr` | 38.2% |
| `kernel_face_encode_v2` | 29.1% |
| `kernel_uf_v2` | 16.7% |
| `kernel_sieve` | 14.7% |
| `kernel_face_sort_pack` + compact | 1.2% |

## Historical Optimization Audit

### Biggest Historical Wins

1. **FJ64_262k two-witness MR path**
   - Source commits: `5997aa1`, `8c1f0a5`.
   - Legacy moved from heavier MR witness sets to base-2 plus hash-table witness.
   - On the original monolithic Jetson kernel, Sinclair witness cleanup and removing redundant trial division took throughput from 1,739 to 2,818 tiles/s, a 62% step.
   - In the multi-kernel path, K2 remained dominant but settled into the 2-round FJ64 shape used by the 155K 4090 benchmark.

2. **Multi-kernel split**
   - Source commit: `8c1f0a5`.
   - Split monolithic tile processing into K1 sieve, K2 MR, K3 compact, K4 UF, K5 face encode.
   - Jetson throughput improved from 2,818 to 3,333 tiles/s (+39%), and made per-kernel resource tuning possible.

3. **Targeted K2 register cap**
   - Source commit: `dd56e2e`; documented in `2026-04-11-4090-tuning-sweep.md`.
   - `--maxrregcount=44` was the stable winner on sm_87 and sm_89.
   - At 40 regs, extra occupancy did not compensate for worse scheduling/spills; uncapped or 48 regs was also slower.

4. **Batch size / steady-state dispatch**
   - Documented in `2026-04-11-4090-tuning-sweep.md`.
   - 4090 throughput increased from ~123K at 640 tiles to ~155K at 10K-20K tiles.
   - Launch overhead is negligible at large batches; small batches suffer from underfilled SMs and launch amortization.

5. **Single-pass K1 sieve with Barrett reduction**
   - Source commit: `0e7c1dc`.
   - Removed the double sieve count/scatter pass and eliminated software `%` in sieve residue math.
   - Legacy monolithic improvement was 1,538 to 1,739 tiles/s (+13%).
   - This optimization is already present in v2 `kernel_sieve`.

6. **Prime-centric face extraction**
   - Source commit: `7c0e2d1`.
   - Replaced boundary-cell by prime brute force with an O(P) face-classification pass.
   - Legacy monolithic face work dropped from 44% to roughly 2%, producing a +40% throughput step.

7. **Fixed-chunk persistent stream process**
   - Source commits: `feat: persistent CUDA subprocess`, `acf9c43`, `3ffd202`.
   - Legacy campaign stream mode allocated GPU and host buffers once and processed arbitrary bursts in fixed chunks.
   - This directly avoided tying GPU allocation size to campaign burst size.

### Tried and Not Useful

1. **Multi-stream overlap inside saturated large batches**
   - `2026-04-11-4090-hardware-profiling.md` found inter-kernel gaps of only 2-3 us.
   - At 10K+ tiles each kernel already saturates the GPU, so overlap did not help the legacy 155K steady-state benchmark.

2. **`mont_to_gpu` modulo removal as a standalone 4090 change**
   - The 4090 sweep measured it within noise and slightly slower in one A/B.
   - It remains a reasonable cleanup only when bundled with the larger MR hot-path audit.

3. **`__ldg` / L2 pinning for FJ64 table**
   - No measurable gain on 4090.
   - The 512KB FJ64 table fits easily in 72MB L2 and the compiler already emitted appropriate cached loads.

4. **320-thread blocks**
   - Correct but no faster than 288 threads.
   - 256-thread blocks broke correctness because kernels assume coverage for 257 columns.

5. **K1 shared-memory sieve table / sieve extension**
   - Historical note `2026-04-11-k1-smem-sieve-extension.md` records these as dead ends.
   - The validated 155K ceiling kept the existing K1 design.

## V1 vs V2 Architecture Comparison

### What V1 Does That V2 Does Not

1. **Reuses GPU buffers across chunks**
   - V1 `TileBatchDeviceMemory::allocate()` allocates phase buffers once per dump/campaign/stream run, then reuses them across chunks.
   - V2 production dispatch currently creates `DeviceBuffer` objects inside each slab loop. The R=85M profile measured 232 `cudaMalloc` and 232 `cudaFree` calls for 200K tiles.

2. **Uses the sieve-survivor MR hot path off-axis**
   - V1 K2 calls `is_prime_norm_fj64_262k_gpu(norm, d_fj64_table)` for off-axis norms, skipping trial division because K1 already sieved small factors.
   - V2 K2 calls `is_prime_fj64_gpu(norm, d_fj64_table)`, which still loops over `NUM_TRIAL_PRIMES` with `n % p` before FJ64.
   - This is the cleanest kernel-level regression found in the audit.

3. **Keeps campaign subprocess state warm**
   - V1 stream mode is a persistent process with fixed-size GPU buffers and fixed host result buffers.
   - V2 has streams and a host ring, but the device allocations still churn per slab.

4. **K5 works mostly from shared scratch**
   - V1 K5 uses per-block shared face lists and scratch, then writes the compact TileOp.
   - V2 face encode uses large global debug/intermediate arrays (`face_indices`, `face_roots`, `face_reps`, counts) and a second `kernel_face_sort_pack`.

### What V2 Adds

1. **Correct v3 TileOp semantics**
   - 256-byte TileOp.
   - Per-group `inner_flags` and `outer_flags`.
   - Overflow/empty/tower-closing flags.
   - No dead-end pruning as a verdict shortcut.

2. **K4 geo flag integration**
   - Per-prime inner/outer norm-form tests.
   - Aggregation to visible UF groups after the April 2026 remap fix.

3. **Strict snapped-grid and octant semantics**
   - K2 now filters `in_octant()` and `in_annulus()`.
   - Bitmap orientation was fixed for v2 stitching.

4. **Host-side async scaffolding**
   - V2 has nonblocking H2D, compute, and D2H streams plus ring slots.
   - Current synchronous diagnostics and per-slab allocations prevent the design from realizing most of that overlap.

## Ranked Optimization Opportunities

### 1. Add V2 slab device-buffer pooling

**Impact:** Very high for dispatch throughput.  
**Effort:** 1-2 days.  
**Source:** Steal from V1 fixed-chunk allocation (`TileBatchDeviceMemory`) and adapt to v2 buffer set.  
**Why first:** It targets measured non-kernel waste: about 450ms `cudaMalloc`/`cudaFree` per 200K tiles, plus 100ms pinned host allocation/free.

Implementation outline:

- Introduce a reusable `DeviceSlabBuffers` object sized to `device_slab_tiles`.
- Allocate K1/K2 buffers once: coords, candidates, total candidates, raw candidate counts, K1 overflow, bitmap.
- Allocate K3-K5 buffers once: row prefix, prime positions/counts, parent, geo bits, visible labels, max label, overflow, group flags, tileops, face indices/counts/roots/reps.
- Preserve V2's phase memory split if needed, but do not allocate/free inside the slab loop.
- Keep one tileop device buffer per ring slot only if D2H lifetime requires it; otherwise copy into pinned host output before reuse.

Expected result:

- Dispatch throughput should move materially toward kernel-only throughput.
- On the 200K profile, removing allocator churn alone can recover roughly 0.45s out of 2.865s dispatch time before counting pinned allocation cleanup.

### 2. Gate or batch synchronous diagnostics in production mode

**Impact:** High for dispatch throughput.  
**Effort:** 0.5-1 day.  
**Source:** New v2 host-driver cleanup; V1 campaign mode copied only the outputs needed by the compositor.  
**Why second:** Current diagnostics force compute completion and multiple D2H copies every slab.

Current v2 production dispatch synchronizes after K5/sort-pack, then copies:

- raw candidate counts
- K1 overflow
- prime counts
- max labels
- remap overflow
- face representative counts

Recommended behavior:

- Add a production stats mode that copies only tileops by default.
- Keep overflow accounting optional behind an explicit diagnostic flag.
- For a cheap always-on safety counter, reduce overflow flags on-device to a small summary buffer and copy only that summary per host chunk.
- Preserve the existing detailed path for debug/profile runs and overflow dump capture.

Expected result:

- Removes per-slab host sequencing and vector allocation overhead.
- Lets the existing H2D/compute/D2H stream ring behave closer to an actual pipeline.

### 3. Restore the V1 off-axis MR hot path

**Impact:** High for GPU kernel time; targets `kernel_mr` at 38.2%.  
**Effort:** 0.5-1 day plus validation.  
**Source:** Direct steal from V1 `is_prime_norm_fj64_262k_gpu()`.  
**Risk:** Low if guarded to off-axis K1 sieve survivors; validate against current v2 on sampled tiles and Tsuchimura checkpoints.

V2 currently does:

```cpp
((norm & 3ULL) == 1ULL) && is_prime_fj64_gpu(norm, d_fj64_table)
```

`is_prime_fj64_gpu()` includes trial division by `NUM_TRIAL_PRIMES`. V1's off-axis hot path did:

```cpp
is_prime_norm_fj64_262k_gpu(norm, d_fj64_table)
```

and relied on K1 to have already removed small-factor composites.

Recommended change:

- Add a v2 equivalent of `is_prime_norm_fj64_262k_gpu()` for off-axis norm candidates.
- Keep full `is_prime_fj64_gpu()` for axis primes, where K1 assumptions differ.
- Add a debug comparison mode that evaluates both paths for representative slabs and asserts equality.

Expected result:

- Removes repeated 64-bit modulo trial division from the dominant kernel.
- The old monolithic session saw trial-division removal bundled into a +62% step; v2 should not expect that full number because FJ64 is already present, but this is still the cheapest MR win.

### 4. Rework `kernel_face_encode_v2` per-face representative extraction

**Impact:** Very high for GPU kernel time; targets `kernel_face_encode_v2` at 29.1%.  
**Effort:** 2-4 days for a safe first pass.  
**Source:** Partially steal from V1 prime-centric K5; new development needed for v3 flags and no-pruning semantics.  
**Risk:** Moderate; TileOp byte-for-byte parity and face-order invariants are mandatory.

Observed V2 issue:

- `build_face_indices_warp()` is parallel enough for face-strip collection.
- `build_face_dsu_and_reps()` then runs on `tid == 0` and performs pairwise all-to-all connectivity within each face strip.
- Coordinate decode uses `/ SIDE_EXP` and `% SIDE_EXP` repeatedly.
- V2 emits global face debug/intermediate arrays and then launches `kernel_face_sort_pack`.

Recommended first pass:

- Replace the serial per-face O(n^2) DSU with an ordered face-strip scan whenever the v3 face-port semantics allow it.
- Steal V1's pattern: classify primes by face, sort by canonical face coordinate, then identify representative port starts from adjacent ordered entries.
- Preserve v3 canonical ordering `(h, p_perp)` and strict I/O/L/R face convention.
- Keep the existing global debug arrays during the first rewrite for validation, then remove or shrink them after parity is proven.

Expected result:

- Legacy optimized K5 was only ~2.6% of 4090 kernel time; v2's K5 at 29.1% is not explained by 256-byte TileOps alone.
- A conservative goal is cutting face encode by 2x, which would improve total kernel time by about 15%.

### 5. Pack/decode prime positions consistently

**Impact:** Medium.  
**Effort:** 0.5-1 day for packed decode only; more if paired with UF row-prefix cache.  
**Source:** V1 K1/K2 candidate packing and older optimization proposals.  
**Risk:** Low if all decode sites are updated together.

V2 candidates are packed as `(row << 16) | col`, but compacted prime positions are decoded in face encode by:

```cpp
row = packed / SIDE_EXP;
col = packed % SIDE_EXP;
```

Recommended change:

- Store compacted `prime_pos` as `(row << 16) | col`.
- Decode with shifts and masks in K4/K5.
- Confirm K3 sort/order assumptions remain unchanged.

Expected result:

- Small direct speedup in K4/K5.
- Also simplifies the face encode rewrite and removes integer division from hot code.

### 6. Re-sweep register caps on V2 kernels after hot-path changes

**Impact:** Medium.  
**Effort:** 0.5 day per target GPU.  
**Source:** Historical 4090 sweep procedure.  
**Risk:** Low.

Current v2 static register use from `cuobjdump`:

| Kernel | Registers |
|---|---:|
| `kernel_sieve` | 28 |
| `kernel_mr` | 46 |
| `kernel_compact` | 21 |
| `kernel_uf_v2` | 38 |
| `kernel_face_encode_v2` | 35 |
| `kernel_face_sort_pack` | 40 |

Historical winner for K2 was maxreg=44. V2 K2 currently reports 46 registers. After restoring the no-trial-division hot path, repeat the sweep:

- K2: uncapped, 48, 46, 44, 42, 40.
- Face encode/sort-pack: only after the algorithmic rewrite; current tuning before algorithm cleanup is likely wasted.

### 7. Consider slab pipelining only after allocator/diagnostics cleanup

**Impact:** Medium if host orchestration still dominates; low if slabs already saturate.  
**Effort:** 2-4 days.  
**Source:** V2 already has stream scaffolding; V1 proved persistent fixed chunks.  
**Risk:** Moderate complexity around buffer lifetimes.

Historical caveat:

- Multi-stream overlap did not help legacy 10K+ steady-state kernels because the GPU was saturated and inter-kernel gaps were tiny.

V2-specific reason to revisit later:

- Current v2 has host-side slab sequencing, diagnostics synchronization, and D2H tileop lifetimes that may leave overlap on the table after pooling.

Do not start here. First remove the measured allocator and diagnostics barriers; then profile whether compute/D2H/H2D overlap is still poor.

### 8. K1 constant-memory table experiments

**Impact:** Low to medium; targets `kernel_sieve` at 14.7%.  
**Effort:** 1-2 days.  
**Source:** Historical hypothesis, not a proven win.  
**Risk:** Low if kept behind an A/B branch.

The 4090 profiling note hypothesized constant-memory cache pressure for sieve tables, but a later K1 shared-memory/sieve-extension note recorded those paths as dead ends for the legacy ceiling.

Recommendation:

- Defer until MR and face encode are improved.
- If revisited, run a clean A/B on v2 with global `__ldg` table loads versus constant memory and a shared-memory preload variant.

## Recommended Execution Order

1. **Device buffer pooling for v2 slabs.**
   - Cheapest large dispatch win.
   - Verifies by rerunning the 200K R=85M profile and comparing `cudaMalloc/cudaFree` counts.

2. **Production diagnostics gate / on-device summary counters.**
   - Removes synchronous per-slab readbacks.
   - Verifies by Nsight Systems API timeline and unchanged TileOp output.

3. **Restore V1 off-axis no-trial-division FJ64 path.**
   - Cheapest kernel win.
   - Verify by CUDA-vs-current diff on representative slabs plus Tsuchimura checkpoint runs.

4. **K2 register sweep on v2 after MR cleanup.**
   - Quick target-hardware tuning.
   - Keep the existing 44-register result as the prior, not as a permanent assumption.

5. **Face encode v2 algorithm rewrite.**
   - Largest remaining kernel opportunity.
   - Start with parity-first implementation that preserves debug arrays; optimize storage only after correctness is locked.

6. **Prime-position packed decode and UF lookup cleanup.**
   - Can be folded into the face rewrite if convenient, but keep the commit separately reviewable.

7. **Reprofile and decide on stream/slab pipelining.**
   - Only pursue if kernel-only and dispatch-level throughput still differ materially after pooling and diagnostics changes.

8. **Defer K1 table/cache experiments.**
   - K1 is not currently the leading bottleneck, and prior variants did not beat the 155K legacy ceiling.

## Verification Gates

For each code-change wave:

- Build on RTX 4090 with `-DK_SQ=36 -DCMAKE_CUDA_ARCHITECTURES=89`.
- Run existing `ctest`.
- Run CUDA-vs-reference or current-vs-optimized TileOp byte comparison over representative slabs.
- Re-run the validated k^2=36 checkpoints:
  - `R_inner=80,000,000`, `R_outer=80,008,192` returns SPANNING.
  - `R_inner=80,000,000`, `R_outer=80,015,782` returns SPANNING.
  - `R_inner=80,000,000`, `R_outer=80,015,790` returns MOAT.
- Capture Nsight Systems on the same 200K R=85M region used by `2026-04-23-r85m-cuda-v2-profiling.md`.

## Bottom Line

The fastest path back toward the legacy 155K tiles/s number is not new math or a broad CUDA rewrite. First steal V1's fixed-buffer discipline and MR hot path, then remove v2's production diagnostics barriers. After that, the main new development item is `kernel_face_encode_v2`, whose serial per-face representative work is the largest v2-specific kernel regression.
