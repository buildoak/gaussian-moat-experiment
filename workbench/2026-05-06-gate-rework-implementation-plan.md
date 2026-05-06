# Gate Rework Implementation Plan - 2026-05-06

Scope: implement the compact verification spine and the first MOAT hardening
matrix agreed in session. This plan is written for `gpt-5.5 high` workers.

Branch context: `feature/postflight-telemetry-spine`.

## Objective

Make the active acceptance surface exactly four gates:

1. Exact Profile Gate.
2. Independent Tile Sample Gate.
3. SPANNING Cert Gate.
4. MOAT Hardening Gate.

Everything else is debug, regression, telemetry, or archive.

## Current Code Anchors

- Postflight bundle builder:
  `verification/postflight/postflight_orchestrate.py`
- Postflight hard rejection logic:
  `verification/postflight/src/postflight_check.cpp`
- Independent tile recalculation:
  `verification/src/tile_sample_check.cpp`
  and `verification/include/independent_moat.hpp`
- SPANNING coordinate certificate checker:
  `verification/src/span_cert_check.cpp`
- CUDA profile/sample/cert emission:
  `tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp`
- Exact BZ checker:
  `verification/src/bz_exact_check.cpp`
- Current compact doctrine:
  `AGENTS.md`
  and `reference/current-verification-spine.md`

## Gate Semantics

### 1. Exact Profile Gate

Question: did this run describe the exact mathematical object we claim?

Accepted/profile/audit rows must reject unless:

- `k_sq`, `r_inner`, `r_outer`, and `width` are internally exact.
- `region == full-octant`.
- BZ evidence is present and bound to the same row:
  `checked=true`, `clean=true`, `bad_norm_count=0`,
  `override_used=false`.
- Telemetry level is not just a string: `stats_v2` must exist for
  `profile`, `audit`, and `full`.
- Overflow evidence is strict: required counters exist, have integer numeric
  shape, and are all zero.
- CUDA host TileOp overflow counter is checked:
  `host_tileop_counters.emitted_overflow_bit_count == 0`.

### 2. Independent Tile Sample Gate

Question: are CUDA-emitted local tile facts faithful on a controlled sample?

For accepted/profile/audit evidence:

- Sample JSONL without sample manifest is debug only.
- `TILE_SAMPLE_AUDIT_PASS` requires sample JSONL plus manifest.
- Manifest must match the row shape.
- Quotas/classes must be non-empty and well-formed.
- Production sample target is 512 manifested tiles.
- Counts below 512 reject unless the manifest explicitly proves population
  exhaustion for requested classes.

### 3. SPANNING Cert Gate

Question: is there an independently checkable Gaussian-prime coordinate path
from `geo_I` to `geo_O`?

For every accepted `SPANNING`:

- Span certificate is mandatory in `postflight_check.cpp`.
- This must not depend on a soft orchestrator flag.
- The certificate artifact must appear in the artifact table.
- `span_cert_check.cpp` remains the independent coordinate verifier.

### 4. MOAT Hardening Gate

Question: is this MOAT detector row robust enough to treat as serious claim
evidence while the full negative certificate is still being designed?

Near-term hardening is not `MOAT_PROOF_PASS`. It is a falsifier matrix:

- Use same `R_inner = 80,000,000`.
- Prefer widths `17k`, `18k`, `19k`, `20k`.
- Each run must pass Exact Profile and Independent Tile Sample gates.
- Each run must be `MOAT`.
- Monotonicity expectation: if width `W` is `MOAT`, then wider `W+delta`
  should not become `SPANNING`, assuming `geo_O(W)` is the true radial
  separator.
- Violation is a hard investigation trigger, not something to average away.

K37-39 are not part of the MOAT gate. They add no meaningful new graph edges
over K36; they are boundary/BZ stress telemetry only. K40 is where the graph
edge palette actually changes.

## Things To Cut Or Demote

- Remove C++ 5-tile golden from active gates.
- Demote `validate_campaign_run.py` to legacy/debug or make it a wrapper around
  postflight.
- Manifestless sample checks remain debug only.
- C++/CUDA CTests remain build sanity, not mathematical acceptance.
- Moat replay is not a gate.
- Offset re-parametrization is future confidence/falsifier work until the
  independent verifier becomes offset-aware.

## Worker Waves

### Wave 1 - Seal Postflight Acceptance

Use 3 workers in parallel.

#### Worker 1A - Exact Profile + Overflow

Ownership:

- `verification/postflight/src/postflight_check.cpp`
- `verification/postflight/fixtures/*overflow*`
- new negative fixtures as needed

Tasks:

- Require `stats_v2` when telemetry level is `profile`, `audit`, or `full`.
- Make overflow validation strict:
  - missing required overflow blocks reject;
  - string, float, array, null, and malformed nested values reject;
  - all known counters must be integer zero;
  - require `host_tileop_counters.emitted_overflow_bit_count == 0`.
- Keep warnings only for genuinely non-acceptance informational fields.

Gate:

- Existing verification CTest passes.
- New negative fixtures reject malformed/missing/nonzero overflow evidence.
- New negative fixture rejects `telemetry_level=audit` with missing `stats_v2`.

Report:

- Changed paths.
- New fixture names.
- Exact CTest command and pass count.

#### Worker 1B - Sample Manifest Gate

Ownership:

- `verification/postflight/postflight_orchestrate.py`
- `verification/postflight/src/postflight_check.cpp`
- sample audit fixtures

Tasks:

- For accepted/profile/audit rows, reject sample evidence without manifest.
- Ensure `TILE_SAMPLE_AUDIT_PASS` requires manifest present.
- Reject empty quota/class structures.
- Preserve debug ability to run `tile_sample_check --samples` without manifest,
  but such runs cannot satisfy acceptance.
- Keep 512 as production target/minimum unless manifest proves exhaustion.

Gate:

- Manifestless sample acceptance fixture rejects.
- Empty quotas fixture rejects.
- Existing valid sample fixture passes.
- Verification CTest passes.

Report:

- Changed paths.
- Clarify exact row classes affected.

#### Worker 1C - Accepted SPANNING Cert

Ownership:

- `verification/postflight/postflight_orchestrate.py`
- `verification/postflight/src/postflight_check.cpp`
- span proof fixtures

Tasks:

- Make orchestrator default `claim_proof_required=true` for accepted rows.
- In `postflight_check.cpp`, require valid span proof unconditionally when
  `row_class=accepted && verdict=SPANNING`.
- Require certificate artifact table entry for accepted SPANNING.
- Keep sweep/debug rows able to report `CLAIM_PROOF_MISSING` without hard
  reject if they are not accepted rows.

Gate:

- Accepted SPANNING without cert rejects.
- Accepted SPANNING with valid cert passes.
- Sweep SPANNING without cert reports non-proof status but does not masquerade
  as accepted proof.
- Verification CTest passes.

Report:

- Changed paths.
- Status vocabulary changes, if any.

### Wave 2 - Cut Stale Active Gates

Use 1 worker after Wave 1.

#### Worker 2A - Golden / Legacy Gate Demotion

Ownership:

- `tiles-maxxing/cpp-campaign-v2/goldens/*`
- `tiles-maxxing/cpp-campaign-v2/tests/test_golden_5tile.cpp`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/validate_campaign_run.py`
- docs that describe these as active gates

Tasks:

- Remove or archive the C++ 5-tile golden from active gate language.
- If test remains, make it explicitly regression/debug, not acceptance.
- Demote `validate_campaign_run.py` in docs or convert its surface to point at
  postflight.
- Do not invent a replacement golden.

Gate:

- C++ CTest passes.
- Verification CTest passes.
- Root docs no longer present golden or validate script as active acceptance.

Report:

- Changed paths.
- Any intentionally archived/deleted paths.

### Wave 3 - BZ/Profile Binding

Use 1 worker after Wave 1.

#### Worker 3A - Exact BZ Row Binding

Ownership:

- `verification/postflight/src/postflight_check.cpp`
- `verification/src/bz_exact_check.cpp` only if needed
- postflight fixtures/schemas

Tasks:

- Require BZ record shape in accepted/profile/audit rows.
- Verify BZ row fields match exact `k_sq`, `r_inner`, `r_outer`, and width when
  those fields are present.
- Reject `override_used=true`.
- Reject missing `checked`, `clean`, or `bad_norm_count`.
- Do not attempt non-square K expansion here; keep K37-39 as telemetry until
  non-square BZ is explicitly designed.

Gate:

- Valid K36 BZ fixture passes.
- Missing/dirty/override/radius-mismatch fixtures reject.
- Verification CTest passes.

Report:

- Changed paths.
- Exact BZ fields now mandatory.

### Wave 4 - MOAT Hardening Matrix

Use 2 workers after Wave 1 and Wave 3.

#### Worker 4A - Run Matrix Script

Ownership:

- new script under `verification/postflight/` or
  `tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/`
- no changes to core CUDA logic unless needed for flags only

Tasks:

- Add a script/runbook that defines the K36 Tsuchimura hardening matrix:
  - `R_inner=80,000,000`
  - widths `17,000`, `18,000`, `19,000`, `20,000`
  - `R_outer = R_inner + width`
  - `--no-early-exit`
  - audit/profile telemetry
  - sample manifest + tile sample out
  - 512 production sample target
  - postflight orchestration with fail-on-reject
- Emit per-width output directories with profile, manifest, samples, bundle,
  postflight report, normalized row, and command log.
- Mark K37-39 as optional boundary telemetry, not part of acceptance.

Gate:

- Dry-run mode prints exact commands without executing CUDA.
- Script refuses to run without required output paths.
- Script has no hard-coded local temp path except a user-supplied output root.

Report:

- Changed paths.
- Dry-run output summary.

#### Worker 4B - MOAT Matrix Postflight Summary

Ownership:

- `verification/stats/` or new postflight summary script
- docs/runbook for matrix interpretation

Tasks:

- Add a small summarizer for the four-width matrix.
- It reports:
  - verdict per width;
  - profile gate status;
  - sample gate status;
  - BZ status;
  - overflow status;
  - `stats_v2` present;
  - `geo_I`/`geo_O` counts;
  - produced/ingested/active counts;
  - monotonicity expectation result.
- If any narrower width is `MOAT` and a wider width is `SPANNING`, flag
  `MONOTONICITY_VIOLATION`.
- Do not report `MOAT_PROOF_PASS`.

Gate:

- Unit test on synthetic rows catches a monotonicity violation.
- Unit test on all-MOAT `17k..20k` passes.
- Stats tests pass.

Report:

- Changed paths.
- Example summary output.

### Wave 5 - Docs Consolidation

Use 1 worker after implementation.

#### Worker 5A - Canonical Docs Update

Ownership:

- `AGENTS.md`
- `reference/current-verification-spine.md`
- `verification/README.md`
- `verification/postflight/README.md`
- root `README.md`

Tasks:

- State the four active gates exactly.
- State that `MOAT_PROOF_PASS` is reserved for future negative certificate.
- State that the near-term MOAT matrix is hardening/falsifier telemetry:
  `17k`, `18k`, `19k`, `20k`, same `R_inner`.
- State that K37-39 are boundary/BZ stress telemetry only.
- Remove active-gate language for C++ 5-tile golden, moat replay, and
  `validate_campaign_run.py`.

Gate:

- `rg` shows no active reference to `current-gate-board.md`.
- `rg` shows no active claim that C++ 5-tile golden is an acceptance gate.
- Verification CTest still passes.

Report:

- Changed paths.
- Any docs intentionally archived.

## Final Acceptance Run

After all waves:

```text
cmake --build verification/build -j
ctest --test-dir verification/build --output-on-failure

cmake --build tiles-maxxing/cpp-campaign-v2/build -j
ctest --test-dir tiles-maxxing/cpp-campaign-v2/build --output-on-failure

python3 -m py_compile verification/stats/*.py verification/postflight/*.py
python3 -m unittest discover -s verification/stats -p 'test*.py'
```

Then run on 4090:

```text
K=36
R_inner=80,000,000
widths=17,000 18,000 19,000 20,000
```

Each run must emit:

- profile JSON;
- tile sample JSONL;
- sample manifest;
- postflight bundle/report;
- normalized sweep row;
- command log.

Hard rejection:

- any postflight reject;
- any nonzero/malformed overflow evidence;
- missing `stats_v2`;
- missing sample manifest;
- sample audit failure;
- BZ dirty/override/mismatch;
- any `MONOTONICITY_VIOLATION`.

## Non-Goals For This Batch

- Do not implement full `MOAT_PROOF_PASS`.
- Do not implement complete all-tile negative certificate yet.
- Do not implement offset re-parametrization yet.
- Do not treat K37-39 as acceptance.
- Do not revive C++ goldens or moat replay as gates.

## Coordinator Notes For Dispatch

Workers are not alone in the codebase. They must not revert unrelated edits,
especially the current archive moves under `reference/archive/`.

Each worker should finish with:

```text
Done:
Found:
Changed:
Evidence:
Blocked:
Next:
```

Acceptance belongs to the coordinator, not to worker confidence.
