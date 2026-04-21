FAIL

## Exec Summary

Verdict: FAIL with 0 BLOCKER findings, 4 MAJOR findings, and 2 MINOR findings. I did not find a direct false-MOAT path in the current full-octant driver, but the implementation is not faithful to the math SSoT: the geo boundary test is widened to a `ceil_isqrt(K)` band instead of the canonical norm-form predicate, and the BZ gate is not tied to the radii used by the executable. CUDA-port readiness is conditional: the data contracts are mostly usable, but the CPU reference relies on heap vectors and O(n^2) pair loops that must be redesigned for kernels. The existing tests pass, but they encode the widened geo behavior and the committed 5-tile golden test is a placeholder.

## Findings By Severity

### BLOCKER

None found.

### MAJOR

#### 1. Geo flags implement a widened ceil-isqrt band, not the spec's norm-form predicate

Evidence:
- Spec: `methodology/lemmas_v2/tile-operator-definition-v-claude.md:314-325` defines `geo_I` and `geo_O` by `(norm_sq - R_inner_sq - K)^2 <= 4*R_inner_sq*K` and `(R_outer_sq - norm_sq + K)^2 <= 4*R_outer_sq*K`; `ceil_isqrt(K)` is only named for the prefilter.
- Code: `tiles-maxxing/cpp-campaign-v2/include/campaign/geo_tests.h:7-13` documents `inner: ... (R_inner + ceil_isqrt(K))^2` and `outer: (R_outer - ceil_isqrt(K))^2 ...`.
- Code: `tiles-maxxing/cpp-campaign-v2/src/geo_tests.cpp:40-63` implements those widened integer bands, with no squared epsilon comparison.
- Tests lock the drift in: `tiles-maxxing/cpp-campaign-v2/tests/test_geo_tests.cpp:25-35`, `42-62`, and `83-94` assert ceil-band boundaries rather than the spec predicate.

Why:
For non-square `K=40`, the code marks primes in `(R + sqrt(40), R + 7]` as `geo_I`, and symmetrically widens `geo_O`. At project `R_inner=80,000,000`, the spec upper integer norm is `6400001011928891`, while the code upper is `6400001120000049`; `6400001011928893` is a prime `1 mod 4` in that widened gap, so this is not an empty theoretical interval. This is conservative for false-MOAT soundness, but it breaks the spec's exact Theorem 11 equivalence and can produce false SPANNING outside the overflow semantics.

Suggested fix:
Implement the spec predicate with an optional `abs(epsilon) <= 2*R*ceil_isqrt(K)+1` prefilter followed by i128 `epsilon*epsilon <= 4*R^2*K`. Update `test_geo_tests.cpp` and all goldens to assert the exact norm form.

#### 2. BZ reconciliation is not bound to the runtime radii that produce verdicts

Evidence:
- Spec: `methodology/lemmas_v2/tile-operator-definition-v-claude.md:333-348` says BZ equality is per-deployment and future deployments must rerun `bz_check.py`.
- CMake: `tiles-maxxing/cpp-campaign-v2/CMakeLists.txt:108-109` defaults the BZ radii to `80,000,000` and `80,008,192`.
- Driver: `tiles-maxxing/cpp-campaign-v2/apps/campaign_main.cpp:346-383` accepts arbitrary `--r-inner` and `--r-outer`, then builds constants/grid for those runtime values.
- CMake: `tiles-maxxing/cpp-campaign-v2/CMakeLists.txt:128-142` skips the BZ check with only a warning if neither `uv` nor Python3 is found.

Why:
A binary can be built after checking the default project radii, then run with different radii that were never BZ-reconciled. That violates the spec's per-deployment condition. The optional skip also contradicts the "build must fail if BZ interval is non-empty" gate because in a no-Python environment no interval is checked at all.

Suggested fix:
Either make campaign radii build-time constants tied to the BZ target, or run/require BZ reconciliation for the exact radii before any verdict-producing execution. Fail configure/build if no BZ runner is available. Include the checked BZ parameter tuple in the constants hash or manifest.

