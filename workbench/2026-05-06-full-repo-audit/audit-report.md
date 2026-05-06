# Gaussian Moat CUDA Full Repo Audit - 2026-05-06

Scope: code, docs, verification, scripts, tests, and cleanup posture for
`feature/postflight-telemetry-spine` after the compact verification-spine
rewrite.

The audit used five `gpt-5.5 xhigh` auditors plus coordinator checks. It was
read-only except for the already-requested cleanup alignment that moved the
pre-push/history runbooks under `reference/archive/`.

## Executive Summary

The repo is directionally sane but not paper-grade yet.

The strongest positive result is that the new post-flight TileOp recalculation
is independent from `cpp-campaign-v2` and CUDA campaign code: verification builds
only link `verification_common` plus `nlohmann_json`, and `tile_sample_check`
rebuilds TileOps through `verification/include/independent_moat.hpp`.

The main negative result is that the compact gate doctrine is stricter than the
current executable acceptance checks. Several metadata-shaped bundles can still
pass post-flight without the evidence the doctrine says should be mandatory:
manifest-backed samples, real `stats_v2`, strict overflow shape, strong MOAT
full ingest, and SPANNING certs for accepted SPANNING rows.

The second major issue is stale operational surface area: Vast helpers, root
README, golden docs/tests, CUDA planning docs, and `validate_campaign_run.py`
still carry old gate assumptions.

## What Is Healthy

- `verification/` is structurally independent from campaign code.
  Evidence: `verification/CMakeLists.txt` links only `verification_common` and
  `nlohmann_json`; `cmake/no_campaign_includes.cmake` scans for campaign and
  CUDA campaign includes.
- Local verification suite passes: `25/25`.
- Local C++ suite passes: `115/115`, with 2 expected skips.
- Stats Python tests pass: `10/10`.
- `cpp-campaign-v2` is still a useful mechanics reference for grid, sieve,
  TileOp layout, geo tests, streaming compositor, and deterministic behavior.
- The live compact-doctrine docs are mostly aligned: `AGENTS.md`,
  `reference/current-verification-spine.md`, `agents-directives/experiment-contract.md`,
  `verification/README.md`, and `verification/postflight/README.md`.

## Critical Findings

### P0/P1 - Postflight Sample Audit Can Be Weakened

`postflight_orchestrate.py` can run `tile_sample_check` with `--samples` but no
manifest. Without a manifest, quota/class enforcement is absent. In
`postflight_check.cpp`, empty quotas can behave like all requested classes are
exhausted, letting tiny sample counts pass.

Evidence:

- `verification/postflight/postflight_orchestrate.py:530`
- `verification/postflight/src/postflight_check.cpp:947`
- `verification/postflight/src/postflight_check.cpp:955`

Fix: require a sample manifest whenever sample JSONL is present; reject empty
quotas for sample-audit pass; add a negative CTest fixture for manifestless
sample pass.

### P0/P1 - Accepted SPANNING Can Miss Coordinate Cert

The orchestrator defaults `--claim-proof-required` to false. It writes that into
the bundle row, and `postflight_check` only reports `CLAIM_PROOF_MISSING` when
that flag is true. That can let an accepted SPANNING row pass without the
coordinate certificate the doctrine says is mandatory.

Evidence:

- `verification/postflight/postflight_orchestrate.py:624`
- `verification/postflight/postflight_orchestrate.py:336`
- `verification/postflight/src/postflight_check.cpp:1164`

Fix: for `row_class=accepted` and `verdict=SPANNING`, require a valid span cert
inside `postflight_check` regardless of orchestrator flags.

### P1 - `stats_v2` Is Optional Even When Telemetry Claims Audit/Profile

`check_stats_v2` returns if no `stats_v2` exists. `check_row_contract` checks
the declared telemetry level string, not the presence of actual telemetry.
Accepted/audit fixtures can pass without `stats_v2`.

Evidence:

- `verification/postflight/src/postflight_check.cpp:623`
- `verification/postflight/src/postflight_check.cpp:320`

Fix: require `stats_v2` for `telemetry_level=profile|audit|full`, and require
the core fields for accepted/profile rows.

### P1 - MOAT Full-Ingest Evidence Is Too Weak

`check_moat_full_ingest` accepts `produced == ingested` even when
`early_exit.full_ingest` is absent and without requiring equality to active or
independently enumerated active tiles.

Evidence:

- `verification/postflight/src/postflight_check.cpp:857`
- `verification/postflight/src/postflight_check.cpp:881`

Fix: for `MOAT`, require explicit full-ingest metadata and
`produced == ingested == active`; when an independent active-tile count is
present, require equality there too.

### P1 - Overflow Validation Misses Shapes And Current Host Counter

Malformed overflow counters such as strings can be ignored. Postflight also
misses the current CUDA `host_tileop_counters.emitted_overflow_bit_count`; it
checks recursive `overflow_counters` and a legacy profile field.

Evidence:

