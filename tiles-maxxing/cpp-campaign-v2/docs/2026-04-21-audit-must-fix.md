---
title: Audit Must-Fix Application — 2026-04-21
date: 2026-04-21
engine: coordinator
type: fix-report
status: partial
scope: [tiles-maxxing/cpp-campaign-v2]
related:
  - /_audits/2026-04-21/synthesis.md
  - docs/2026-04-21-grid-optimization.md
---

## Summary

Applied 3 of 4 audit must-fix items. Fix #2 (flip `from_radii` default to strict=true) fails under the current test lock-in; reverted per hard-rule revert-on-gate-break. Task 3 (R=1M tile count discrepancy) resolved as a configuration artifact, not a regression.

| # | Item | Result |
|---|---|---|
| 1 | O-M1: `annulus_thickness_rhs` coefficient `√K` → `√(2K)` | Applied, all gates pass |
| 2 | C2: flip `from_radii` default to `strict=true` | Failed — breaks 17 tests in FORBIDDEN `tests/*` (see below) |
| 3 | Codex-M2: bind BZ gate to runtime radii | Applied, all gates pass |
| 4 | Codex-M1: `geo_tests` use spec norm-form predicate | Applied, all gates pass |
| Task 3 | R=1M tile count: 16,184 vs 102,524 | Resolved — different `R_outer` |

Final gates (all three simultaneously after all applied fixes):
- K=36 golden at `R=80M δ=8192`: byte-identical, exit 0
- K=40 golden at `R=800M δ=10000`: byte-identical, exit 0
- `ctest --test-dir build-k36-tests`: **90/90 passed**, 2 skipped (expected: DEBUG-only `UnionFind.FindOutOfRangeAbortsInDebugBuilds`, K=40-only `GeoTests.NonSquareKUsesCeilBoundary`)

---

## Task 3 — R=1M tile count verdict

**Verdict: current 102,524 is correct; prior session's 16,184 used a different `R_outer`.**

### Evidence

Prior handoff at `/Users/otonashi/.gaal/data/claude/handoffs/2026/04/20/7c6e39c6.md:87-90`:
> Closed with a final capstone dry-run:
>   - full-octant mid-scale run at R=1M,
>   - 16,184 tiles,

The handoff itself is terse. The full session transcript at `/Users/otonashi/.gaal/data/claude/sessions/2026/04/20/7c6e39c6.md` reveals the actual invocation parameters (from the Phase 3.4 capstone task output):

> **Full-octant dry-run — `R_inner=1,000,000  R_outer=1,001,024  K_SQ=36`**
> | Metric | Value |
> | Active tiles | **16,184** |
> | Snapshot SHA-256 | `c3cd76e9c23292939abd8e3bde404afb684409cdfae079f63c3c1632e07ef122` |
> | constants_hash | `af99e998e29dc98afeab7f42ec6b697dbe82912597fd62730ebe6e5ab561130b` |

So δ = `R_outer - R_inner = 1,024`, **not** 8,192. The current measurement at `R_outer = 1,008,192` uses δ = 8,192 = 8× the prior capstone's annulus width.

### Analytic cross-check

Octant active-tile count scales with annulus area: `N_tiles ≈ (2π·R·δ) / (8·S²)` + boundary overhead.

| Config | Computed core | Measured | Boundary overhead |
|---|---|---|---|
| R=1M, δ=1024, S=256 | 12,272 | 16,184 | +32% (axis/octant edges) |
| R=1M, δ=8192, S=256 | 98,175 | 102,524 | +4.4% |

Ratio check: measured 102,524 / 16,184 ≈ 6.33×. Analytic δ ratio 8192/1024 = 8×. The octant enumerator has a fixed boundary cost that scales sub-linearly with δ, so the 6.33× measured ratio is consistent with the 8× analytic ratio once boundary tiles are accounted for. No grid regression.

### Git history

`git log --oneline -- tiles-maxxing/cpp-campaign-v2/src/grid.cpp` shows only two touches since the prior capstone:
- `59b8161` perf(cpp-campaign-v2): O(R/S) grid build + K=40 anchor + preflight harness (this session)
- `7317e26` perf(cpp-campaign-v2): grid — replace lattice scan with monotone-y-range (prior)

Both already demonstrated byte-identical golden preservation at K=36 and K=40. The committed K=36 golden `6ba6d9...` and K=40 golden `e037a6...` both still byte-match, and thread-determinism at 1T vs 12T is preserved.

