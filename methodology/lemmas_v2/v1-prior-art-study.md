---
title: v1 Prior-Art Study for cpp-campaign-v2
date: 2026-04-20
engine: coordinator
type: design-note
status: complete
refs: [methodology/lemmas_v2/cpp-campaign-v2-execution-plan.md, methodology/lemmas_v2/campaign-blueprint.md]
scope: "Engineering patterns from v1 (tile-cpp, tile_cuda_multi_kernel, tiles-compositor at tiles-maxxing/campaign-sqrt-36/). Correctness logic is NOT extracted — v1 and v2 disagree on is_inner/is_outer, TileOp format, compositor stitching."
---

## Framing

v1 is not ground truth for v2 correctness. This doc extracts engineering patterns — integer discipline, primality, DSU flavors, layout hygiene, sieve organization, build, tests, pitfalls, perf — that transcend the correctness math. Each finding is tagged **LIFT** or **AVOID**. Source paths are under `tiles-maxxing/campaign-sqrt-36/{tile-cpp,tile_cuda_multi_kernel,tiles-compositor}/`.

## A. Integer arithmetic discipline

- **LIFT.** `norm_sq` is `uint64_t`; intermediates are `unsigned __int128`. `tile-cpp/src/sieve.cpp:10, 27-31, 76-78` squares `abs_i64` operands as `u128` before narrowing to `u64`. Montgomery MR mulmod also uses `u128` (`:125-139`). v3 on CPU should match.
- **LIFT.** `ceil_isqrt` is a pure-integer monotone loop: `tile-cpp/include/constants.h:12-25`. `COLLAR = ceil_isqrt(K_SQ)` is a constexpr derived constant. No `floor_isqrt` in the hot path.
- **AVOID.** `tile-cpp/src/sieve.cpp:293-306` has `[[maybe_unused]] isqrt64` that seeds with `std::sqrt(long double)` and corrects by `u128`. Dead code, but a latent trap — v2 must forbid float sqrt outright. Long-double precision is not portable across architectures.
- **AVOID.** Float sqrt/cos/sin in `bench_tiles.cpp:28-39` and `census_tiles.cpp:281-289` — test-only, but v2's parity reference must use gmpy2 integer arithmetic.
- **CUDA DIVERGENCE.** v1 CUDA deliberately avoids `__int128` — `kernel_mr.cu:63-65` uses plain `uint64_t` with `__umul64hi` + manual carry (`gpu_math.cuh:56-65`). Do NOT mirror on CPU; CPU has `__int128` natively and losing it invites bugs for nothing.

## B. Miller-Rabin & primality

- **CPU witness set (v1 tile-cpp).** Tiered — `sieve.cpp:267-281`:
  - `n < 25 326 001` → {2, 3, 5}
  - `n < 3 215 031 751` → {2, 3, 5, 7}
  - `n < 2³²` → {2, 3, 5, 7, 11}
  - `n < 2⁶⁴` → `kLargeWitnesses = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37}` (`sieve.cpp:13-14`). Jaeschke/Sorenson 12-base: correct, not minimal.
- **CUDA witness set (v1 tile_cuda_multi_kernel).** FJ64 hash-table: base-2 MR + single indexed witness from a 262144-entry `uint16_t` table (`fj64_262k_table.h:1-4`; lookup `gpu_math.cuh:187-217`). Sinclair 7-base `{2, 325, 9375, 28178, 450775, 9780504, 1795265022}` still uploaded (`kernel_sieve.cu:197-200`) but **no longer used for norms**.
- **LIFT: trial-division prefilter.** `tile-cpp/src/sieve.cpp:16-19` hardcodes `kTrialPrimes = {3, 5, …, 97}` (24 primes). Applied at `:247-255`; branch `n < 97² → true` after prefilter.
- **LIFT: axis Gaussian prime handling.** `sieve.cpp:412-423`: `is_axis_gaussian_prime(coord)` = `|coord| != 0 ∧ |coord| ≡ 3 (mod 4) ∧ is_prime(|coord|)`. `is_gaussian_prime_point(a,b)` dispatches `a==0 → axis(b)`, `b==0 → axis(a)`, else `is_prime(norm)`. Split primes `p ≡ 1 (mod 4)` < 10⁴ store `sqrt(-1) mod p`; inert primes `p ≡ 3 (mod 4)` < 10⁴ stored separately, mark residues only when `a ≡ 0 (mod p)` (`:458-510, :531-551`).
- **AVOID: silent CPU/CUDA witness divergence.** v1 CPU 12-base vs CUDA FJ64 — no one documented why. v2 must pin ONE set in `campaign_constants.h` and have the CUDA port brief hard-constrain a match.

