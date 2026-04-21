---
date: 2026-04-21
engine: claude-opus-4-7
role: synthesizer
status: complete
inputs:
  - 2026-04-21-opus-reuse.md
  - 2026-04-21-opus-correctness.md
  - 2026-04-21-codex-performance.md
---

# Canonical CUDA Port Plan — Stages 5-9 — cuda-campaign-v2-sqrt-36

## 1. Executive summary

The three angles converge on a single thesis: **lift K1/K2/K3 byte-for-byte from v1, extend K4 with v2 geo-flags + single-threaded dense-remap (not parallel compaction), rebuild K5 from scratch around face-strip UF + 256 B encode, and keep the five-kernel split intact.** The highest-leverage decision arbitrated here is **dense-remap placement**: Plan A (REUSE) would have folded dense label assignment into a parallel compact kernel; Plan B (CORRECTNESS) demands it run single-threaded in ascending prime-index order because `cpp-campaign-v2/src/tileop.cpp:172-204` assigns wire labels `1..max_label` in ascending-prime-index-first-appearance order — any parallel scheme assigns by root-ID sort order instead and silently permutes every `face_groups` byte plus every `inner_flags`/`outer_flags` bit on every tile with ≥2 UF components. **B is correct by ground-truth code; A's K3-parallel-compact is amended: dense-remap lives at the tail of K4, single-threaded (thread 0 over ≤6144 primes), after the geo-flag staging pass.** The second-largest arbitration is kernel count: Plan A proposed fusing stages 6+8+9 into K5; Plans B and C hold the 5-kernel split. **C wins** — the v1 profile (`docs/supportive/2026-04-10-multi-kernel-architecture.md:146-155`) shows K4@27% and K5@2.4% with K2@54%; fusing K4+K5 recouples registers that the v1 split decouples on purpose, and the blueprint explicitly mandates K4-extended + K5-refactored (`methodology/lemmas_v2/campaign-blueprint.md:269-344`). K5 fusion is not viable.

## 2. Three-way reconciliation table