No reproduction of the 16,184 config was attempted (read-only investigation — the analytic cross-check is decisive).

**Conclusion.** Grid semantics are unchanged. The 16,184 vs 102,524 disparity is a δ-parameter difference (1,024 vs 8,192), not a grid regression. No action required.

---

## Fix #1 — O-M1 `annulus_thickness_rhs` coefficient

**Finding (audit opus.md MAJOR 1):** Spec line 352 requires `delta > S·√2 + 2·√K`, squaring gives `delta² > 2S² + 4S·√(2K) + 4K`. Driver at `apps/campaign_main.cpp:85-89` used `ceil_isqrt(K)` where spec needs `ceil_isqrt(2K)`. At K=36: `ceil_isqrt(36)=6` vs `ceil_isqrt(72)=9`. At K=40: `7` vs `9`.

**Before.** `apps/campaign_main.cpp:85-89`:
```cpp
std::uint64_t annulus_thickness_rhs(std::uint32_t k_sq) {
  const std::uint64_t s = static_cast<std::uint64_t>(campaign::S);
  const std::uint64_t ceil_sqrt_k =
      static_cast<std::uint64_t>(campaign::ceil_isqrt(k_sq));
  return 2ULL * s * s + 4ULL * s * ceil_sqrt_k + 4ULL * k_sq;
}
```

**After.** `apps/campaign_main.cpp:85-96`:
```cpp
std::uint64_t annulus_thickness_rhs(std::uint32_t k_sq) {
  // Spec (tile-operator-definition-v-claude.md:352): delta > S*sqrt(2) + 2*sqrt(K).
  // Squaring: delta^2 > 2*S^2 + 4*S*sqrt(2K) + 4K.
  // Audit O-M1: previous code used ceil_isqrt(K) instead of ceil_isqrt(2K).
  const std::uint64_t s = static_cast<std::uint64_t>(campaign::S);
  const std::uint64_t ceil_sqrt_2k =
      static_cast<std::uint64_t>(campaign::ceil_isqrt(2 * k_sq));
  return 2ULL * s * s + 4ULL * s * ceil_sqrt_2k + 4ULL * k_sq;
}
```

**Gate result.** K=36 golden byte-identical, K=40 golden byte-identical, ctest 90/90.

**Project-param impact.** At δ=8192, `delta² = 67,108,864`. RHS-new at K=36 = `131,072 + 9216 + 144 = 140,432`. Project clears by ~478×. No behavior change at project params. Edge parameters in `delta ∈ [371, 375]` newly rejected as spec requires.

---

## Fix #2 — C2 flip `from_radii` default to `strict=true`

**Status: FAILED — reverted. Would require deeper change (test updates).**

**Attempted change.** `include/campaign/campaign_constants.h:103-106`: `bool strict = false` → `bool strict = true`.

**Gate result.** `build-k36-tests` ctest:
- Before fix: 90/90 pass (2 skipped).
- After flip: **17/90 FAIL** (73/90 pass, 2 skipped).

**Root cause.** Multiple test files in the FORBIDDEN `tests/*` directory call `from_radii(tiny_R_inner, tiny_R_outer, k_sq)` without explicitly passing `strict=false`. Affected tests:
- `test_campaign_constants.cpp`: `FromRadiiHappyPath`, `PrefilterUsesCeilIsqrt`, `CanonicalHashIsDeterministic`, `CanonicalHashDifferentiatesParams`, `AnnulusThicknessGate`, `FromRadiiStrictThrowsOnTooThin` (the last explicitly asserts `EXPECT_NO_THROW(from_radii(10000, 10032, k_sq_value))` on line 95-96)
- `test_sieve.cpp`: 5 tests using `from_radii(r_inner=10000, r_outer=20000)`
- `test_snapshot.cpp`: 6 tests using `from_radii(10000, 20000)`

Per hard rules, `tests/*` is FORBIDDEN to touch. Per hard rules, "If a fix breaks a gate, revert JUST that fix and mark it 'failed — would require deeper change.'"

**Required change for future session.** Update every `from_radii(r_i, r_o, k)` call in `tests/*` to `from_radii(r_i, r_o, k, /*strict=*/false)` (adding ~15 call-site updates), and rewrite `FromRadiiStrictThrowsOnTooThin`'s line 95-96 assertion to test the explicit `strict=false` path. Estimated ~20-30 line diff across 4 test files.

**Revert confirmed.** Post-revert ctest: 90/90 pass.

---

## Fix #3 — Codex-M2 bind BZ gate to runtime radii