## C. DSU / Union-Find

- **LIFT: deterministic smaller-root-wins.** Both v1 paths use lower-root, not rank/size. `tile-cpp/src/union_find.cpp:16-36` — path halving in `find_root`; union swaps so `ra ≤ rb` then `parent[rb] = ra`. `tiles-compositor/src/compositor.cpp:240-262` — identical. Logic doc `tiles-compositor/docs/supportive/2026-04-12-compositor-logic.md:576` calls this "deliberate for reproducibility: higher ID toward lower ID". Matches blueprint §5.4.
- **LIFT: `uint16_t` parent array, `MAX_PRIMES <= 65535`.** Cache-friendly. `tile-cpp/src/union_find.cpp:10`.
- **LIFT: incremental reachability latch.** `tiles-compositor/include/compositor.h:31-44` stores `root_reach_[i]` bits; `mark_inner/mark_outer` OR bits into the current root; `unite` ORs during merge (`compositor.cpp:259-265`). `has_spanning()` returns a cached flag (`:62-69`). This replaces an O(N²) scan (`bb10035`). v2 keeps the pattern but attaches flags per v3 group, not v1 staircase collectors.
- No SIMD in v1 DSU; popcount/ctz via `__builtin_*`. v2 can defer SIMD.

## D. TileOp / payload layout

v1's TileOp is NOT blueprint v3 — layout is wholesale AVOID:

- v1 struct is `uint8_t bytes[TILEOP_SIZE]` with no fixed-offset fields (`tile-cpp/include/types.h:11-13`; CUDA `gpu_types.cuh:10-12`). `static_assert(sizeof(TileOp) == 256)` is present in CUDA (`:118`) but MISSING on CPU.
- Header bytes 0-2 are variable offsets `off_I/off_L/off_R`; payload order O, I, L, R groups, then L/R `h1` bytes (`tile-cpp/src/encode.cpp:85-149, :203-223`; `kernel_face_encode.cu:425-441`).
- **Bit-stealing trap:** L/R group bytes steal bit 7 for `h1` MSB → 7-bit group labels, groups > 127 overflow (`encode.cpp:14-19, :185-190`). This caused `7e28a44`: `decode_group_id(& 0x7F)` silently truncated I/O face labels (which had full 8-bit IDs); fix was to read I/O raw and poison tiles with ≥128 groups.
- Overflow sentinel: all 256 bytes `0xFF`; parser checks `bytes[0] == 0xFF` (`constants.h:39-43`).
- Empty TileOp: offsets `3, 3, 3`.

Patterns to LIFT:

- **`static_assert(sizeof(TileOp) == 256)`**. v1 CPU was missing it. v2 M4 gate (e) already specifies.
- **Poison-on-overflow sequencing** (`2b54b6d` K5 fix): set `scratch->overflow = 1` BEFORE clamping face counts, poison output at the end. Applies to v3 OVERFLOW_BIT (sum(n) > 192 or max_label > 128).

Patterns to AVOID:

- **Parser accepts odd residual.** `tiles-compositor/src/tileop_parse.cpp:43-62` enforces offset monotonicity but accepts `residual % 2 != 0` (AUDIT-FINAL.md:5-7). v2 must reject.
- **Derived implicit R count + zero-padded slots.** `tileop_parse.cpp:62, 116-129`. v3 `n[4]` is authoritative — do not regress.

## E. Sieve organization

- **LIFT: halo-expanded per-tile bitmap.** `tile-cpp/src/sieve.cpp:513-522` runs on the region `(a_lo - COLLAR, b_lo - COLLAR)` up. Row-major, per-row bitmap of `kRowWords = (SIDE_EXP + 31) / 32`. CUDA kernel identical in spirit (`kernel_sieve.cu:124-127`).
- **LIFT: layered candidate generation.** Per row: (1) parity composites, (2) split-prime residue classes, (3) inert axis-aligned residue class, (4) `is_prime(norm)` on survivors (`sieve.cpp:524-587`).
- **LIFT: compact via word-popcount prefix + ctz.** `tile-cpp/src/compact.cpp:22-49` — `counts[]` via `__builtin_popcount`, cumulative `prefix[]`, emit via `__builtin_ctz` + `word &= (word - 1)`. Byte-for-byte portable.
- **LIFT (CUDA-only, defer):** Barrett mod for residue marking with precomputed `mu` + `__umulhi` (`gpu_math.cuh:23-35, 38-50`). On CPU plain `%` is fine.
- **AVOID: no octant clipping.** Neither CPU `sieve_tile()` nor `kernel_sieve` clips. v1 bench/census do it externally (`bench_tiles.cpp:27-44`). v2 must push octant clipping into `grid.cpp` + `sieve.cpp` per blueprint §6.2.
- **AVOID: caller-zeroed bitmap contract.** v1 `sieve_tile` originally required it (flagged in `prompts/fix_audit_bugs.md:59`). v2 must zero internally.