#### 3. Grid enumeration omits negative-i active tiles required by Lemma 6 under offset `(1,1)`

Evidence:
- Spec: `methodology/lemmas_v2/tile-operator-definition-v-claude.md:577-592` proves every `p in V(G_full)` lies in the proper region of an active tile using `i = floor((a - o_x)/S)`.
- With `o_x=1`, a point on the axis `a=0` maps to `i=-1`.
- Code: `tiles-maxxing/cpp-campaign-v2/src/grid.cpp:189-220` starts `find_i_min` at `i=0`; `find_i_max` also scans down only to `0` at `grid.cpp:223-247`.
- Tests mirror the same restriction: `tiles-maxxing/cpp-campaign-v2/tests/test_grid.cpp:55-68` exhaustive reference only checks `i >= 0`.

Why:
The implementation excludes proper-owner tiles like `T_{-1,j}` even though they can contain `x=0` axis primes in `R`. The sieve tries to recover axis primes through the `i=0` halo (`tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:13-23`, `94-113`), so this may be harmless for current parameters, but it is no longer the active-tile collection used by Lemma 6, Lemma 7, and Theorem 11's host-tile choice.

Suggested fix:
Enumerate from `floor((0 - OFFSET_X)/S)` when `OFFSET_X > 0`, or formally replace the Lemma 6 dependency with a proved and tested "axis-in-halo" special case. Add an exhaustive grid test that includes the negative-i column for offset `(1,1)`.

#### 4. Golden verification is effectively absent and the reference generator encodes the geo drift

Evidence:
- Test: `tiles-maxxing/cpp-campaign-v2/tests/test_golden_5tile.cpp:1-10` is a placeholder asserting `1 == 1`.
- Reference generator: `tiles-maxxing/cpp-campaign-v2/goldens/5tile-k36.reference.py:105-109` defines geo bands using `(R_inner + ceil_sqrt_k)^2` and `(R_outer - ceil_sqrt_k)^2`.
- Reference generator: `goldens/5tile-k36.reference.py:182-192` emits flags from that widened interval form.

Why:
The committed golden cannot catch C++ drift because no byte-for-byte assertion runs. Worse, the independent reference is not spec-faithful on the same geo predicate as finding 1. The test suite passing therefore does not establish spec conformance for the boundary flags.

Suggested fix:
Replace the placeholder with a test that regenerates or reads the committed snapshot and compares C++ output byte-for-byte. Update the Python generator to use the same i128 norm-form predicate as the spec.

### MINOR

#### 5. Release invariant verification is weaker than the comments and spec-facing contract imply

Evidence:
- Header: `tiles-maxxing/cpp-campaign-v2/include/campaign/grid.h:156-166` presents `verify_invariants()` as the always-on I1/I2/I4 seatbelt.
- Code: `tiles-maxxing/cpp-campaign-v2/src/grid.cpp:303-320` checks only that stored column ranges are well formed; it does not re-run `is_active_tile` to prove every tile in the interval is active.
- Code: the actual active predicate scan for I1 is debug-only at `grid.cpp:377-399`.

Why:
If the enumeration algorithm has a release-only or parameter-specific bug that inserts a gap or omits an active tile inside a tower, `verify_invariants()` can still return success because it trusts the generated `[j_low,j_high]` table. This is a verification gap, not an observed current failure.

Suggested fix:
For production init, add a bounded active-predicate audit over tower interiors for I1, or rename the current check as a shape check and add separate evidence that enumeration was validated for project-scale parameters.

#### 6. CUDA port must not copy the CPU reference hot paths directly

Evidence:
- `tiles-maxxing/cpp-campaign-v2/src/process_tile.cpp:18-31` allocates per-tile `std::vector`s.
- `tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:159-168` builds local UF with all-pairs prime comparisons.
- `tileop.cpp:92-149` creates per-face vectors and runs another all-pairs face-strip UF.

