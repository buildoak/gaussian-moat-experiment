# Synthesis — cpp-campaign-v2 Independent Audits

**Tiebreaker:** Claude Opus 4.7 (sighted, with spec + code access)
**Date:** 2026-04-21
**Inputs:** `opus.md` (Opus 4.7 blinded) + `codex.md` (gpt-5.4 codex blinded)
**Spec:** `methodology/lemmas_v2/tile-operator-definition-v-claude.md`
**Code:** `tiles-maxxing/cpp-campaign-v2/`

---

## Convergence summary

**Findings both auditors flagged (high confidence): 3**

| # | Finding | Opus tag | Codex tag |
|---|---|---|---|
| C1 | Grid enumeration omits `i < 0` under offset `(1,1)`; pipeline recovers via halo + face_L + reflection closure; spec Lemma 6 literal statement diverges | m1 | MAJOR 3 |
| C2 | `CampaignConstants::verify_annulus_thickness()` exists but `from_radii` default-strict=false; driver is the only enforcement path | m2 | MINOR (under MAJOR 1 mapping row) |
| C3 | CUDA port must not copy CPU hot paths directly: heap-per-tile vectors, O(n²) pairwise unions, per-face allocations | n1 / CUDA-gotcha 1,2 | MINOR 6 |

Note: both also land on the same spec-to-code mapping table where `geo_tests` and BZ-binding are the sore thumbs; they just give different severities.

**Fix divergence across shared findings:** None substantive. Both agree on the remediation direction for C1 (either extend enumeration to `i<0` or document the halo-rescue path as the real guarantee); C2 (flip default to `strict=true` at verdict-producing entry points); C3 (fixed-capacity buffers + spatial bucketing for CUDA).

---

## Divergence analysis

### ONLY Opus — verified

#### O-M1 — `annulus_thickness_rhs` uses `4·S·⌈√K⌉` where spec requires `4·S·√(2K)`  **[ACCEPT]**

**Evidence.** Spec line 352: *"R_outer − R_inner > S√2 + 2√K"*. Squared: `delta² > (S√2 + 2√K)² = 2S² + 4S·√(2K) + 4K`. Code at `apps/campaign_main.cpp:85-89` computes `2S² + 4·S·ceil_isqrt(K) + 4K`. For K=36: `⌈√36⌉=6`, but `√(2·36)=√72≈8.485 (⌈⌉=9)`. Driver surrogate is smaller than true spec RHS by `4·S·(√(2K)-√K) ≈ 4·256·2.485 ≈ 2,545`. At project `delta²=67,108,864` vs RHS ≈ 140,432, irrelevant. Narrow false-MOAT window only opens at `delta² ∈ [137360, 140432]`, i.e., delta ∈ [370.6, 374.7]. Project delta=8192 clears by 22×.

**Verdict.** Real bug. Soundness-adjacent but operationally inert at project parameters. **Must fix before any entry point accepts deltas near the annulus-thickness boundary.** Note: `CampaignConstants::verify_annulus_thickness()` uses a different (stronger, correct) surrogate `S·⌈√2⌉ + 2·⌈√K⌉` but is dormant (strict=false default).

**Fix:** Two options, both one-line. Either replace `ceil_isqrt(k_sq)` with `ceil_isqrt(2 * k_sq)` in `annulus_thickness_rhs`, or delete the bespoke driver check and call `constants.verify_annulus_thickness()` (preferred — library-owned, already stronger).

#### O-M2 — `std::exception` thrown from inside `#pragma omp parallel for` is UB  **[ACCEPT — defer]**

**Evidence.** `apps/campaign_main.cpp:419-424`; throw sites in `src/sieve.cpp:47-52,64-70`, `src/tileop.cpp:212`, `src/union_find.cpp:27-35`. At project parameters (R ≤ 1e9, R² < 2^62) these throws are unreachable. Future >2× scale or harness misuse could hit this, and CUDA port needs error-code returns regardless.

**Verdict.** Latent; project-params-safe. **CUDA-port-gate only** — port team must translate throws to error codes anyway.

#### O-m3 — `find_tower` down-extension bounded `j_low > 0`  **[ACCEPT — nice-to-have]**