## F. Build & CI discipline

- **LIFT: `-DK_SQ_VAL=<n>` cache var + per-K build dirs.** CPU `CMakeLists.txt:8-14, :27-65` defaults 40. CUDA `Makefile:3-14` — `make K_SQ=36` sets `OBJ_DIR := build-k$(K_SQ)`. v2 plan M1 matches.
- **LIFT (CUDA, defer): per-kernel `--maxrregcount`.** `Makefile:17-22` — Sieve 40, MR 44, Compact 32, UF 40, Face 40. Commit `dd56e2e`: MR @ 44 beat uncapped by 2.2% (spills at 40 killed 5-block/SM).
- **AVOID: raw `assert()` + home-rolled `expect_*` helpers.** `tests/test_compact_uf.cpp:19-93`, `test_face_encode.cpp:13-45`. Zero `gtest_discover_tests` integration, opaque CI signal. v2 plan C/Q6 correctly chooses GoogleTest.
- v1 had zero third-party deps (no boost, no json, no fmt). v2 adds GoogleTest + nlohmann/json — both lightweight, both fine.

## G. Test harness patterns

- **LIFT: determinism-by-repeated-run.** `tile-cpp/tests/test_sieve.cpp:56-84` runs N times on a fixed tile, asserts bit-identical. v2 should do this per component via snapshot-hash self-compare.
- **LIFT: binary dump + Python byte-compare as parity harness.** `tile-cpp/tests/dump_tileops.cpp:1-13, :227-249` + `tests/compare_tileops.py`. v2 `compare_snapshots` (plan §3 C12) is the evolution.
- **AVOID: Python reference that re-implements face semantics.** v1's `compare_tileops.py` and Python validator had drift: face-label transposition (`prompts/fix-python-validator.md:5-10`) and port-clustering mismatch (`prompts/ports-mismatch-investigation.md:4, 10, 18`). v2 Python stays sieve-enumeration-only (gmpy2).
- Compositor test gaps (`tiles-compositor/AUDIT-FINAL.md:8-9, :69-74`): uncovered `q=31`, `q=32`, `f=255`, `secondary_prev_row < 0`, compositor-level zero-port faces. v2 must add.

## H. Pitfalls from git history

Each row: SHA — bug — fix — meta-pattern v2 should internalize.

- **`2b54b6d` K5 overflow poison.** Face-prime overflow silently truncated `face_prime_counts`. Fix: set `scratch->overflow = 1` BEFORE clamping; K5 poisons output (`kernel_face_encode.cu:221-227, :475-484`). *Never truncate without marking overflow first.*
- **`46d73db` depth-6 face extraction.** Strict `< COLLAR` / `>= TILE_SIDE - COLLAR + 1` excluded distance-exactly-6 at K=36. Fix: `<= COLLAR` / `>= TILE_SIDE - COLLAR` (`kernel_face_encode.cu:28-50`). *Face-depth boundary off-by-ones are recurring; test at `depth == COLLAR` for every K.* Same species as `ceil_isqrt(K)` vs `floor_isqrt(K)`.
- **`5997aa1` Sinclair 7-base MR, delete redundant trial division.** Later superseded by FJ64. *Pick ONE MR strategy and document why.*
- **`bb10035` O(N²) spanning → incremental reachability.** `has_spanning()` rebuilt inner/outer sets from scratch: 3.5h at R=80M / 221K towers. Fix: cache `spanning_detected_` on the DSU. *Never walk accumulator state at verdict time.*
- **`7e28a44` poison ≥128 groups + stop `decode_group_id` on I/O.** Two bugs: (a) L/R 7-bit labels made ≥128-group tiles structurally unreliable; (b) `decode_group_id(& 0x7F)` truncated I/O labels (full 8-bit). *If faces have different layouts, type-tag the decode path.*
- **`0e9d9a7` between-tower row index sign.** `row + q` should have been `row - q`. *Write the derivation next to the sign-sensitive code.*
- **`cf2d0e6` extend grid past diagonal.** Tiles at `y=x` had no Face I partner → severed diagonal paths. Fix: one extra row below diagonal (`tiles-compositor/include/grid.h:14-20`). *Diagonal boundaries in snapped grids need an explicit cross-diagonal test.*
- **`cc71ab4` "ports being non deterministic in some cases".** Commit body terse. Best-supported reading (cross-ref `prompts/ports-mismatch-investigation.md:20` + `fix_audit_bugs.md:7, 24`): `cluster_face_ports` does linear h-sorted consecutive-pair scan — adjacent tiles disagree on cluster boundaries when the "bridge" prime is in one halo but not the other. v1 also had a Python validator using full-cluster reverse scan → permanent C++↔Python divergence. *Canonical port identity is the whole game; if clustering depends on within-tile visibility, neighbors will disagree.* Blueprint §5.4 snapped-grid positional ordinals are the fix.
- **`30722ae` K=36 post-mortem: 22% overflow.** Caps set against median, not worst-case. *First R you measure is not worst case.*

