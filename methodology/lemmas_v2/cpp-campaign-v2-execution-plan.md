---
title: cpp-campaign-v2 Execution Plan
date: 2026-04-20
engine: coordinator
type: design-note
status: complete
refs: [methodology/lemmas_v2/campaign-blueprint.md, methodology/lemmas_v2/tile-operator-definition-v-claude.md, methodology/lemmas_v2/BACKLOG.md, AGENTS.md]
---

## 1. Executive summary

We build `tiles-maxxing/cpp-campaign-v2/` — a single, unified, self-contained C++ reference campaign — as the **ground truth** the later CUDA port will be byte-compared against. Scope: the full Blueprint v3 pipeline end-to-end (grid → sieve+axis primes → tile UF → face-strip UF → TileOp encode → compositor → verdict → snapshot), implemented in plain C++20 on a Mac Mini / Linux x64 host. No GPU. No Jetson SSH dance. No Python hot path.

The load-bearing mental model is **sub-region = self-contained campaign**. Every invocation takes a region spec (defaulting to the full canonical octant) and runs the complete pipeline against it, emitting `snapshot.bin + manifest.json`. A 3-tile test region and a 10 M-tile full-octant production run are the **same binary, same code paths, same output format**. This collapses unit tests, integration tests, and the eventual CUDA-parity gate into one primitive: `compare_snapshots A B`.

**First slice target:** a hand-golden 5-tile region near `R_inner` at `K = 36`, processed end-to-end, producing a snapshot that matches a manually-traced expected output byte-for-byte. Ship that, ship trust. Everything after is scaling and polish.

## 2. Folder structure

```
tiles-maxxing/cpp-campaign-v2/
├── CMakeLists.txt              # Top-level. -DK_SQ=36 build flag; pulls bz_check.py as pre-build target.
├── README.md                   # One page: what/why/how-to-run. No deep docs here.
├── include/campaign/           # Public headers; one per component.
│   ├── constants.h             # S=256, TILEOP_SIZE=256, C=floor_isqrt(K_SQ), face enum, flag bits.
│   ├── campaign_constants.h    # R_inner/R_outer/prefilter/four_rin_sq_k, derived at init.
│   ├── grid.h                  # Grid struct + TileCoord + enumerate_active_tiles().
│   ├── sieve.h                 # Halo Gaussian-prime sieve API (incl. axis primes).
│   ├── union_find.h            # Tile-local DSU + generic int-indexed DSU.
│   ├── geo_tests.h             # is_inner_prime / is_outer_prime (two-stage int64→i128).
│   ├── tileop.h                # TileOp struct (packed 256 B) + face-strip UF + canonical port sort.
│   ├── compositor.h            # ingest_column + global DSU + spanning latch.
│   ├── snapshot.h              # Snapshot writer + manifest schema.
│   └── region.h                # Region spec (tile-index box or full-octant) parsing.
├── src/                        # Implementations; filenames mirror headers.
│   ├── grid.cpp
│   ├── sieve.cpp               # Includes axis-tower y-axis prime enumeration (option B).
│   ├── union_find.cpp
│   ├── geo_tests.cpp
│   ├── tileop.cpp
│   ├── compositor.cpp
│   ├── snapshot.cpp
│   ├── region.cpp
│   └── process_tile.cpp        # Orchestrates sieve→UF→geo→face-strip→encode per tile.
├── apps/
│   ├── campaign_main.cpp       # Primary driver: argv → Region → run → snapshot.
│   └── compare_snapshots.cpp   # Byte-level diff tool (snapshot.bin + manifest.json).
├── tests/                      # GoogleTest suites; one file per component.
│   ├── test_grid.cpp
│   ├── test_sieve.cpp
│   ├── test_union_find.cpp
│   ├── test_geo_tests.cpp
│   ├── test_tileop.cpp
│   ├── test_compositor.cpp
│   ├── test_snapshot.cpp
│   └── test_golden_5tile.cpp   # The hand-traced regression.
├── goldens/                    # Locked-in expected outputs.
│   ├── 5tile-k36-inner.manifest.json
│   └── 5tile-k36-inner.snapshot.bin
├── scripts/
│   ├── run_full_octant.sh      # Production invocation.
│   └── bz_check.py             # Copied from /build/bz_check.py at CMake time.
└── .github/workflows/ci.yml    # Ubuntu + macOS matrix, -DK_SQ=36 and 40.
```