| Decision | Plan A (REUSE) | Plan B (CORRECTNESS) | Plan C (PERFORMANCE) | CANONICAL RESOLUTION + justification |
|---|---|---|---|---|
| Kernel count / boundaries | 4 kernels; stages 6+8+9 folded into rewritten K5 | 5 distinct kernel phases with potential fusion; Stage 7 UF+remap in one kernel; Stages 8+9 in another | 5 separate kernels (K1-K5), blueprint-aligned, K4 absorbs geo, K5 rewritten | **5-kernel split (C wins).** Blueprint §6.1-6.3 pins this exact topology. V1 profile shows K5@2.4% — no launch-overhead pressure to fuse. Fusing K4+K5 breaks the independent `--maxrregcount` contract that `docs/supportive/2026-04-10-multi-kernel-architecture.md:120-130` identifies as the root of the +39% monolithic → multi-kernel speedup. |
| Dense-remap placement | Inside K3 compact (parallel compaction) | Stage 7 Phase C, thread 0 only, strictly serial over ≤6144 primes | Inside K4 after path compression, in the final pass | **B's serialization semantics + C's K4 placement.** Ground-truth: `cpp-campaign-v2/src/tileop.cpp:172-179` iterates `for i=0..prime_count; raw_roots.push_back(dsu->find(i))` and `:181-204` assigns wire label on *first-appearance in that scan order*. A's K3-parallel scheme would violate this; A's K3 is amended. Blueprint `campaign-blueprint.md:313` permits any mechanism but requires the contract; the safe implementation is thread 0 sequentially over primes, which on a 288-thread block wastes 287 threads for ~5μs per tile — imperceptible at K4@27% budget. |
| Launch config per kernel | All kernels 288 threads/block, 1 block/tile; K1 rreg=40, K2 uncapped, K3 rreg=32, K4 rreg=40, K5 in single launch | Same one-block-per-tile; Stage 7 288 threads; thread 0 does Phase C | K1 288/40, K2 288/uncapped-or-44-tuned, K3 288/32/540B smem, K4 288/40/2 KB smem, K5 288/40/13 KB smem | **C's config wins.** All three agree on 288 threads + 1 block/tile (v1's shape). C's per-kernel shared-memory budgets are blueprint-grounded (`campaign-blueprint.md:334`); A and B don't contradict. K2 cap decision deferred to M7 tuning per v1 `dd56e2e` +2.2% evidence. |
| Memory layout | Flat per-tile slices in global; pinned host I/O; single stream OK | Pinned; SoA in burst buffers; snapshot written host-side via cpp-campaign-v2 | SoA flat buffers; v1 two-phase overlay mandatory to fit 14.4 GB nominal → <8.5 GB peak on 24 GB 4090; 3 streams (H2D/compute/D2H) | **C's layout + overlay + 3 streams.** C's device-memory table quantifies the 6144-prime inflation A and B underscaled. Two-phase overlay (`campaign-sqrt-36/.../main.cu:240-252`) is mandatory, not optional, per C §6 risk #3. A's single-stream recommendation is rejected; C's 3-stream ring overlaps 200k-chunk H2D + compute + D2H with host compositor ingest. |
| UF determinism (atomic union policy) | LIFT v1 `atomic_union` at `kernel_uf.cu:57-75` — smaller-root-wins, path-splitting CAS | Same atomic_union + **mandatory final path-compression pass** before dense-remap reads | Same | **LIFT v1 atomic_union verbatim with mandatory full-compression pass.** All three agree; B makes the `__syncthreads()` fence between compression and dense-remap explicit — ground-truth at `kernel_uf.cu:134-137` already does it. No ambiguity. |
| Port ordinal sort | Warp bitonic over 4 face lists in shared | Full 3-key comparator `(h, p_perp, global_wire_label)` via thrust or bitonic | Insertion/bitonic in shared; single-thread OK for bring-up; 3-key comparator | **Bitonic 3-key comparator in shared (B+C merged).** B's 3-key requirement is ground-truth mandatory: `tileop.cpp:143-147` is `std::sort` with three keys — the label tiebreak is non-negotiable because `std::sort` is unstable and two ports CAN share `(h, p_perp)` within a tile. Use warp-local bitonic sort (one warp per face) with a functor that reads all three keys. Thrust rejected: it is host-launched per call, unacceptable overhead at 1 block/tile scale. |
| 128-bit arithmetic | Reuse v1 `__umul64hi` helpers in `gpu_math.cuh:56-65`; reassemble i128 via hi/lo | i128 emulation via `uint2`/`ulonglong2` with signed multiply; sign discipline for `eps*eps` | Inline `__umul64hi`/`__umul64low` pair; hi/lo i128 limbs; avoid generic emulation that spills registers | **C's inline hi/lo limbs, B's sign discipline for `eps` path.** B is right that `eps` can be negative and the emulation must correctly two's-complement square. C is right that a generic `__int128` emulation header spills registers and blows K4's 40-reg class. Compromise: hand-code `i128_sq_leq(u64 hi, u64 lo, u64 bound_hi, u64 bound_lo)` that takes signed `eps` as i64, squares via `__umul64hi(eps, eps)` directly (unsigned product of magnitudes, sign is squared away), and compares to the upload. Unit test against CPU across the boundary band at M3 per B's `test_geo_i128`. |
| MR reproducibility (bit-identical primality) | LIFT v1 FJ64 table + kernel verbatim; SHA-256 pin at M1 | FJ64 table `__constant__` upload; SHA-256 pinned; unit-test parity across full candidate band | FJ64 table stays in global memory (512 KB too large for constant); SHA-256 pinned via `campaign_constants.h:28-29` | **C's global-memory FJ64 + B's full-band parity test + A's SHA-256 pin.** 512 KB is too big for `__constant__` — B is wrong on placement but right on the contract. Upload once to global, keep L2-resident by `cudaFuncSetAttribute` access hints. CI gate: (1) `static_assert(sizeof(kFj64Table) == 262144 * sizeof(uint16_t))`, (2) compute SHA-256 at host init, assert equals pinned constant in `campaign_constants.h`, (3) M1 unit test sweeps `n` across `[R_inner²-K, R_outer²+K]` at R=1000 asserting `is_prime_cuda(n) == is_prime(n)` for every n. (4) Grep gate: no `{2,3,5,7,11,13,17,19,23,29,31,37}` in the CUDA TU — forbid v1's 12-base fallback from re-entering. |
| Overflow atomicity | Block-level `__syncthreads()` then single-thread emits zero-payload overflow TileOp | Two-pass: first computes `sum(n)` + `max_label`; if either overflows, skip fill, go directly to `memset(0); tile_flags=0x01` | K5 emits overflow from one tid after shared counters; compositor already skips overflow stitching | **B's two-pass discipline wins.** B correctly identifies that mid-fill reversion is fragile; the clean approach is to compute all overflow conditions first (remap.overflow from Phase C of K4, sum(n) from Phase 4 of K5, any n[f]>255 from Phase 3 of K5), latch a single `tile_overflow` flag in shared, and the encode-emit path branches on that flag exactly once. Matches `cpp-campaign-v2/src/tileop.cpp:151-155,238-240,260-262,268-270` which builds overflow outputs via `overflow_tileop()` that returns a fully zero `TileOp{}` with only `tile_flags = 0x01`. |