- `verification/postflight/src/postflight_check.cpp:361`
- `verification/postflight/src/postflight_check.cpp:379`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp:1795`

Fix: reject unknown/non-numeric overflow values and explicitly require
`host_tileop_counters.emitted_overflow_bit_count == 0`.

### P1 - C++ Golden Is Stale And Not Tested

The C++ 5-tile golden manifest still uses old offset/hash data while current
code uses canonical offset `(0,0)`. The test is a placeholder
`EXPECT_EQ(1, 1)`. An auditor-generated current snapshot failed against the
committed golden with header hash mismatches.

Evidence:

- `tiles-maxxing/cpp-campaign-v2/include/campaign/constants.h:53`
- `tiles-maxxing/cpp-campaign-v2/goldens/5tile-k36.manifest.json:4`
- `tiles-maxxing/cpp-campaign-v2/tests/test_golden_5tile.cpp:8`

Fix: either regenerate and enforce the golden, or demote/remove it so it cannot
be mistaken for a reference gate.

### P1 - BZ Enforcement Is Not Exact For Runtime/Preflight Radii

Methodology says BZ equality is per deployment. C++ CMake runs `bz_check.py`
from config defaults, and can skip if Python/uv is absent. `preflight.py` passes
`-DBZ_R_INNER/-DBZ_R_OUTER`, but CMake does not consume those variables.
`campaign_main` checks K and thickness, not exact BZ/profile equality.

Evidence:

- `methodology/tile-operator-definition-v-claude.md:341`
- `tiles-maxxing/cpp-campaign-v2/CMakeLists.txt:130`
- `tiles-maxxing/cpp-campaign-v2/CMakeLists.txt:141`
- `tiles-maxxing/cpp-campaign-v2/scripts/preflight.py:50`
- `tiles-maxxing/cpp-campaign-v2/apps/campaign_main.cpp:368`

Fix: make BZ parameters explicit runtime/profile fields and require exact
profile BZ status for accepted rows; remove or hard-fail the Python-missing
path.

## Other Findings

- 512 samples are implemented as a default/floor, not a hard cap. If “cap” is
  literal, enforce `--tile-sample-count <= 512` or rename the doctrine to
  “default/minimum”.
- `row_class=profile` currently accepts profile-level telemetry, while docs say
  accepted/profile postflight rows should use audit/full.
- `span_cert` schema is stale: executable code uses `path` as coordinate array;
  schema describes `path` as string plus `points`.
- `gaussian_prime_point(Point{1,1})` rejects norm 2 in independent verifier
  logic; likely irrelevant at production radii but wrong in the oracle.
- Non-streaming `Compositor` can return `MOAT` without proving full ingest if
  reused with missing/out-of-order columns. App path is currently ordered; API is
  fragile.
- Annulus thickness checks differ between CLI and strict library path.
- `validate_campaign_run.py` is a drifted parallel gate: stricter in some places,
  weaker in others, and not aligned with sample/cert postflight.
- Root CI is effectively absent; nested C++ workflow will not run as repo CI.
- Stats/schema tests are not fully wired into CTest.

## Docs And Archive Findings

- Root `README.md` still points to deleted `reference/current-gate-board.md`.
- `tiles-maxxing/vast-ai/README.md` and Vast scripts describe stale legacy paths
  and origin-reachability semantics.
- `methodology/tile-operator-definition-v-claude.md` has broken BZ provenance
  paths: it cites `build/bz_check.py` and missing supportive docs.
- `verification/compositor-replay/README.md` partly contradicts the demotion of
  replay and bounded UF.
- CUDA `planning/` docs still look canonical while encoding old snapshot/golden
  gates.
- Golden README overstates authority and references old `lemmas_v2` paths.
- `reference/archive/README.md` should include the loose archived runbooks:
  `pre-push-secret-check.md` and `heavy-history-cleanup-plan.md`.

## Research Assets Cleanup

Measured:

- `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets`:
  `7.2M`, contains unrelated travel/business/Claude ecosystem assets.
- `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda`:
  `6.9M`, contains Gaussian moat evidence from 2026-05-04 and 2026-05-05.

Recommendation: move only the `gaussian-moat-cuda` subtree into a tracked or
ignored repo-local archive/evidence path, then record provenance. Do not move
the full `research-assets` root into this repo unless intentionally importing
unrelated Pratchett assets.

## Verification Evidence

Coordinator-run checks:

```text
verification CTest: 25/25 passed
cpp-campaign-v2 CTest: 115/115 passed, 2 skipped
stats Python tests: 10/10 passed
```

CUDA was not rerun in this audit. Local CUDA build dirs are not reliable CUDA
evidence on the Mac Mini.

## Proposed Fix Waves

### Wave 1 - Seal Postflight Acceptance

- Require manifest-backed samples.
- Reject empty sample quotas for sample-audit pass.
- Require `stats_v2` for claimed telemetry.
- Require SPANNING cert for accepted SPANNING.
- Strengthen MOAT full-ingest equality.
- Strict overflow typing and host TileOp overflow counter check.
- Add negative fixtures for each bypass.

### Wave 2 - Restore Reference/Gate Hygiene

- Fix or demote C++ 5-tile golden.
- Make BZ/profile enforcement exact and runtime-aware.
- Guard non-streaming `Compositor` or clearly make it test-only/non-gate.
- Centralize annulus thickness predicate.
- Wire schema tests into CTest.

### Wave 3 - Prune Operational Staleness

- Replace/archive stale Vast helpers.
- Update root README and Vast README.
- Reduce compositor replay README to debug-only.
- Move/mark CUDA planning docs as historical.
- Update golden docs and methodology BZ paths.
- Decide on repo-local import of Gaussian moat research-assets subtree.

## Open Questions

- Is 512 a hard maximum or the normal production default/minimum? Auditors found
  code implements a floor/default, not a cap.
- Should `row_class=profile` mean “profile telemetry only” or “postflight
  profiled accepted evidence requiring audit/full”?
- Should the C++ 5-tile golden remain tracked, or should it be retired in favor
  of postflight/sample/cert checks?
- Where should Gaussian moat research assets live inside the repo: tracked
  `reference/archive/evidence/`, ignored `evidence/`, or another explicit path?

## Done / Next

Done: mapped repo-level sanity and identified the main blockers to paper-grade
verification.

Next: implement Wave 1 before new publication-grade runs. The current code is
usable for engineering sweeps, but postflight acceptance is not yet tight enough
to be treated as paper-grade evidence.