Justifications: **one tree, not three** (v1 was `tile-cpp + tile_cuda_multi_kernel + tiles-compositor` split for host/GPU separation — irrelevant here). **`include/campaign/` prefix** forestalls header clashes when the CUDA port later links against us. **`goldens/` committed to git** — the golden is the contract. **`apps/` separate from `src/`** so the library is linkable from tests without main collision.

## 3. Component decomposition

| # | Component | Purpose | Public API sketch | Deps | LOC | Build position |
|---|---|---|---|---|---|---|
| C1 | `constants` | Compile-time constexpr hub | `constexpr int S, C, TILEOP_SIZE; enum Face` | — | 80 | M1 |
| C2 | `campaign_constants` | Runtime campaign params + derived i128 squares | `CampaignConstants::from_radii(R_i, R_o, K)` | C1 | 150 | M1 |
| C3 | `region` | Sub-region spec parsing | `Region::full_octant(grid)`, `Region::from_tile_box(i0,i1,j_ranges)` | C1 | 200 | M1 |
| C4 | `grid` | Snapped-grid builder; I1/I2/I4 assertions | `Grid::build(R_i,R_o,K,offset)`, `enumerate_active_tiles()` | C1,C2 | 450 | M2 |
| C5 | `sieve` | Halo Gaussian-prime enumeration, incl. y-axis primes at x=0 | `sieve_tile(TileCoord, CampaignConstants) -> vector<Prime>` | C1,C2 | 400 | M2 |
| C6 | `union_find` | Rank-path-compressed DSU; deterministic smaller-root-wins | `DSU::find/union/roots()` | — | 120 | M3 |
| C7 | `geo_tests` | Two-stage is_inner / is_outer norm-form test | `bool is_inner_prime(i64 norm_sq, CampaignConstants)` | C2 | 150 | M3 |
| C8 | `tileop` | Tile pipeline: local UF → face-strip UF → dense-remap → 256 B encode | `process_tile(TileCoord, ...) -> TileOp`; struct `TileOp` | C1,C5,C6,C7 | 800 | M4 |
| C9 | `compositor` | Cross-tile DSU, flag-driven port marking, verdict latch | `Compositor::ingest_column`, `finalize()` | C1,C6,C8 | 350 | M5 |
| C10 | `snapshot` | Binary snapshot emit + manifest.json | `write_snapshot(path, grid, tileops, constants_hash)` | C1,C4,C8 | 300 | M5 |
| C11 | `campaign_main` | CLI driver | `argv → Region → run → snapshot` | all | 200 | M5 |
| C12 | `compare_snapshots` | Byte diff + manifest cross-check | standalone binary | C10 | 250 | M5 |

Total: ~3,450 LOC — lean. No boost, no third-party beyond GoogleTest and nlohmann/json (vendored as single-header).

C10 snapshot hashing: `campaign_constants_hash` is canonical logical-field serialization — format `K=<K>;R_inner=<R_i>;R_outer=<R_o>;offset=<ox>,<oy>;collar=<C>` as UTF-8 bytes — then SHA-256 of that string. Stable across compiler/toolchain changes. Struct-byte hash is fragile against padding, alignment, and field-reorder; logical string is byte-stable across any platform.

## 4. Milestone plan

### M1 — Scaffolding & constants (Day 0–1, half-day wall clock)
- **Goal:** Empty but compiling skeleton; `campaign_main --help` prints; CI green on empty tests.
- **Components:** C1, C2, C3 stubs; CMakeLists; CI workflow; README.
- **Acceptance gate:** `cmake -DK_SQ=36 -S . -B build && cmake --build build && ctest` passes on Mac + Ubuntu; `./build/campaign_main --help` exits 0.
- **Worker:** codex gpt-5.4 xhigh (mechanical scaffolding).
- **Brief template:** "Create tiles-maxxing/cpp-campaign-v2 tree at `<path>` matching the folder layout in §2 of the execution plan. Populate CMakeLists.txt with C++20, GoogleTest via FetchContent, -DK_SQ as a required cache var, and a pre-build custom_target running `scripts/bz_check.py`. Create stub headers/sources that compile to empty objects. Stub campaign_main.cpp as argv-parser with `--help`. CI matrix: ubuntu-latest + macos-latest, K_SQ ∈ {36, 40}. No logic yet."
- **Estimate:** 4 hours.
- **Deps:** none.