**Evidence.** `src/grid.cpp:281-282`. Opus' analysis is correct: at project params `j_low ≈ 312,500`; at tiny-radius tests the only lattice point that would be missed (under octant `y ≥ x ≥ 0`, offset `(1,1)`) is `(1,1)` with norm² = 2, never in any realistic annulus. Practically harmless; principally incorrect bound.

**Verdict.** Defer. Harmless at project; fix as part of general "match Lemma 6 literal coverage" pass if C1 is addressed structurally.

#### O-m4 — `is_inner_prime`/`is_outer_prime` take `std::int64_t` norm; silent narrowing at R > 2^31.5  **[ACCEPT — defer]**

**Evidence.** `include/campaign/geo_tests.h:27-33`, cast in `src/process_tile.cpp`. `CampaignConstants::from_radii` rejects `R_outer > 2^32`, so the construction gate catches the danger window before geo_tests can mis-classify. At project params (R_outer ≈ 8e7), far below 2^31.5.

**Verdict.** Latent; project-params-safe, double-fenced by the `from_radii` construction check. Change signatures to `uint64_t` during the general geo_tests cleanup (see CodexM1).

#### O-m5 — `annulus_thickness_rhs` returns `uint64_t` but LHS is `unsigned __int128`  **[DISMISS]**

**Evidence.** `apps/campaign_main.cpp:92-99`. At project params RHS is ~137K — fits in `uint32_t`. Comparison is well-defined. Only an issue at hypothetical S ≥ 2^32.

**Verdict.** False positive at project. Nice-to-have type discipline; no impact.

#### O-n1 / O-n2 / O-n3 — algorithmic micro-hotspots  **[DISMISS as CPU issues]**

Real CUDA-port concerns (already covered by C3). On CPU with project tile prime counts (200-600 per tile), these are fine. Re-raise in CUDA-port backlog.

---

### ONLY Codex — verified

#### X-M1 — Geo flag predicate is a `⌈√K⌉`-wide band, not the canonical norm-form  **[ACCEPT — narrow scope]**

**Evidence.** Spec lines 314-317 give the canonical integer test `(‖p‖² - R²_inner - K)² ≤ 4·R²_inner·K`. Code in `src/geo_tests.cpp:34-64` implements `R²_inner ≤ norm² ≤ (R_inner + ⌈√K⌉)²`.

**Key check — do the two predicates agree at K=36?** Yes, exactly. For K=36, `√K = 6 = ⌈√K⌉`, so `(R_inner + ⌈√K⌉)² = (R_inner + √K)²` and the code's band equals the canonical upper bound. **At K=36 (project target) there is no divergence.**

**At K=40:** `√K ≈ 6.324 < 7 = ⌈√K⌉`. Code accepts slightly more primes (the integer-band width is 7-wide rather than 6.324-wide), making the code's `geo_I` a superset of spec's. This is *conservative for false-MOAT soundness* (more primes flagged as inner-boundary candidates ⇒ more opportunities to detect a spanning path). It cannot produce a false MOAT.

Codex's cited concrete example — prime norm `6,400,001,011,928,893` at `R_inner=80M, K=40` falling into the code-but-not-spec band — needs verification but even if correct, it only produces false SPANNING at K=40, which Model A allows.

**Verdict.** Real spec↔code divergence, but at K=36 the two predicates are identical. At K=40 the code is strictly more conservative (false MOAT impossible; false SPANNING permissible). **Safe for K=36 pre-flight.** **Should fix before K=40 runs** to match the spec literally and avoid unclear verdicts.

**Fix:** Implement the spec's exact norm-form predicate (eps² ≤ 4·R²·K in i128) with the existing `⌈√K⌉` prefilter retained for short-circuit. Update `goldens/5tile-k36.reference.py:182-192` to match.

#### X-M2 — BZ reconciliation is not bound to runtime `--r-inner`/`--r-outer`  **[ACCEPT]**

**Evidence.** `CMakeLists.txt:108-109` defaults BZ radii to `80M / 80,008,192`; `CMakeLists.txt:128-142` only runs `bz_check.py` if `uv` or `python3` is found, otherwise issues a WARNING and proceeds; `campaign_main.cpp:346-383` accepts arbitrary runtime radii.