## I. Performance patterns

- **Throughput floor.** v1 `tile-cpp` census at R=860M: **~1003 tiles/s** single-threaded, mean 2044 primes/tile, mean 51.1 ports post-prune, zero overflow (`census_output/census_R860000000_T3125.summary.txt:8-35`). v2 target is the same ceiling.
- **Compositor memory.** Streaming-by-tower but global `parent_`, `root_reach_`, `inner_members_`, `outer_members_` in RAM (`tiles-compositor/include/compositor.h:31-40`). `0e9d9a7` post-fix run R=850M / 75M tiles: SPANNING, 733M groups, 56s wall, **5019 MB peak RSS**.
- **Phase timing hook to lift.** `tile-cpp/src/process_tile.cpp:40-97` — `steady_clock` per-phase, `PhaseTimings` struct. Cheap; makes M6 perf tuning trivial. v2 should include from M1.
- CUDA tuning (defer to port): `9814a1c` unroll UF popcount +4.1%; `dd56e2e` MR reg cap 44 +2.2%.

## J. Forbidden list for v2

Ten named v1 anti-patterns:

1. **Consecutive-pair port clustering** (`tile-cpp/src/face_extract.cpp:124-161, :146-149`). Use v3 canonical positional ordinal.
2. **Empty-TileOp-on-overflow** (`tiles-compositor/src/campaign.cpp:610-621, :743-770`) as workaround to dodge `assert_not_overflow` (`compositor.cpp:7-12`). Unsound; v3 wants conservative-SPANNING bias.
3. **Bit 7 of L/R group bytes stolen for `h1` MSB** (`tile-cpp/src/encode.cpp:14-19, :185-190`). Killed group label space; seeded the `decode_group_id` landmine.
4. **Shared `decode_group_id` across face types.** Cause of `7e28a44`. Type-tag I/O vs L/R decode.
5. **`[[maybe_unused]] isqrt64` using `std::sqrt(long double)`** (`tile-cpp/src/sieve.cpp:293-306`). Forbid float math in sieve TU.
6. **Missing `static_assert(sizeof(TileOp) == 256)` on CPU** (CUDA had it at `gpu_types.cuh:118`).
7. **Derived `r_cnt` + implicit zero-padded R slots** (`tiles-compositor/src/tileop_parse.cpp:62, 116-129`).
8. **O(N²) spanning verdict** (fixed in `bb10035`; do not regress).
9. **Caller-zeroed bitmap contract** (`prompts/fix_audit_bugs.md:59`). Zero internally.
10. **Silent MR witness drift between CPU (12-base) and CUDA (FJ64)**, no rationale in code. Pin one in `campaign_constants.h`.

## K. Confidence and gaps

- **High** on A, B, C, D, E, H, J — three explorer reads + scout supplementary + direct spot-reads of sieve/face_extract/compact/process_tile, triangulated against commit messages.
- **Medium** on F CI — v1 has no `.github/workflows/` at campaign-sqrt-36 level. v2's M1 CI spec stands alone.
- **Medium** on `cc71ab4` nondeterminism — inference from consistent signals in code + prompts, no definitive post-mortem. Low risk because v3 replaces clustering entirely.
- **Low** on compositor integration tests — read via AUDIT-FINAL summary only.

---

## Proposed amendments to cpp-campaign-v2-execution-plan.md

Minimal, concrete, numbered for easy apply.

### Amendment 1 — §3 C8 / M4 brief
**Add to M4 brief template (Codex impl):** "Forbid bit 7 of L/R group bytes being used for any payload. Blueprint v3 uses fixed-offset `n[4]`, `face_groups[192]`, `inner_flags`, `outer_flags`, `tile_flags` — no bit-stealing. Enforce with `static_assert(offsetof(TileOp, face_groups) == N)` at each field."