### M2 — Grid + sieve with axis primes (Day 2–4)
- **Goal:** Build a snapped Grid for given radii; enumerate active tiles; sieve returns Gaussian primes (including x=0 y-axis primes) for any TileCoord.
- **Components:** C4, C5.
- **Acceptance gate:** (a) `test_grid.cpp` passes `coverage_verifier.py` semantics at project parameters (8.18M tiles, I1/I2/I4 hold — we port the checks into C++ assertions). (b) `test_sieve.cpp` enumerates Gaussian primes in a 257×257 tile near (1M, 1M); count and set match a Python reference generated via gmpy2 (one-shot). (c) Axis-tower: sieve on column i=0 (offset (1,1) means first real column is still > 0, but we add an explicit unit test forcing a tile at `(a_lo=0, b_lo=1000)` and verify primes `(0, q)` with `q ≡ 3 (mod 4)` appear).
- **Worker:** Opus 4.7 designs the sieve API and axis-prime handling pseudocode (1 dispatch). Codex gpt-5.4 xhigh implements + tests (2 dispatches, sequential).
- **Brief template (Opus design):** "Spec the halo-sieve API for tiles-maxxing/cpp-campaign-v2. Inputs: TileCoord, CampaignConstants. Output: vector<Prime{a, b, norm_sq}> over the 269×269 halo-expanded region, octant-clipped per blueprint §6.2. Include y-axis primes `(0, q), q ≡ 3 mod 4` when the halo dips to x=0. Specify the Python reference generator used for sieve parity testing. Deliver: header comments + 1-page note. 400 words max."
- **Brief template (Codex impl):** "Implement `include/campaign/{grid,sieve}.h` and `src/{grid,sieve}.cpp` per design spec at `<design_doc_path>`. Integer arithmetic only (i128 for R_outer²); no `llround(sqrt())`. Tests in `tests/test_{grid,sieve}.cpp`. Sieve candidate generation layers: (1) parity composite marking, (2) split-prime residue classes, (3) inert-prime axis-aligned residue class, (4) `is_prime(norm)` verification using the pinned MR witness set. Pattern reference: `tile-cpp/src/sieve.cpp:524-587`. Forbid any `std::sqrt` / `std::sqrtf` / `std::sqrtl` in the sieve translation unit — add `#pragma GCC poison sqrt sqrtf sqrtl` at file top. Must pass acceptance gates (a), (b), (c) in plan §4 M2."
- **Estimate:** 12 hours Codex + 1 hour Opus design = 1.5 wall-clock days.
- **Deps:** M1.

### M3 — UF + geo tests (Day 4–5)
- **Goal:** Per-tile DSU that unions Gaussian primes at squared distance ≤ K; geo tests flag per-prime inner/outer with pre-filter + i128 fallback.
- **Components:** C6, C7.
- **Acceptance gate:** (a) UF unit tests: chain of 10 primes, star topology, disjoint pairs — all produce deterministic root assignments. (b) Geo-test unit tests hit the BZ boundary: synthetic primes with `norm_sq = R_inner² + K` and `norm_sq = R_inner² − K` and `norm_sq` inside BZ (should not exist per bz_check, but we test the code path with synthetic inputs). Pin MR witness set in `include/campaign/campaign_constants.h` as `constexpr uint64_t kMillerRabinWitnesses[]` with a comment citing the deterministic bound (recommend FJ64 hash-table approach per `tile_cuda_multi_kernel/include/fj64_262k_table.h`, or Sinclair 7-base `{2, 325, 9375, 28178, 450775, 9780504, 1795265022}`). Add a compile-time assert that the array is non-empty and a runtime assert that it is applied identically on every `is_prime` call. (c) Pre-filter correctness: assert `is_inner_prime` uses `⌈√K⌉` not `⌊√K⌋` via a targeted test with `|ε| ∈ (2R·C, 2R·√K]` at non-square K=40.
- **Worker:** Codex gpt-5.4 xhigh (implementation is well-specified by blueprint §2 and §6.2).
- **Brief template:** "Implement `union_find.h/cpp` and `geo_tests.h/cpp` per blueprint §2 (integer-overflow pre-filter) and AGENTS.md §Geo-test Integration. The DSU must be deterministic: smaller-root-wins tiebreak. DSU union rule: after `find(a)` and `find(b)`, swap so `ra ≤ rb`, then `parent[rb] = ra`. Add a unit test that builds a chain {5, 3, 17, 2, 9} via successive unions in random order and asserts the final root is `2` regardless of input order (pattern reference: `tile-cpp/src/union_find.cpp:24-35`). Pre-filter uses `ceil_isqrt(K)`; add compile-time and runtime asserts forbidding floor. Tests in `tests/test_{union_find,geo_tests}.cpp` must include the non-square K=40 boundary test at `|ε| ∈ (2R·6, 2R·6.32]`."
- **Estimate:** 6 hours.
- **Deps:** M2 (needs Prime type).