## 3. Canonical milestone sequence

Each milestone has a byte-parity gate, a prior-kernel anchor, a v2 CPU-ref anchor, a day estimate, and its dependency. The byte-parity gates are ordered so each downstream gate presupposes all upstream gates have passed — no gate can be skipped.

### M1 — Scaffold + CPU-oracle link + host-side stub round-trip. 0.5 day.
- **Goal:** `cuda-campaign-v2-sqrt-36/` builds; CMake `add_subdirectory(../cpp-campaign-v2 EXCLUDE_FROM_ALL)` links `libcampaign.a`; `cuda_vs_cpu_diff` CLI runs CPU via `campaign::process_tile`, stubs CUDA as identity passthrough, diffs bytes and exits 0 on 1-tile region.
- **Byte-parity gate:** trivially passes (CUDA = CPU passthrough). Validates CMake + struct marshaling.
- **Prior kernel:** none.
- **v2 CPU ref:** `cpp-campaign-v2/src/process_tile.cpp` (entry), `tileop.h:95-102` (signature).
- **Dependency:** none.

### M2 — LIFT K1+K2 verbatim + FJ64 parity. 1 day.
- **Goal:** Copy `kernel_sieve.cu` + `kernel_mr.cu` from `campaign-sqrt-36/tile_cuda_multi_kernel/src/`. Rename v1's `COLLAR = ceil_isqrt(K_SQ)` to v2's `C = floor_isqrt(K_SQ)` in `gpu_constants.cuh` (identical at K=36; divergent at K=40 — static_assert catches it). Update `MAX_PRIMES_GPU=6144`. Copy `fj64_262k_table.h` verbatim, assert SHA-256 matches `campaign_constants.h:28-29`.
- **Byte-parity gate:** for 16 test tiles at R=1000, K=36, reconstruct CPU halo bitmap from `sieve_tile(coord, constants)` by setting `bitmap[p.packed_pos]` per prime. `memcmp` against GPU bitmap. All 16 bit-identical. Secondary gate: for every `n` in `[R_inner²-K, R_outer²+K]` at R=1000, `is_prime_cuda(n) == is_prime(n)`.
- **Prior kernel:** `campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_sieve.cu` (LIFT), `kernel_mr.cu` (LIFT), `gpu_math.cuh` (LIFT).
- **v2 CPU ref:** `cpp-campaign-v2/src/sieve.cpp:80-121`, `primality.cpp:69-110`, `include/campaign/fj64_table.h`.
- **Dependency:** M1.

### M3 — LIFT K3 + K4 union pass + compression pass, parent-array parity. 1 day.
- **Goal:** Lift `kernel_compact.cu` and `kernel_uf.cu`. Sizing update `MAX_PRIMES_GPU=6144`. Re-derive `NUM_BACKWARD_OFFSETS` from `floor_isqrt(K_SQ)` (at K=36: 56 offsets; add `static_assert`). Keep UF Phase A (union via backward-offset scan) + Phase B (full compression) exactly as v1; do not add dense-remap or geo-flags yet.
- **Byte-parity gate:** download `d_parent[]` for 16 test tiles. CPU runs `build_local_dsu(primes)` + `dsu.find(i)` for each `i` in ascending order. GPU `parent[i] == cpu_find(i)` after compression. All 16 tiles match bit-for-bit.
- **Prior kernel:** `kernel_compact.cu` (LIFT), `kernel_uf.cu` (LIFT Phases A+B).
- **v2 CPU ref:** `cpp-campaign-v2/src/tileop.cpp:159-170` (build_local_dsu), `include/campaign/union_find.h:47-52` (smaller-root-wins), `src/union_find.cpp:43-53` (path-halving find).
- **Dependency:** M2.