**Why this matters.** The spec (line 348) says *"future deployments must re-run `bz_check.py`; the equality is per-deployment, not a theorem of the interval form alone."* A binary built with CMake's default BZ radii then executed with different `--r-inner`/`--r-outer` has *no* BZ evidence for the actual run parameters — this breaks the per-deployment guarantee.

**Can this cause false MOAT?** If `geo_I_w ≠ geo_I` for the runtime radii (i.e., a Gaussian prime sits in the bad zone), then the pipeline's canonical `geo_I` (norm form) disagrees with the lattice-witness `geo_I_w` used in downstream proofs. Theorem 11's Case A.2 uses `p ∈ geo_I ⟹ ‖p‖ ≤ R_inner + √K`, which holds unconditionally from the canonical form — so the **downstream proof chain is safe even if BZ equality fails**. BZ reconciliation is a "both forms agree" convenience, not a load-bearing soundness bolt.

**Verdict.** Real governance gap. Does not threaten false MOAT at any parameters (Theorem 11 uses only the forward-safe implication per spec line 350). Still, the gap violates the spec's explicit per-deployment requirement and the "build fails if BZ non-empty" intent. **Must fix before project-scale runs** — either bind BZ to runtime radii or lock radii at build time.

**Fix:** Either (a) make campaign radii build-time constants tied to BZ target (reject runtime overrides that differ); or (b) run `bz_check.py` inside `campaign_main` before any verdict-producing execution with the actual runtime radii. Also: fail configure if neither `uv` nor `python3` is available.

#### X-M4 — Golden test is a placeholder  **[ACCEPT — must-fix]**

**Evidence.** `tests/test_golden_5tile.cpp:1-10` asserts `1==1`. The Python reference `goldens/5tile-k36.reference.py:105-109` encodes the same `⌈√K⌉` band that X-M1 flags.

**Verdict.** Severe verification gap. The 5-tile golden is the only byte-level cross-engine parity anchor for the CUDA port. Without it, any snapshot difference between CPU reference and CUDA port is impossible to triage. **Must fix before CUDA port.**

**Fix:** Replace placeholder with snapshot comparison test that regenerates via the Python reference and byte-compares against `process_tile` output. Update Python reference to use the spec's exact i128 norm-form predicate (couples with X-M1 fix).

#### X-M5 — Release `verify_invariants` checks shape tables only, not active-predicate  **[ACCEPT — defer]**

**Evidence.** `src/grid.cpp:303-374` checks malformed ranges, shift bounds, and diagonal orphans using only the stored `j_low/j_high` tables; does not re-run `is_active_tile` inside tower interiors. The full active-predicate scan is DEBUG-only (`grid.cpp:377-399`).

**Verdict.** Release builds trust the enumerator to have correctly populated `j_low/j_high`; if the enumerator has a release-only parameter-specific bug, the seatbelt won't catch it. This is a verification hygiene gap, not an observed bug. **Defer** — the enumerator has been exercised by project-scale test runs and by the DEBUG path.

**Fix:** Add a bounded active-predicate audit over tower interiors for I1, OR add explicit evidence (perhaps a sampled spot-check at campaign init) that enumeration was validated for the specific runtime radii.

---

### Unique-and-dismissed (none)

Every finding from either auditor either reflects a real issue or is a mild type/style concern worth noting. Nothing is a pure false positive.

---

## Action table