Why:
This is acceptable for a C++ reference, but it is not CUDA-ready as written: heap allocation, STL sorting/vectors, and all-pairs loops will not map cleanly to fixed shared-memory kernels or predictable occupancy.

Suggested fix:
For CUDA, use fixed-capacity buffers, spatial bucketing by lattice offset, bounded neighbor stencils from `K`, and deterministic block-local reductions for face ports and group flags.

## Spec-To-Code Mapping Table

| Load-bearing invariant | Spec citation | Code checked | Status |
|---|---:|---|---|
| Closed tile proper `[t,t+S]`, 257 points | `tile-operator-definition-v-claude.md:105-117` | `grid.cpp:45-60`, `154-169`; `sieve.cpp:88-96` | PASS |
| Collar `C=floor(sqrt(K))` | `tile-operator-definition-v-claude.md:109-117` | `constants.h:83-110` | PASS |
| Face strips within perpendicular distance `C` | `tile-operator-definition-v-claude.md:126-135` | `tileop.cpp:72-90` | PASS |
| Port UF is face-strip induced graph and sorted by representative `(h,p_perp)` | `tile-operator-definition-v-claude.md:138-153` | `tileop.cpp:92-149` | PASS |
| TileOp fixed 256 B and per-group flags | `tile-operator-definition-v-claude.md:686-704` plus TileOp layout from project contract | `tileop.h:51-69`; `tileop.cpp:242-251` | PASS |
| Geo flags use exact norm-form integer test | `tile-operator-definition-v-claude.md:314-325`, `695-698` | `geo_tests.cpp:34-64` | FAIL |
| BZ reconciliation per deployment | `tile-operator-definition-v-claude.md:333-348` | `CMakeLists.txt:104-142`; `campaign_main.cpp:346-383` | FAIL |
| Active-tile coverage for every prime | `tile-operator-definition-v-claude.md:577-592` | `grid.cpp:189-247`; `sieve.cpp:94-113` | DIVERGES |
| I1/I2/I4 grid invariants | `tile-operator-definition-v-claude.md:531-533`, `595-601` | `grid.cpp:303-405`; `campaign_main.cpp:389-399` | PARTIAL |
| Strict positional stitching, count mismatch rejected | `tile-operator-definition-v-claude.md:264-267`, `612-615` | `compositor.cpp:34-44`, `183-216` | PASS |
| Overflow is conservative SPANNING | project overflow semantics | `tileop.cpp:151-155`, `238-240`, `268-270`; `compositor.cpp:148-153` | PASS |
| Side-exposed L/R not stitched; reflection closure handles full annulus | `tile-operator-definition-v-claude.md:819-858` | `compositor.cpp:46-61`, `277-305` | PASS |
| Annulus thickness and adjacent-octant preconditions | `tile-operator-definition-v-claude.md:352-357`, `786-794` | `campaign_constants.cpp:91-123`, `campaign_main.cpp:85-99`, `359-369` | PASS for `campaign_main`; library default is relaxed |
| Golden/reference coverage | audit scope goldens | `test_golden_5tile.cpp:1-10`; `goldens/5tile-k36.reference.py:105-109` | FAIL |

## What I Checked

- Read the full math SSoT: `methodology/lemmas_v2/tile-operator-definition-v-claude.md`.
- Inspected contracts and implementation under `tiles-maxxing/cpp-campaign-v2/include/campaign/*.h`, `src/*.cpp`, `apps/campaign_main.cpp`, `goldens/`, and tests.
- Did not read `tiles-maxxing/cpp-campaign-v2/docs/final-math-audit.md`.
- Ran `ctest --test-dir build --output-on-failure` from `tiles-maxxing/cpp-campaign-v2`; result: 90/90 tests passed, with `GeoTests.NonSquareKUsesCeilBoundary` skipped in this K=36 build.