### Amendment 2 — §3 C7 / M3 acceptance gate
**Add to M3 acceptance gate (b):** "Pin MR witness set in `include/campaign/campaign_constants.h` as `constexpr uint64_t kMillerRabinWitnesses[]` with a comment citing the deterministic bound (recommend FJ64 hash-table approach per `tile_cuda_multi_kernel/include/fj64_262k_table.h`, or Sinclair 7-base `{2, 325, 9375, 28178, 450775, 9780504, 1795265022}`). Add a compile-time assert that the array is non-empty and a runtime assert that it is applied identically on every `is_prime` call."

### Amendment 3 — §3 C5 / M2 brief (Codex impl)
**Replace** "Sieve uses Miller–Rabin for `n ≡ 1 (mod 4)` candidates; inert primes `q ≡ 3 (mod 4)` trial-divide to some bound then MR." **with:** "Sieve candidate generation layers: (1) parity composite marking, (2) split-prime residue classes, (3) inert-prime axis-aligned residue class, (4) `is_prime(norm)` verification using the pinned MR witness set. Pattern reference: `tile-cpp/src/sieve.cpp:524-587`. Forbid any `std::sqrt` / `std::sqrtf` / `std::sqrtl` in the sieve translation unit — add `#pragma GCC poison sqrt sqrtf sqrtl` at file top."

### Amendment 4 — §3 C6 / M3 brief
**Add to brief:** "DSU union rule: after `find(a)` and `find(b)`, swap so `ra ≤ rb`, then `parent[rb] = ra`. Add a unit test that builds a chain {5, 3, 17, 2, 9} via successive unions in random order and asserts the final root is `2` regardless of input order (pattern reference: `tile-cpp/src/union_find.cpp:24-35`)."

### Amendment 5 — new §6.3 Forbidden patterns inherited from v1

**Add as new subsection after §6.2. Briefs should paste verbatim.**

> ### 6.3 Forbidden patterns inherited from v1
> 1. No `std::sqrt` / `sqrtf` / `sqrtl` in production paths. Test generators only.
> 2. No bit-stealing inside group/port bytes. Fields are per blueprint §5.2 fixed offsets.
> 3. No consecutive-pair port clustering. Port identity is positional-ordinal per blueprint §5.4.
> 4. No empty-TileOp substitution for overflow. Emit OVERFLOW_BIT and conservative-SPANNING bias.
> 5. No derived `r_cnt` or zero-padded implicit counts. `n[4]` is authoritative.
> 6. No shared decode path across I/O and L/R faces. Type-tag the decode.
> 7. No caller-zeroed buffer contracts. Zero internally or refuse to run.
> 8. No Python reference re-implementing face extraction or clustering. Python is sieve-enumeration-only (gmpy2).
> 9. No O(N²) verdict queries. Spanning is a cached bit on the DSU root.
> 10. Pre-filter uses `ceil_isqrt(K)`, never `floor_isqrt(K)`. (Recurring boundary bug, commit `46d73db`.)

### Amendment 6 — §5 Testing strategy
**Append bullet:** "Determinism-by-repetition: for every component, add a test that runs N≥3 times on the same input and asserts bit-identical output. Pattern reference: `tile-cpp/tests/test_sieve.cpp:56-84`."

### Amendment 7 — §7 Risk register
**Add row R10:**

| Risk | Likelihood | Blast radius | Mitigation |
|---|---|---|---|
| R10. MR witness set drifts between v2 and its future CUDA port (v1 had CPU 12-base vs CUDA FJ64 mismatch) | med | Parity spot-checks fail; debugging nightmare | Pin witness set in `campaign_constants.h`; CUDA port brief includes "MUST MATCH `kMillerRabinWitnesses[]`" as hard constraint. |

### Amendment 8 — §9 Scaffolding Day-0 punch-list
**Insert new item 1.5 between items 1 and 2:**
> 1.5 Add `#pragma GCC poison sqrt sqrtf sqrtl` to `include/campaign/sieve.h` top (only active under `#ifdef CAMPAIGN_STRICT`).

### Amendment 9 — §3 C10 / M5 brief
**Add to snapshot format spec:** "Snapshot header includes `mr_witness_set_sha256` in addition to `grid_params_hash` and `constants_hash`. CUDA port parity is gated on matching all three."

### Amendment 10 — §10 Open questions
**Resolve Q6 affirmatively with rationale:** "Q6: GoogleTest — CONFIRMED. v1 used home-rolled `assert/expect_*` helpers (`tile-cpp/tests/test_compact_uf.cpp:19-93`), which had zero `gtest_discover_tests` integration and made CI signal opaque. GoogleTest is the upgrade."

---

*End of study.*