### M4 — TileOp: face-strip UF + dense remap + encode (Day 6–9)
- **Goal:** Complete tile processing — local UF → dense-label compaction → face-strip UF → canonical port sort → 256 B packed TileOp. Deterministic and byte-stable.
- **Components:** C8. This is the **highest-risk milestone** — it's where math-doc fidelity actually gets tested.
- **Acceptance gate:** (a) Unit tests for dense-remap: raw DSU roots {3, 17, 42, 88} compact to {0, 1, 2, 3}. (b) Face-strip UF test: hand-constructed face with 10 primes in 3 connected components returns 3 ports with deterministic ordinals via `(h, p⊥)` lex. (c) TileOp encoding test: known 5-prime tile encodes to a specific 256-byte blob, hash matches committed golden. (d) Overflow: synthetic `sum(n) = 193` tile sets OVERFLOW_BIT and zeros groups/flags. (e) `static_assert(sizeof(TileOp) == 256)`.
- **Worker:** Opus 4.7 designs face-strip UF + dense-remap API + canonical port sort spec (1 dispatch, the subtle bit). Codex gpt-5.4 xhigh implements (2 dispatches sequential, encode then tests). Opus 4.7 audits the encode output against blueprint §5 after codex completes (1 dispatch).
- **Brief template (Opus design):** "Spec the TileOp pipeline for tiles-maxxing/cpp-campaign-v2. Reference blueprint §5 and §6.2–6.3. Deliverables: (1) pseudocode for dense-remap (raw_root → dense_label in [0, max_label)); (2) face-strip UF algorithm over face-strip prime subset; (3) canonical port sort: lex `(h, p⊥)` primary, `(p⊥, h)` tiebreak; (4) 256 B packed encode with exact offsets from blueprint §5.2; (5) OVERFLOW emit when `sum(n) > 192` or `max_label > 128`. Output: 800-word design note."
- **Brief template (Codex impl):** "Implement TileOp encode/tests per Opus design. Forbid bit 7 of L/R group bytes being used for any payload. Blueprint v3 uses fixed-offset `n[4]`, `face_groups[192]`, `inner_flags`, `outer_flags`, `tile_flags` — no bit-stealing. Enforce with `static_assert(offsetof(TileOp, face_groups) == N)` at each field."
- **Brief template (Opus audit):** "Audit `tiles-maxxing/cpp-campaign-v2/src/tileop.cpp` and `test_tileop.cpp` at commit `<sha>` against blueprint §5. Look for: wrong face order, non-deterministic sort, off-by-one in face-strip extraction, missing OVERFLOW path. Report blockers vs nits separately. ≤400 words."
- **Estimate:** 3 wall-clock days (design 3h, impl 12h, audit 2h, fixes 4h).
- **Deps:** M3.