### M4 — K4 geo-flag staging + single-threaded dense-remap. 1.5 days.
- **Goal:** Extend K4 with two new passes after Phase B. Phase B.5 (288-thread, per-prime): compute `(a, b, norm_sq)` from `(a_lo + col - C, b_lo + row - C, a²+b²)`; evaluate `is_inner_prime` + `is_outer_prime` via `eps_i`, `eps_o`, prefilter via `llabs(eps) <= prefilter_{inner,outer}`, full test via hand-coded `i128_sq_leq`. Stage 2-bit `prime_geo_bits[i]` in global. Phase C (**thread 0 only**, strictly single-threaded): `max_label=0; for i=0..N: root=parent[i]; if wire_label_by_raw_root[root]==0: if max_label>=128: overflow=1; break; else: wire_label_by_raw_root[root] = ++max_label`. Phase D (288-thread): read each prime's `raw_root`, look up `label = wire_label_by_raw_root[raw_root]`, `atomicOr` the 2-bit flag into `d_group_flags[tile_idx*32 + ((label-1)>>2)]` by bit position `2*((label-1)&3)`.
- **Byte-parity gate:** for 100 test tiles, `{parent[], wire_label_by_raw_root[], max_label, overflow, prime_geo_bits[], group_flags[]}` matches CPU bit-for-bit. Adversarial test: construct tile with 50 components where primes are sorted such that lowest-index prime's root is numerically HIGHER than a later prime's root — verify label permutation is by first-appearance-in-prime-index-order, not by root-ID order. (This is the v1-gap test B identifies at its §1.)
- **Prior kernel:** `kernel_uf.cu` (extended). No analogue for geo + dense-remap in v1.
- **v2 CPU ref:** `cpp-campaign-v2/src/tileop.cpp:172-204` (dense_remap_roots + _for_test), `src/geo_tests.cpp:22-67` (is_inner/is_outer), `src/process_tile.cpp:22-28` (flag pipeline), `include/campaign/campaign_constants.h:65-73` (prefilter + i128 constants), blueprint `campaign-blueprint.md:287-313`.
- **Dependency:** M3.

### M5 — K5 empty/overflow skeleton + face-strip filter. 0.5 day.
- **Goal:** New kernel `kernel_face_encode_v2.cu`. Skeleton: read `prime_count` from M3; if 0, single-thread writes `TileOp{tile_flags=0x02}`; if K4 set `overflow`, single-thread writes `TileOp{tile_flags=0x01}`; else stubs normal encode with placeholder zero-payload. Add Phase 1: per-face warp-scan filter producing `face_indices[f][M]` in ascending prime-index order, reading `on_face_strip(prime, coord, face)` via `face_perp` + C-boundary check (`cpp-campaign-v2/src/tileop.cpp:86-90`).
- **Byte-parity gate:** for tiles where `primes.empty()` OR `remap.overflow`, GPU TileOp matches CPU byte-for-byte (256 B of zero with 0x01 or 0x02 at offset 228). For any active tile, GPU `face_indices[f][]` matches CPU `build_face_ports` intermediate (dump via debug flag).
- **Prior kernel:** none (rewrite; cite `kernel_face_encode.cu:464-473` for face-prime shmem pattern only).
- **v2 CPU ref:** `cpp-campaign-v2/src/tileop.cpp:92-103` (face_indices), `:151-155` (overflow_tileop), `:230-233` (empty branch).
- **Dependency:** M4.