| Priority | Finding | Source(s) | Fix summary | What it blocks |
|---|---|---|---|---|
| **MUST before CUDA port** | X-M4 Golden test is a placeholder | Codex MAJOR 4 | Write real byte-for-byte snapshot-vs-reference test; couple Python reference update with X-M1 | CUDA-port parity triage; any cross-engine verification |
| **MUST before CUDA port** | C3 Document CUDA anti-patterns in hot loops | Both (Opus n1/gotcha1-2, Codex MINOR 6) | Formal port-backlog item: fixed-capacity buffers, spatial bucketing by lattice offset, bounded neighbor stencils from K, block-local face-port reductions | GPU occupancy, determinism |
| **MUST before project-scale 4090** | O-M1 `annulus_thickness_rhs` uses `4·S·√K` instead of `4·S·√(2K)` | Opus MAJOR 1 | One-line: replace `ceil_isqrt(k_sq)` with `ceil_isqrt(2*k_sq)` OR route through `CampaignConstants::verify_annulus_thickness()` (preferred) | Edge-parameter correctness; spec faithfulness |
| **MUST before project-scale 4090** | C2 Flip `from_radii` default to `strict=true` at verdict entry points | Both (Opus m2, Codex MAJOR 1 mapping) | Switch `campaign_main.cpp` call to `strict=true`; leave tiny-radius tests on `strict=false` explicitly | Prevents alternative harnesses (CUDA, new tests) from bypassing annulus gate |
| **MUST before project-scale 4090** | X-M2 BZ reconciliation not bound to runtime radii | Codex MAJOR 2 | Either lock radii at build time tied to BZ target, or run `bz_check.py` inside `campaign_main` with actual `--r-inner/--r-outer` before verdict; fail configure if no Python runtime | Per-deployment spec compliance |
| **MUST before K=40 runs** | X-M1 geo_tests use `⌈√K⌉` band, not spec norm-form (divergence only at K=40) | Codex MAJOR 1 | Implement i128 `(norm-R²-K)² ≤ 4R²K` with existing prefilter; update 5-tile reference Python to match | K=40 spec faithfulness (at K=36 already identical) |
| **Nice-to-have** | C1 / O-m1 Spec Lemma 6 literal statement vs. grid enumeration at `i=-1` | Both | Either extend enumeration to `i<0` under offset `(1,1)`, or add spec note that axis-prime coverage goes through halo + face_L + reflection closure | Spec-to-code mapping clarity; no soundness impact |
| **Nice-to-have** | O-M2 Exception-from-omp-parallel UB | Opus MAJOR 2 | Per-iteration try/catch with thread-local error slot; aggregate after parallel region | Future >project scale + CUDA error channel |
| **Nice-to-have** | O-m3 `find_tower` `j_low > 0` guard | Opus MINOR 3 | Remove guard, or replace with static-asserted project-params invariant | Tiny-radius edge cases only |
| **Nice-to-have** | O-m4 `is_inner_prime`/`is_outer_prime` int64 narrowing | Opus MINOR 4 | Change signatures to `uint64_t`; drop negative-guard | >2^31.5 radius safety (currently double-fenced) |
| **Nice-to-have** | X-M5 Release `verify_invariants` only checks shape | Codex MINOR 5 | Add bounded active-predicate sample audit at campaign init | Defense-in-depth |
| **Dismiss** | O-m5 `annulus_thickness_rhs` u64 vs u128 comparison | Opus MINOR 5 | No action at project params | — |
| **Dismiss** | O-n1 / O-n2 / O-n3 CPU-side algorithmic hotspots | Opus NITs | Re-raise in CUDA-port backlog only | — |

---

## Verdict paragraph

**Green for pre-flight on Mac Mini.** Both pre-flight runs (R=80M K=36 at delta=8192, and R=800M K=40 at whatever delta is planned) are structurally sound against false MOAT under the Theorem 11 chain as long as runtime BZ reconciliation is either re-run or acknowledged as reliant only on the forward-safe `p ∈ geo_I ⟹ ‖p‖ ≤ R_inner + √K` implication (which is always true from the norm-form definition — see spec line 350). At K=36 the geo_tests divergence X-M1 collapses to identity, so the 80M pre-flight can run without spec contortions. The 800M K=40 pre-flight is acceptable given that the code's `geo_I` is a superset of spec's (conservative, false-SPANNING-only). Neither pre-flight is blocked by any finding in either audit.

**Green for CUDA port only after the MUST-FIX-BEFORE-CUDA items land.** The two blockers are X-M4 (write the real golden test so CUDA snapshot mismatches can be triaged) and C3 (document CUDA-side hot-loop redesigns — not a code fix but a port-plan artifact). The MUST-BEFORE-4090 items (O-M1, C2, X-M2, and X-M1 for K=40) should be closed in the same sprint since they're all one-line-to-short-commit fixes and they improve the robustness of any subsequent CUDA parity tests. False MOAT verdicts remain impossible across every flagged item: at project parameters all soundness gates hold, and the findings only describe edge-parameter windows or governance gaps that don't touch the Theorem 11 reflection-closure chain.