### M5 — Compositor + snapshot + driver + golden (Day 10–13)
- **Goal:** First end-to-end campaign run on the hand-golden 5-tile region; produces snapshot; `compare_snapshots` confirms byte match against committed golden.
- **Components:** C9, C10, C11, C12.
- **Acceptance gate:** (a) `./campaign_main --k-sq=36 --r-inner=80000000 --r-outer=80008192 --region goldens/5tile-spec.json --out /tmp/run.snapshot.bin`. (b) `./compare_snapshots goldens/5tile-k36-inner.snapshot.bin /tmp/run.snapshot.bin` exits 0. (c) Full-octant dry-run at K=36, tiny radii (R_inner=10000, R_outer=10032) completes and produces a verdict. (d) Positional stitching test: 2-tile region, mismatched port counts → panic in debug build.
- **Worker:** Codex gpt-5.4 xhigh implements compositor + snapshot + driver + compare tool (2 dispatches). Opus 4.7 hand-traces the 5-tile golden (1 dispatch — Opus produces the expected snapshot byte stream manually from math, committed as golden).
- **Brief template (Opus golden):** "Hand-trace 5 tiles for the golden. Region: column i=0..1, rows j=0..2 (5 active tiles), at R_inner=80M, R_outer=80008192, K=36, offset (1,1). For each tile: list expected Gaussian primes (via gmpy2 enumeration reference), local DSU components, face-strip ports with ordinals, geo flags per group. Produce expected TileOp bytes for each of 5 tiles. Hash the concatenated bytes; commit to `goldens/5tile-k36-inner.snapshot.bin` + `manifest.json`. Deliver: the binary + JSON + a 300-word README explaining what was traced and why these tiles. Codex re-trace MUST derive expected bytes from the math doc + blueprint alone and MUST NOT read, receive, or reference this trace output before independently deriving its own."
- **Brief template (Codex re-trace):** "Independently re-trace the 5-tile golden from the math doc + blueprint only. MUST NOT read, receive, or reference Opus's trace output before deriving expected bytes. Deliver only your independently derived bytes/hash and discrepancy notes."
- **Brief template (Codex impl):** "Implement compositor (`src/compositor.cpp`), snapshot (`src/snapshot.cpp`), driver (`apps/campaign_main.cpp`), and compare tool (`apps/compare_snapshots.cpp`) per blueprint §7–§8 and plan §3. Region spec: JSON with tile-index box or `"full_octant": true`. Snapshot format: `[header: magic, version, grid_params_hash, constants_hash, mr_witness_set_sha256, tile_count, bytes_per_tile=256][tile_payload: tile_count * 256B]` + `manifest.json` sibling. `campaign_constants_hash` is canonical logical-field serialization — format `K=<K>;R_inner=<R_i>;R_outer=<R_o>;offset=<ox>,<oy>;collar=<C>` as UTF-8 bytes — then SHA-256 of that string. Stable across compiler/toolchain changes. Struct-byte hash is fragile against padding, alignment, and field-reorder; logical string is byte-stable across any platform. Snapshot header includes `mr_witness_set_sha256` in addition to `grid_params_hash` and `constants_hash`. CUDA port parity is gated on matching all three. Must pass acceptance gates in plan §4 M5."
- **Estimate:** 3 wall-clock days.
- **Deps:** M4.

### M6 — Scaling + polish (Day 14–16)
- **Goal:** Run the full octant at K=36 on Mac Mini 12-core; verify verdict is produced; establish performance baseline for CUDA to beat.
- **Acceptance gate:** (a) Full-octant run at project parameters completes within 4 hours wall-clock on Mac Mini (conservative; single-threaded is acceptable for reference). (b) Snapshot hashes deterministic across 3 runs. (c) Determinism: `test_golden_5tile` and the full-octant snapshot produce bit-identical hashes across 3 repeated runs AND bit-identical between `OMP_NUM_THREADS=1` and `OMP_NUM_THREADS=12` for the 5-tile golden.
- **Worker:** Codex gpt-5.4 xhigh for perf polish. Opus 4.7 final audit.
- **Estimate:** 3 wall-clock days.
- **Deps:** M5.

**Total wall-clock:** ~16 working days (~3 weeks with slack). Budget 4 weeks.

## 5. Testing strategy

- **Unit tests per component (§3 table).** Each `test_*.cpp` pinned to its header; `ctest -j` runs in <30s.
- **Hand-golden 5-tile regression.** Tiles chosen: bottom of inner band at canonical octant, `(i, j) ∈ {(0,312500), (0,312501), (0,312502), (1,312500), (1,312501)}` — mix of inner-flagged primes, adjacent face-stitching (I/O between 0,312500 and 0,312501), between-tower stitching (L/R between 0,312500 and 1,312500). Verified fields: `n[4]`, `face_groups[sum(n)]`, `inner_flags`, `outer_flags`, `tile_flags`. Golden bytes committed; Opus traces them by hand (worker brief in M5). Rebuilding the golden requires human sign-off — it is the anchor.
- **Integration test: `compare_snapshots`.** This is THE validation primitive. It runs at every milestone gate; final CUDA port simply adds `./compare_snapshots cpp_snapshot.bin cuda_snapshot.bin`.
- **Sub-region coverage tests.** 1-tile, 3-tile, 5-tile (golden), 50-tile, full-column, full-octant. Each produces a snapshot; each is round-tripped through compare_snapshots against a self-snapshot (determinism check).
- **No parity against v1 results.** v1 logic differs from v2 by design — a v1 mismatch would mislead investigation, a v1 match would falsely reassure. The 5-tile hand-golden is the sole correctness anchor.
- **Determinism-by-repetition:** for every component, add a test that runs N≥3 times on the same input and asserts bit-identical output. Pattern reference: `tile-cpp/tests/test_sieve.cpp:56-84`.