### M6 — K5 face-strip UF + canonical port sort + `face_groups` byte-parity. 1.5 days.
- **Goal:** Phase 2: per face (one warp per face = 4 warps), build face-DSU over `face_indices[f][M]` using the `(i<j)` nested-loop pair order from `tileop.cpp:106-114`; M ≤ ~50 in practice (blueprint `§5.4` cites ≤40). Reuse v1 `atomic_union` primitive. Phase 3: per face-DSU root (sorted ascending via `face_dsu.roots()`), scan `k=0..M` ascending, select lex-min `(h, p_perp)` representative with **strict `<`** (first-encountered tiebreak per `tileop.cpp:119-138`), record `(h, p_perp, global_wire_label = remap.wire_label_by_raw_root[local_dsu.find(face_indices[k])])`. Phase 4: warp-local bitonic sort over ≤50 ports per face with **3-key comparator** `(h, p_perp, global_wire_label)`. Phase 5: thread 0 writes `n[4]` + `face_groups[sum(n)]` in face order I,O,L,R.
- **Byte-parity gate:** for 100 test tiles + 5-tile K=36 golden, `memcmp(gpu.n, cpu.n, 4) == 0 && memcmp(gpu.face_groups, cpu.face_groups, 192) == 0`. Padding bytes `face_groups[sum(n)..192]` must be zero (B's R4 risk). Adversarial test: construct tile with two face-ports colliding at same `(h, p_perp)` — verify 3rd-key label tiebreak is applied.
- **Prior kernel:** none (rewrite; v1 K5 consecutive-pair clustering at `kernel_face_encode.cu:247-283` is anti-pattern #1).
- **v2 CPU ref:** `cpp-campaign-v2/src/tileop.cpp:92-148` (build_face_ports end-to-end), `:254-278` (face-order write loop), blueprint `campaign-blueprint.md:250-258` (canonical rule).
- **Dependency:** M5.

### M7 — K5 flag remap + terminal 256 B parity. 1 day.
- **Goal:** Phase 6: read `d_group_flags[tile_idx*32 + (dense_label>>2)]` for each dense label `1..max_label`, unpack 2-bit slot at `2*((label-1)&3)`, `bit_set` into `out.inner_flags[(label-1)>>3]` and `out.outer_flags[(label-1)>>3]` per bits. Full overflow gate: if any `n[f] > 255` OR `sum(n) > 192` at Phase 3/4 — latch shared `tile_overflow=1`, do NOT write any face_groups/flags, thread 0 writes overflow TileOp.
- **Byte-parity gate:** full 256 B `memcmp(gpu_tileop, cpu_tileop, 256) == 0` for every tile in a 1024-tile batch at R=10000, K=36. Zero differences.
- **Prior kernel:** none.
- **v2 CPU ref:** `cpp-campaign-v2/src/tileop.cpp:242-252` (flag loop), `:258-270` (per-face overflow gate), `include/campaign/tileop.h:110-119` (bit_set / bit_test).
- **Dependency:** M6.

### M8 — Host pipeline + 3-stream async + snapshot SHA parity at R=80M K=36. 1-1.5 days.
- **Goal:** `campaign_main_cuda.cpp` forks `cpp-campaign-v2/apps/campaign_main.cpp`; replace tile loop at `:426-432` with burst-chunked GPU dispatch (200k tiles per chunk per C's sizing). Three CUDA streams + three pinned buffer sets (stream A compute n, B D2H n-1, host compositor ingest n-2). Reuse CPU `write_snapshot` unmodified. Two-phase buffer overlay (v1 pattern) to fit <8.5 GB peak on 24 GB 4090.
- **Byte-parity gate (ship):** full K=36 R=80M run (8,166,667 active tiles). `sha256sum snapshot.bin` equals CPU golden. Secondary: tile-by-tile `memcmp` via `cuda_vs_cpu_diff --verbose`; first-divergence offset reported if any. Zero bytes diff = pass.
- **Prior kernel:** `campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu:316-389` (launch_pipeline shape, upgraded to async).
- **v2 CPU ref:** `cpp-campaign-v2/apps/campaign_main.cpp:250-494`, `src/snapshot.cpp:157-234` (snapshot writer with SHA-256 manifest).
- **Dependency:** M7.

### M9 — Perf tuning to 155k tiles/s. 1-2 days.
- **Goal:** Nsight Compute profile at M8 config. Test K2 `--maxrregcount=44` vs uncapped (v1 `dd56e2e` +2.2% evidence). Test K4 reg cap 40 vs 44 after geo addition. K5 shared-memory bank audit (C §6 risk #2). Compositor ingest overlap check.
- **Gate:** `tiles/sec >= 155000` at full R=80M K=36 on 4090. Snapshot bytes still match M8.
- **Prior kernel:** consult `docs/supportive/2026-04-10-multi-kernel-architecture.md:124-130` register-cap table as baseline.
- **v2 CPU ref:** none (pure perf).
- **Dependency:** M8.

### M10 (optional) — K=40 byte-parity. 1 day.
- **Goal:** Rebuild with `-DK_SQ=40`. `NUM_BACKWARD_OFFSETS` re-derives to 56 (floor_isqrt(40)=6, same as K=36); `static_assert` catches any drift. Re-run M7 + M8 gates at K=40, R=800M anchor.
- **Gate:** full K=40 R=800M snapshot SHA matches CPU.
- **Dependency:** M9.

**Total wall-clock: ~7-9 engineer-days for M1-M9 (K=36 ship criterion), +1 day for M10 (K=40).**

## 4. Consolidated risk register

Ordered by blast radius × likelihood.

1. **[B] Dense-remap parallelization temptation — blast: every tile with ≥2 components byte-different.** High likelihood; the perf team WILL see thread 0 idle 287 lanes and want to parallelize. Any warp-scan or `thrust::unique_by_key` scheme assigns labels by root-ID order, not first-appearance-in-prime-index order. Mitigation: hard-pin thread 0 compaction in review gate; CI test `test_dense_remap_adversarial` constructs a 50-component tile where root-ID order inverts prime-index order. Embed `// CORRECTNESS: DO NOT PARALLELIZE — see planning/2026-04-21-synthesis-canonical-plan.md §3 M4` at the Phase C site in `kernel_uf.cu`.

2. **[synth] Kernel-fusion drift toward A's 4-kernel vision under perf pressure.** Medium likelihood; if M9 misses 155k tiles/s, engineers will propose K4+K5 fusion to save a launch. Don't. K4@27% + K5@2.4% is dominated by K2@54%; fusion touches the two kernels whose work is most register-heterogeneous. Mitigation: re-read blueprint `campaign-blueprint.md:269-344` before accepting any K4+K5 fusion PR; require Nsight proof that unfused version is the bottleneck.

3. **[B] `thrust::sort` default is not stable + 2-key `(h, p_perp)` comparator.** High likelihood. Must use 3-key `(h, p_perp, global_wire_label)` comparator. Manifests as 1-byte shifts on tiles with `(h, p_perp)` port collisions — rare but present. Mitigation: warp-local bitonic sort with explicit 3-key functor; reject thrust entirely at 1-block-per-tile scale. CI: `test_port_sort_collision` with hand-crafted adversarial tile.

4. **[B] i128 emulation sign bug in geo tests.** Medium likelihood. `eps = norm - r_sq - K` can be negative near inner arc; `eps*eps` emulation via `uint2` must handle two's-complement. Mitigation: hand-code `i128_sq_leq(int64_t eps, u64 bound_hi, u64 bound_lo)` that takes signed eps, squares `|eps|` via `__umul64hi` + `__umul64lo`, compares to bound. CI sweep: `test_geo_i128` over `norm_sq ∈ [R_inner²-2K, R_inner²+2K] ∪ [R_outer²-2K, R_outer²+2K]` at R=1000. Unit-tested bit-for-bit against CPU.

5. **[C] K4 occupancy cliff from geo additions + dense-remap in one kernel.** Medium likelihood. i128 multiply adds registers; dense-remap adds ALU; pushing K4 past 44 regs drops 5→4 blocks/SM on 4090. Mitigation: measure actual K4 register use at M4 completion; if >44 regs, split Phase C + Phase D into `kernel_uf_b` (still single-threaded Phase C + parallel Phase D, but independent register cap). Do NOT push geo math into K5 — breaks the blueprint contract for `d_group_flags` layout.

6. **[C] `MAX_PRIMES_GPU=6144` memory inflation at 200k chunks.** Medium likelihood. Naive flat buffers are 14.4 GB; 24 GB 4090 has no slack for OS/driver. Mitigation: v1's two-phase buffer overlay (`campaign-sqrt-36/.../main.cu:240-252`) is MANDATORY. Re-use the K1/K2 `d_cand_list` space as K3/K4/K5's `d_prime_pos` + `d_parent` + `d_group_flags` space once K1/K2 have completed.

7. **[B] Overflow mid-fill non-atomicity.** Medium likelihood. If Stage 8 detects `sum(n) > 192` after Stage 6 has already written `inner_flags`, partial state leaks. Mitigation: two-pass overflow discipline — compute `remap.overflow`, `sum(n)`, `any n[f]>255` first; if any set, write zero TileOp with `tile_flags=0x01` and skip all other writes.

8. **[A+C] v1 COLLAR/backward-offset semantic drift at K=40.** Low for K=36 (byte-parity target); medium for K=40 extension. v1's `gpu_constants.cuh:36` derives `COLLAR=ceil_isqrt(K_SQ)`; v2 uses `floor_isqrt`. At K=36 both are 6 (coincident because 36 is square); at K=40 they diverge (7 vs 6). Mitigation: `static_assert(COLLAR_GPU == C)` fails the build; re-derive `c_bk_dr/c_bk_dc` arrays host-side from v2's constant.

## 5. Repo layout

```
tiles-maxxing/cuda-campaign-v2-sqrt-36/
├── CMakeLists.txt                      # -DK_SQ=36 default; add_subdirectory(../cpp-campaign-v2 EXCLUDE_FROM_ALL)
├── include/cuda_campaign/
│   ├── constants.cuh                   # mirrors cpp-campaign-v2/include/campaign/constants.h
│   ├── campaign_constants.cuh          # mirrors include/campaign/campaign_constants.h for __constant__ upload
│   ├── tileop.cuh                      # verbatim TileOp struct + static_asserts, device-callable
│   ├── fj64_table.cuh                  # verbatim copy of cpp-campaign-v2 include/campaign/fj64_table.h
│   ├── gpu_math.cuh                    # LIFT from v1: Barrett, Montgomery MR, __umul64hi helpers
│   ├── gpu_types.cuh                   # TileInput {a_lo, b_lo, i, j} 24 B SoA; slice pointer helpers
│   ├── i128_sq_leq.cuh                 # hand-coded signed-eps i128 sq + leq; NO generic i128 emu
│   └── kernels.cuh                     # K1-K5 launch-function API
├── src/
│   ├── kernel_sieve.cu                 # K1 — LIFT with COLLAR→C rename
│   ├── kernel_mr.cu                    # K2 — LIFT verbatim, FJ64 global buffer + SHA-256 gate
│   ├── kernel_compact.cu               # K3 — LIFT, update MAX_PRIMES_GPU=6144
│   ├── kernel_uf_v2.cu                 # K4 — Phases A (union) + B (compress) LIFT; B.5 (geo stage) NEW; C (thread-0 remap) NEW; D (flag accumulate) NEW
│   ├── kernel_face_encode_v2.cu        # K5 — REWRITE: face-strip UF, port sort, 256 B pack
│   ├── constants_upload.cu             # once-per-campaign upload of CampaignConstants, FJ64, Barrett, bk_offsets
│   ├── host_driver.cpp                 # 3-stream async pipeline, two-phase buffer overlay
│   └── main.cu                         # thin main; links `campaign_main_cuda` app
├── apps/
│   ├── campaign_main_cuda.cpp          # fork of cpp-campaign-v2/apps/campaign_main.cpp
│   └── cuda_vs_cpu_diff.cpp            # runs both, reports first-byte-diff offset + tile index
├── tests/
│   ├── test_k1k2_fj64_parity.cpp       # M2 gate
│   ├── test_k3k4_parent_parity.cpp     # M3 gate
│   ├── test_dense_remap_adversarial.cpp # M4 critical gate (50-component inverted-root-order test)
│   ├── test_geo_i128_sweep.cpp         # M4 gate (full band)
│   ├── test_face_groups_parity.cpp     # M6 gate
│   ├── test_port_sort_collision.cpp    # M6 gate (adversarial 3-key tiebreak)
│   ├── test_full_tileop_parity.cpp     # M7 gate (256 B memcmp over 1024 tiles)
│   └── test_snapshot_sha_R80M.cpp      # M8 ship gate
├── goldens/                            # symlinks to cpp-campaign-v2/goldens/
└── planning/
    ├── 2026-04-21-opus-reuse.md
    ├── 2026-04-21-opus-correctness.md
    ├── 2026-04-21-codex-performance.md
    └── 2026-04-21-synthesis-canonical-plan.md    # THIS FILE
```

**CMake strategy.** Two-level. Root `CMakeLists.txt` does `add_subdirectory(../cpp-campaign-v2 cpp_campaign_v2_build EXCLUDE_FROM_ALL)` — this builds `libcampaign.a` from CPU sources without installing them. The CUDA target `campaign_cuda` links `PRIVATE campaign`, inheriting `Grid`, `CampaignConstants`, `write_snapshot`, `Compositor`, `is_prime`, and the SHA-256 implementation. Use CUDA separable compilation (`CUDA_SEPARABLE_COMPILATION ON`) so each `kernel_*.cu` TU gets its own `--maxrregcount` cap — mandatory per `docs/supportive/2026-04-10-multi-kernel-architecture.md:120-130`. Per-TU flags via `set_source_files_properties(... COMPILE_FLAGS "--maxrregcount=40")`. Build knobs: `-DK_SQ=36` (default) or `-DK_SQ=40` propagates to both CPU and CUDA TUs through `target_compile_definitions`. `CMAKE_CUDA_ARCHITECTURES=89` default; 80/86/90 override via CLI.

**CPU oracle linking.** The CPU library is NOT rewritten or forked. `apps/campaign_main_cuda.cpp` calls `Grid::build`, `CampaignConstants::from_radii`, and `write_snapshot` directly from the CPU library; only the tile-processing loop is replaced with `gpu.dispatch_chunk(...)`. Tests invoke CPU `campaign::process_tile(coord, constants, grid)` in-process for side-by-side diffing.

## 6. Success gate

**Three acceptance scales, strictest last.**

1. **K=36 R=10000 byte-parity (M7 gate).** 1024 active tiles, <1 sec CPU, <200 ms CUDA. `memcmp` over 1024 × 256 = 262 KB payload must yield zero diffs. Padding `face_groups[sum(n)..192]` explicitly asserted zero.

2. **K=36 R=80M byte-parity (M8 ship gate).** 8,166,667 active tiles per `cpp-campaign-v2/src/grid.cpp:610-620` (C §1 arithmetic). 2.09 GB snapshot payload. `sha256sum snapshot.bin` from CUDA == `sha256sum snapshot.bin` from CPU-anchored golden. Manifest header (grid_params_hash, constants_hash, mr_witness_set_sha256) must also match — enforced by `cpp-campaign-v2/src/snapshot.cpp:157-234`.

3. **155k tiles/s throughput at K=36 R=80M on 4090 (M9 perf gate).** 8.17M tiles / 155k tiles/s = 52.7 s compute. End-to-end wall-clock target 60-80 s including grid-build (~1.28 s), transfers, compositor, snapshot I/O (per C §8 table).

4. **Optional K=40 R=800M byte-parity (M10 gate).** Full K=40 R=800M snapshot SHA matches CPU.

"Done" at each scale is **self-evidently verifiable via `cuda_vs_cpu_diff` exit code + `sha256sum` equality** — no human inspection of kernel code required to judge correctness. The verification gate is the tool, not the review.

## 7. Open questions (max 5)

1. **K=40 scope.** M10 is listed optional. Coordinator: is K=40 R=800M a ship requirement, a post-ship deliverable, or out-of-scope? Affects calendar by +1 day.

2. **5-tile K=36 golden existence.** Does `cpp-campaign-v2/goldens/5tile-k36-inner.snapshot.bin` exist and is it committed? B §7 Q3 flags this; if not yet materialized on the CPU side, M6's adversarial gates shift to synthetic tiles the CUDA port generates and diffs CPU→CPU. Please confirm or point to the file.

3. **24 GB 4090 confirmed, or is 16 GB in scope?** C §6 risk 3 projects 8.5 GB peak with two-phase overlay on 24 GB. 16 GB (4080/4090D variants) would require 100k-tile chunks instead of 200k — halves throughput headroom. Confirm vast.ai provisioning.

4. **K2 MR table placement final call.** B wanted `__constant__` upload of the 512 KB FJ64 table; I overrode to global with L2 warm-up because the table exceeds the 64 KB `__constant__` segment. If the coordinator has a specific reason to want constant memory (bank-conflict avoidance), the call is reversible — but sizeof(kFj64Table) = 524288 bytes, so constant memory is architecturally impossible. Confirming the global-memory placement is accepted.

5. **MR witness policy commitment.** Plan C's Q1 raises whether FJ64 is the operational oracle given v1's witness drift history. Ground-truth: `cpp-campaign-v2/src/primality.cpp:107-110` uses FJ64 + base-2. CUDA port adopts same. If coordinator wants Sinclair-7 or other set, this is a CPU-side decision first (affects golden) — flag now before M2 starts.

---

*End of canonical plan. Start execution at M1.*