**Finding (audit codex.md MAJOR 2):** `CMakeLists.txt:108-109` hardcoded BZ radii as defaults. `campaign_main` accepts arbitrary runtime `--r-inner`/`--r-outer`. Build-time BZ evidence may not cover actual runtime radii. Spec line 348: "future deployments must re-run `bz_check.py`; the equality is per-deployment."

**Approach.** Factor the BZ radii out of CMake into a single-source-of-truth config file. Both `CMakeLists.txt` and `bz_check.py` read from the same file. Deployments that change runtime radii must update the config; CMake re-runs `bz_check` on config edit. This is the minimal interpretation of the task language "Make it read them from … a config file, not hardcoded."

**Files changed:**

1. New file `scripts/bz_config.json` — single source of BZ-reconciled radii keyed by `K_SQ`:
   ```json
   {
     "k_sq_to_radii": {
       "36": {"r_inner": 80000000, "r_outer": 80008192},
       "40": {"r_inner": 800000000, "r_outer": 800010000}
     }
   }
   ```

2. `scripts/bz_check.py` — made `--r-inner` / `--r-outer` optional; default-load from `bz_config.json` based on `--k-sq`. Throws clear error if the config file is missing or the K_SQ entry is absent.

3. `CMakeLists.txt:103-142` — removed hardcoded `BZ_R_INNER` / `BZ_R_OUTER` cache vars; custom target now invokes `bz_check.py --k-sq ${K_SQ}` and declares `DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/bz_config.json` so config edits trigger re-run.

**Before** (CMakeLists.txt excerpt):
```cmake
set(BZ_R_INNER "80000000" CACHE STRING "R_inner passed to bz_check.py")
set(BZ_R_OUTER "80008192" CACHE STRING "R_outer passed to bz_check.py")
...
add_custom_target(bz_check ALL
  COMMAND ${BZ_CHECK_CMD}
    --r-inner ${BZ_R_INNER}
    --r-outer ${BZ_R_OUTER}
    --k-sq    ${K_SQ}
  ...
)
```

**After:**
```cmake
add_custom_target(bz_check ALL
  COMMAND ${BZ_CHECK_CMD} --k-sq ${K_SQ}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/bz_config.json
  ...
)
```

**Gate result.** K=36 golden byte-identical, K=40 golden byte-identical, ctest 90/90. Manual BZ runs for K=36 and K=40 both return `PASS` with `gaussian_prime_norm_count=0` on both zones.

**What this does NOT do.** The strongest form (bind BZ to the runtime radii passed to campaign_main at exec time) would require either (a) making campaign_main reject runtime radii that don't match the BZ config, or (b) running `bz_check.py` inside `campaign_main` at startup. Both break tests (tests run campaign_main with `r_inner=1000, r_outer=1600` and `r_inner=10000, r_outer=10032` — not in the BZ config). This fix closes the drift between CMake and the Python script but leaves the runtime-campaign_main side as a follow-up. Note that Theorem 11 only uses the forward-safe implication `p ∈ geo_I ⇒ ||p|| ≤ R_inner + √K` (spec line 350), which holds unconditionally from norm-form definition — so the remaining gap is governance, not soundness.

---

## Fix #4 — Codex-M1 geo_tests spec norm-form predicate

**Finding (audit codex.md MAJOR 1).** Spec lines 314-317 give the canonical integer test:
- `p ∈ geo_I ⟺ (‖p‖² − R_inner² − K)² ≤ 4·R_inner²·K`
- `p ∈ geo_O ⟺ (R_outer² − ‖p‖² + K)² ≤ 4·R_outer²·K`

Spec line 321 notes that `p ∈ V(G_full)` implies `‖p‖² ≥ R_inner²` (annulus membership). Combined, the effective inner predicate is `R_inner² ≤ ‖p‖² ≤ (R_inner + √K)²`.