## 6. Worker dispatch protocol

**Dispatch decision tree:**
- **Opus 4.7** when: designing an API that touches canonical math (face-strip UF, dense-remap, port sort); auditing encode outputs; hand-tracing golden; writing the region spec grammar; resolving ambiguities flagged in §10.
- **Codex gpt-5.4 xhigh** when: scaffolding; writing implementation code from a clear spec; writing unit tests; adding CMake; writing CI workflow; performance tuning.
- **Codex gpt-5.4 xhigh auditor profile** when: adversarial review of completed implementation milestones (M4 post-impl, M6 pre-ship).

**Every brief must contain:**
1. Target file paths (absolute).
2. Acceptance gate as specific pass/fail commands.
3. The blueprint/math-doc section numbers that bound correctness.
4. Explicit non-goals (e.g., "do not add SIMD intrinsics", "do not touch CUDA").
5. Output contract: inline summary ≤ 300 words; file artifacts directly under `tiles-maxxing/cpp-campaign-v2/` (not `_workbench/` — these are production code).

**Acceptance checks Jenkins runs before handoff:**
1. `cmake --build build --target all && ctest` — green.
2. `git diff --stat` — no unexpected file creation outside `tiles-maxxing/cpp-campaign-v2/`.
3. Blueprint §2/§5/§6 grep for magic numbers: `S=256`, `C=floor_isqrt`, `TILEOP_SIZE=256`, `ceil_isqrt(K)` in pre-filter. All present.
4. Spot-check against the milestone's acceptance gate.
5. On M4/M5 completion, dispatch Opus auditor before merging.

### 6.3 Forbidden patterns inherited from v1
1. No `std::sqrt` / `sqrtf` / `sqrtl` in production paths. Test generators only.
2. No bit-stealing inside group/port bytes. Fields are per blueprint §5.2 fixed offsets.
3. No consecutive-pair port clustering. Port identity is positional-ordinal per blueprint §5.4.
4. No empty-TileOp substitution for overflow. Emit OVERFLOW_BIT and conservative-SPANNING bias.
5. No derived `r_cnt` or zero-padded implicit counts. `n[4]` is authoritative.
6. No shared decode path across I/O and L/R faces. Type-tag the decode.
7. No caller-zeroed buffer contracts. Zero internally or refuse to run.
8. No Python reference re-implementing face extraction or clustering. Python is sieve-enumeration-only (gmpy2).
9. No O(N²) verdict queries. Spanning is a cached bit on the DSU root.
10. Pre-filter uses `ceil_isqrt(K)`, never `floor_isqrt(K)`. (Recurring boundary bug, commit `46d73db`.)

## 7. Risk register

| Risk | Likelihood | Blast radius | Mitigation |
|---|---|---|---|
| R1. Dense-remap off-by-one → silent label mis-aggregation | med | Unsound (false MOAT possible) | Opus audits M4; unit test enumerates raw→dense mapping for ≥3 DSU topologies; golden regression catches end-to-end. |
| R2. Canonical port sort non-deterministic on tied `(h, p⊥)` | med | Snapshot non-reproducibility → CUDA parity impossible | Blueprint §5.4 + BACKLOG B9 mandate `(p⊥, h)` secondary tiebreak; unit test constructs artificial tie; CI re-runs golden 3× and compares hashes. |
| R3. Axis-prime sieve misses `(0, q)` or double-counts at offset (1,1) boundary | low | Incomplete DSU at column 0 → unsound | Explicit unit test forces `(0, q)` primes into a tile; Opus design spec pins axis-prime inclusion rule. |
| R4. Sieve parity with gmpy2 reference drifts due to MR witness choice | low | False negatives in sieve → missing primes → unsound | Fix MR witnesses to match blueprint FJ64 spec; cross-check all primes under norm 10⁸ once at CI. |
| R5. `compare_snapshots` passes on accidental padding bytes | low | False-green CUDA parity | Zero out `reserved[27]` at encode; snapshot header includes SHA-256 of payload; compare tool diffs header first. |
| R6. M4 estimate (3 days) is optimistic — it's the hardest milestone | high | Slips M5 + M6 by a week | Dispatch Opus design + Opus audit bracketing codex impl — reduces rework cycles. Build the golden first, not last. |
| R7. Opus-traced golden itself contains a math error | med | Wrong anchor → everything downstream confirms-to-wrong | Codex gpt-5.4 xhigh independently re-traces the 5 tiles from math doc + blueprint only — Codex MUST NOT read Opus's trace output before deriving its own. Discrepancies escalate to Jenkins/user as the arbiter and block merge until reconciled. This is the only 2-engine cross-check in the plan. |
| R8. Mac Mini full-octant run hits memory wall at ~1M tiles. | low | M6 needs rework | Stream tileops to disk instead of holding in RAM; snapshot format already is a stream. 256B × 8M tiles = 2 GB, fits comfortably. Per-tile state is O(halo_primes), bounded by halo size not thread count; threading does not multiply memory footprint beyond `num_threads × per-tile buffer`. |
| R9. Scope creep: "just add CUDA while we're here" | high | Miss the point of C++-first | Hard line: this plan does not mention CUDA code. CUDA port is a separate plan post-M6. |
| R10. MR witness set drifts between v2 and its future CUDA port (v1 had CPU 12-base vs CUDA FJ64 mismatch) | med | Parity spot-checks fail; debugging nightmare | Pin witness set in `campaign_constants.h`; CUDA port brief includes "MUST MATCH `kMillerRabinWitnesses[]`" as hard constraint. |

## 8. Pre-mortem

**"This plan has failed by week 4. What's the most likely reason?"**

Top-3 failure modes:

**FM1. M4 TileOp dragged 2 weeks instead of 3 days.** Dense-remap, face-strip UF, and canonical port sort each look simple and are each a subtle correctness trap. Codex xhigh writes something that compiles and passes trivial tests but is wrong on the golden, and the golden itself isn't ready yet (because we're building M5 in parallel) so we don't notice until week 3. **Defense baked in:** M5 golden is authored by Opus *before* Codex finishes M4 impl — the 5-tile expected bytes exist as the correctness anchor the moment Codex's encoder compiles. Also: M4 brief explicitly requires Opus audit before M5 starts. If this defense isn't holding, surface risk at M4 day 2.

**FM2. Offset-(1,1) edge cases break things in ways nobody tested.** The axis-tower special handling (option B) is a v2 addition not present in v1 `tile-cpp`. If the sieve under-enumerates at `x=0` or if the grid builder puts a tile at `(1, j)` that should cover `x ∈ [1, 257]` but some off-by-one has it covering `[0, 256]`, we get a DSU that's structurally wrong and the golden (which also reflects the bug) doesn't catch it. **Defense:** M2 acceptance gate (c) explicitly tests `(0, q)` axis primes at a contrived `(a_lo=0)` tile. Additionally, I add to M3: a cross-check where `coverage_verifier.py` is run on the C++-built Grid's enumeration and must match tile-count to within 0. If FM2 triggers, this gate catches it.

**FM3. The "sub-region = campaign" idea is great until the compositor is written, at which point we realize the compositor expected a column-major full-grid ingestion and sub-regions need partial columns.** We thrash re-architecting `ingest_column` to accept discontinuous j-ranges. **Defense:** Add to M1 scaffolding the `Region` type with explicit `column_slice(i) -> j_range` accessor; compositor M5 is already built against this API. Region spec for the 5-tile golden tests the partial-column path from day one. **If this isn't sufficient**, fallback: compositor accepts "subset" mode where it processes whatever tiles are present and emits a snapshot without demanding full-grid completeness. Verdict output is marked `partial` in manifest.

## 9. Scaffolding Day 0 punch-list

One Codex gpt-5.4 xhigh dispatch, one commit.