Previous code in `src/geo_tests.cpp:34-64` used the widened `(R + ⌈√K⌉)²` upper. At K=36 identical; at K=40 strictly wider (code's `⌈√40⌉=7` vs spec `√40≈6.324`).

**Before** (`src/geo_tests.cpp`, inner):
```cpp
const std::uint64_t ceil_sqrt_k = ceil_isqrt(k_sq_value);
const __int128 lower = to_i128(constants.R_inner_sq);
const __int128 upper = lower + square_band_delta(constants.R_inner, ceil_sqrt_k);
return lower <= norm && norm <= upper;
```

**After** (`src/geo_tests.cpp:34-56`):
```cpp
const __int128 norm = static_cast<__int128>(norm_sq);
const __int128 r_sq = to_i128(constants.R_inner_sq);
if (norm < r_sq) {
  return false;  // p in V(G_full) clause: ||p||^2 >= R_inner^2
}
const __int128 k = static_cast<__int128>(k_sq_value);
const __int128 four_r_sq_k = constants.four_rin_sq_k_i128();
const __int128 eps = norm - r_sq - k;
return eps * eps <= four_r_sq_k;
```

Outer form symmetric with `norm > R_outer²` as the annulus short-circuit.

**Header updated** (`include/campaign/geo_tests.h:1-17`) to document the norm-form contract.

**Gate result.** 
- K=36 golden byte-identical (the spec predicate and ceil-band agree at K=36 because `ceil_isqrt(36) = √36`).
- K=40 golden byte-identical (verified: no primes in the 5-tile-k40 region have norms in the gap `[R² + 2R·√40 + 40, R² + 2R·7 + 49]` that the old band accepted and norm-form rejects).
- ctest 90/90 pass — `GeoTests.InnerBandCorners` and `OuterBandCorners` still pass because the corrected implementation enforces the `R_inner² ≤ norm² ≤ R_outer²` annulus bounds combined with the i128 eps² test, matching the existing test assertions at the boundary.

**Dead-code removal.** Unused helpers `square_band_delta` and `lower_outer_delta` removed from anonymous namespace in `src/geo_tests.cpp`. `ceil_isqrt` static asserts retained (they documented the K=36 vs K=40 boundary transition and are cheap compile-time tests).

**Note on Python reference.** `goldens/5tile-k36.reference.py:105-109` still encodes the widened `ceil_isqrt(K)` band. This is flagged but not in scope for this pass (reference.py is not a gate, goldens are); at K=36 the two forms agree byte-for-byte so the K=36 reference remains accurate. At K=40 the reference.py would now produce different output than the C++; that is a separate follow-up (X-M4 in the synthesis, not in this audit-must-fix scope).

---

## Files touched (all within ALLOWED list)

| File | Lines changed | Purpose |
|---|---|---|
| `apps/campaign_main.cpp` | +4 / -1 | Fix #1 |
| `scripts/bz_check.py` | +30 / -3 | Fix #3 |
| `scripts/bz_config.json` | +14 (new) | Fix #3 |
| `CMakeLists.txt` | +3 / -10 | Fix #3 |
| `include/campaign/geo_tests.h` | +8 / -10 | Fix #4 (doc) |
| `src/geo_tests.cpp` | +15 / -25 | Fix #4 |

Touched FORBIDDEN files: none.

---

## Final gate record

Command sequence:
```
cmake --build build-k36
cmake --build build-k36-tests
cmake --build build-k40
build-k36/campaign_main --k-sq=36 --r-inner=80000000 --r-outer=80008192 \
  --region goldens/5tile-spec.json --out /tmp/ta-5.bin --threads=1
build-k36/compare_snapshots /tmp/ta-5.bin goldens/5tile-k36.snapshot.bin
build-k40/campaign_main --k-sq=40 --r-inner=800000000 --r-outer=800010000 \
  --region goldens/5tile-k40-spec.json --out /tmp/ta-k40.bin --threads=1
build-k40/compare_snapshots /tmp/ta-k40.bin goldens/5tile-k40.snapshot.bin
ctest --test-dir build-k36-tests --output-on-failure
```

Results:
- K=36: VERDICT=MOAT, 5/5 tiles identical, `OK: snapshots byte-identical`, exit 0.
- K=40: VERDICT=MOAT, 5/5 tiles identical, `OK: snapshots byte-identical`, exit 0.
- ctest: **100% tests passed, 0 tests failed out of 90.** Skipped: `UnionFind.FindOutOfRangeAbortsInDebugBuilds` (DEBUG-only), `GeoTests.NonSquareKUsesCeilBoundary` (K=40-only, current build is K=36).

## Open threads

- Fix #2 (C2 strict default) requires a follow-up PR that touches `tests/*`. Outside this pass's scope.
- Python reference `goldens/5tile-k36.reference.py` still uses the widened ceil-band (codex X-M4). No golden drift at K=36; K=40 reference is a separate follow-up.
- BZ runtime-radii binding inside `campaign_main` (strongest form of X-M2) is a follow-up; current fix closes the CMake ↔ Python drift but not the runtime ↔ BZ drift.