```
1. mkdir -p tiles-maxxing/cpp-campaign-v2/{include/campaign,src,apps,tests,goldens,scripts,.github/workflows}
1.5 Add `#pragma GCC poison sqrt sqrtf sqrtl` to `include/campaign/sieve.h` top (only active under `#ifdef CAMPAIGN_STRICT`).
2. Create CMakeLists.txt:
   - cmake_minimum_required 3.25
   - project(cpp_campaign_v2 CXX)
   - set(CMAKE_CXX_STANDARD 20)
   - cache var K_SQ required
   - FetchContent GoogleTest
   - add_library(campaign STATIC src/*.cpp) — stub
   - add_executable(campaign_main apps/campaign_main.cpp) linking campaign
   - add_executable(compare_snapshots apps/compare_snapshots.cpp) linking campaign
   - enable_testing() + add_subdirectory(tests)
   - custom_target bz_check running scripts/bz_check.py pre-build
3. Create stub headers (empty structs, no logic) for all 10 include/campaign/*.h.
4. Create stub .cpp files (empty function definitions matching headers).
5. apps/campaign_main.cpp: parses --help, --k-sq, --r-inner, --r-outer, --region, --out; prints and exits 0.
6. apps/compare_snapshots.cpp: takes two paths; opens, prints size comparison, exits 0.
7. tests/CMakeLists.txt: add_executable per test file, gtest_discover_tests.
8. tests/test_*.cpp: one empty TEST(component, placeholder) per component that just asserts 1==1.
9. .github/workflows/ci.yml: ubuntu-latest + macos-latest, matrix K_SQ in [36, 40], runs cmake + build + ctest.
10. scripts/bz_check.py: copy from /build/bz_check.py — already exists.
11. README.md: 40 lines max. What, why, build-and-run in 3 commands.
12. Commit: "feat(cpp-campaign-v2): M1 scaffolding — empty compiling skeleton, CI green, 12 stub components"
```

Acceptance: CI passes on the commit.

## 10. Open questions

| # | Question | Conservative default | Why Jenkins might revisit |
|---|---|---|---|
| Q1 | Should the v2 tree live inside the tile-compute umbrella or at the repo root? | **Q1 resolved 2026-04-20: Nested under `tiles-maxxing/` — sibling to `campaign-sqrt-36/` etc. Signals v2 is the next campaign in the umbrella, not a detached reference tree.** | Closed. |
| Q2 | Multi-threading inside the reference? | **Q2 resolved 2026-04-20: Per-tile threading enabled.** OpenMP `#pragma omp parallel for` over the active-tile list for sieve → UF → geo → TileOp encode. Each tile is a closed computation with no cross-tile state, so parallelism is embarrassing. Compositor stays sequential (cross-tile stitching requires order). Snapshot emission is sorted by tile ID for bit-stable output regardless of thread count. Determinism guaranteed by: (i) tile-level independence, (ii) sorted emission, (iii) the N≥3 repetition test (Amendment 6) that confirms bit-identical output across runs and across `OMP_NUM_THREADS` values. Single-threaded would be ~27 hours for full-octant at project parameters — v1's 1000 tiles/s empirical baseline already assumed 12 cores. | Closed. |
| Q3 | Snapshot compression (zstd the tile payload)? | **No, uncompressed.** Compare tool and determinism easier on raw bytes. 2 GB full-octant snapshot is fine. | If we need to archive many snapshots, revisit. |
| Q4 | Include `manifest.json` campaign_constants_hash as a struct-byte hash or as a human-readable logical fingerprint? | **Canonical logical-field serialization — format `K=<K>;R_inner=<R_i>;R_outer=<R_o>;offset=<ox>,<oy>;collar=<C>` as UTF-8 bytes — then SHA-256 of that string. Stable across compiler/toolchain changes.** Struct-byte hash is fragile against padding, alignment, and field-reorder; logical string is byte-stable across any platform. | Closed. |
| Q5 | Region spec grammar: JSON-based, CLI-flag-based, or both? | **JSON file passed via `--region` flag**; CLI flags supported only for `--full-octant` shortcut. Regions have nested j-range per column; JSON scales better. | If most workflows use the full-octant shortcut, JSON is overkill. Defer if true. |
| Q6 | GoogleTest vs Catch2? | Q6: GoogleTest — CONFIRMED. v1 used home-rolled `assert/expect_*` helpers (`tile-cpp/tests/test_compact_uf.cpp:19-93`), which had zero `gtest_discover_tests` integration and made CI signal opaque. GoogleTest is the upgrade. | Closed. |
| Q7 | `nlohmann/json` for manifest — vendored header or FetchContent? | **FetchContent**, pinned to a release tag. One less committed binary blob. | If CI is slow, vendoring the single header avoids network fetch. |
| Q8 | Where does `bz_check.py` actually live? It already exists at `/build/bz_check.py`. | **Symlink (or copy at CMake time) into `tiles-maxxing/cpp-campaign-v2/scripts/`.** Keep single source of truth. | Workers might prefer a self-contained `tiles-maxxing/cpp-campaign-v2/` without external symlinks; can inline-copy. |

*End of plan. Revisit after M4 retrospective.*
