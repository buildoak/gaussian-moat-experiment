# Post-Flight Verification And Telemetry Plan - 2026-05-05

## Purpose

Replace the current verification sprawl with a small post-flight spine that is
stronger, easier to explain, and closer to paper-grade evidence.

The goal is not more gates. The goal is fewer gates with clearer authority:

1. build sanity before runs;
2. hard post-flight run contract after runs;
3. claim-specific coordinate proof for `SPANNING`;
4. first-class telemetry for prediction and anomaly understanding.

Post-flight reports must use explicit status vocabulary:

- `REJECT` - evidence is invalid for the requested row class.
- `RUN_CONTRACT_PASS` - bundle/run health is coherent, but no mathematical
  claim proof has been accepted.
- `TILE_SAMPLE_AUDIT_PASS` - run health passed and stratified sampled TileOps
  independently regenerate, but this is still sampled evidence.
- `SPAN_PROOF_PASS` - `SPANNING` coordinate certificate passed.
- `MOAT_PROOF_PASS` - reserved for a future stronger negative proof that also
  closes TileOp mathematical faithfulness beyond sampled audits.
- `CLAIM_PROOF_MISSING` - run health passed, but the required proof artifact
  is absent or not implemented.

Only `SPAN_PROOF_PASS` and future `MOAT_PROOF_PASS` should be described as
claim-proof acceptance. `MOAT_PROOF_PASS` is intentionally unreachable in this
plan. MOAT replay is removed from the build plan: running a second compositor
over emitted TileOps mostly checks the emitted surface, not TileOp mathematical
faithfulness.

Current campaign verdicts remain static-annulus claims:

- `SPANNING` means `ANY-SPAN`: some component connects `geo_I` to `geo_O`
  inside the tested static annulus.
- full-ingest `MOAT` means `ANY-SHELL-MOAT`: no component connects `geo_I`
  to `geo_O` inside the tested static annulus.

This plan does not claim origin-component reachability, exact global thresholds,
or Tsuchimura-style source-connected bounds.

## User Decisions Locked

- Official gate logic should be cut ruthlessly.
- Post-flight failure is a hard rejection. This is math evidence, not a soft
  quality warning.
- Tsuchimura/K34/golden/tiny-UF gates are not part of the official spine for
  now. They may remain historical sanity or dev-regression surfaces.
- CUDA should emit persisted deterministic stratified tile samples.
- Post-flight C++ should regenerate sampled TileOps independently and compare
  them against CUDA-emitted TileOps.
- Accepted rows use `4096` tile samples. Cheap sweep/audit rows may use `1024`.
- Sample classes: `geo_I`, `geo_O`, `axis`, `diagonal`, `high_pressure`, and
  deterministic random background.
- New C++ verifier surface should live under `verification/postflight/` and
  remain independent from `campaign/` and `cuda_campaign/` imports.
- Full TileOp snapshots are optional forensic/debug surfaces only. MOAT replay
  is not an official post-flight gate or implementation wave.
- Telemetry overhead of about `5-10%` is acceptable for accepted/profile runs,
  not for broad exploratory sweeps unless explicitly enabled.
- Telemetry levels are `none`, `profile`, `audit`, and `full`.
- Accepted post-flight proof rows are square-K rows until exact non-square BZ
  support is implemented. Non-square K38/K40 rows may be normalized and studied
  as telemetry/candidate evidence, but not promoted through the same proof
  status vocabulary without a BZ generalization.

## Reduced Architecture

### 1. Build Sanity

**When it fires:** before campaign runs and after code changes.

**Question:** is the executable internally sane enough to produce evidence?

**Keep:**

- local C++ CTest;
- CUDA CTest on CUDA hosts;
- exact BZ logic;
- K-dependent boundary assertions for `geo_I`, `geo_O`, collars, axis,
  diagonal, face strips, and constants;
- grid/tower invariants;
- no-campaign-import check for independent verification code.

The no-campaign-import check must cover more than textual `#include` matches:
post-flight CMake must not add campaign/cuda include directories, link campaign
or CUDA campaign targets, include `tileop_internal.h`, reuse campaign snapshot
or compositor implementations, or depend on generated campaign headers. The
post-flight surface may duplicate wire formats and minimal math; it must not
reuse the production implementation as its verifier.

**Cut from official claim spine:**

- narrow Tsuchimura `R_outer` boundary gate;
- K34 regression gate;
- golden JSON smoke;
- bounded `exact_global_uf_tiny`;
- CPU/CUDA diff as a named acceptance gate.

Those cut surfaces may stay as dev/debug/historical tools, but they should not
be described as the paper-grade verification spine.

### 2. Post-Flight Run Contract

**When it fires:** after every accepted/profile run.

**Question:** is this run bundle coherent, full enough for its claim, BZ-clean,
zero-overflow, and tied to the intended build/input/artifacts?

**Hard rejects:**

- profile/stdout/run-index disagree on K, radii, width, region, verdict, tile
  counts, or command;
- accepted proof row is not `full-octant`;
- `width != R_outer - R_inner`;
- annulus thickness fails the campaign soundness bound;
- independently enumerated active tile count disagrees with the run profile;
- `bz.checked != true`, `bz.clean != true`, `bz.override_used != false`, or
  nonzero `bz.bad_norm_count`;
- any CUDA or emitted TileOp overflow counter is nonzero;
- `MOAT` without full ingest;
- accepted `SPANNING` without required certificate artifacts;
- missing artifact hashes, sizes, or schema versions;
- missing or mismatched grid hash, constants hash, MR witness hash, commit,
  build identity, or CUDA architecture;
- telemetry level is too weak for the claimed row class.

**Inputs:**

- run directory;
- profile JSON;
- stdout/log;
- BZ record;
- command/build/commit metadata;
- sample manifest and samples JSONL when telemetry is `audit` or `full`;
- optional SPANNING certificate;
- optional TileOp snapshot and snapshot manifest for forensic/debug work.

**Outputs:**

- `postflight.report.json`;
- compact Markdown summary;
- normalized claim row for sweep tables;
- artifact hash/size table;
- one of the explicit status values listed in the purpose section.

`validate_campaign_run.py` is a useful predecessor, but it is bundle plumbing,
not the final post-flight object. It should be replaced once
`verification/postflight/` reaches parity.

### 3. Claim Proof

**When it fires:** after the run contract passes.

**Question:** does this specific mathematical claim have an independent proof
artifact?

#### SPANNING

For every accepted `SPANNING` claim:

1. CUDA emits TileOps.
2. The CPU streaming compositor detects a component with both `geo_I` and
   `geo_O` reach.
3. With path tracing enabled, the compositor records a stitch path in
   tile/group space.
4. The campaign materializes a coordinate certificate by regenerating primes
   only in the stitch-path tile tube.
5. Post-flight C++ independently checks the coordinate certificate.

The stitch trace is only a locator. The accepted proof object is the coordinate
path certificate.

The certificate checker must verify:

- schema and row metadata;
- every point is in the canonical full octant;
- every point is inside the annulus;
- every point is an independently verified Gaussian prime;
- every consecutive step has squared distance `<= K`;
- the first point is in `geo_I`;
- the last point is in `geo_O`;
- certificate path appears in the profile/command when applicable;
- certificate SHA appears in the artifact table;
- certificate row metadata and region match the profile;
- certificate binds to profile, build identity, grid hash, constants hash, and
  command metadata in post-flight.

#### MOAT

There is no official `MOAT` proof gate in this plan.

For current work, a `MOAT` row can pass run contract and tile-sample audit, but
it should still report `CLAIM_PROOF_MISSING` for mathematical negative proof.
This is an honesty rule, not a blocker for sweeps. The row may be used as
high-confidence static-annulus detector evidence and telemetry, but not as a
paper-grade negative certificate.

MOAT replay is deliberately not part of the official spine. Full TileOp
snapshots may be used later for forensic debugging, compositor regression
checks, or artifact preservation, but replay should not consume implementation
attention now.
The next valuable negative-proof idea would need to attack TileOp faithfulness
directly, not merely run another compositor over emitted TileOps.

### 4. Tile Audit And Telemetry

**When it fires:** accepted rows; cheap sweeps when `audit` is explicitly
enabled.

**Question:** do sampled production TileOps match independent C++ regeneration,
and what did the annulus look like?

#### Tile Sample Audit

CUDA/campaign persists deterministic stratified samples:

- row metadata: `K`, `R_inner`, `R_outer`, width, region;
- sample plan: level, seed, quotas, class counts;
- sample class: `geo_I`, `geo_O`, `axis`, `diagonal`, `high_pressure`,
  `random`;
- tile coordinate: `i`, `j`, `a_lo`, `b_lo`;
- pressure metadata when available;
- raw TileOp bytes or structured TileOp fields.

Post-flight C++ regenerates the TileOp independently from math/primes and
compares semantic signatures modulo label renaming.

One mismatch hard rejects accepted rows.

Additional hard rejects:

- duplicate sample entries;
- sampled tile outside the independently enumerated grid;
- manifest/sample count mismatch;
- missing class quotas;
- emitted class counts below required quotas unless the manifest records
  population exhaustion for that class;
- accepted row sample count below the row-class minimum;
- sample row metadata does not match the profile.

#### Telemetry Levels

`none`

- Fast exploratory mode.
- Stdout only.

`profile`

- Low-overhead JSON profile.
- Required fields:
  - schema version and telemetry level;
  - command, commit/build metadata, CUDA arch;
  - device and driver;
  - K, radii, width, region;
  - verdict and early-exit state;
  - BZ record;
  - active/produced/ingested tiles and columns;
  - chunk/slab/overshoot counts;
  - total timing and CUDA stage timing;
  - memory footprint;
  - overflow counters;
  - emitted overflow TileOp count;
  - throughput.

`audit`

- `profile` plus:
  - persisted sample manifest and samples JSONL;
  - 4096 accepted-row samples or 1024 cheap-sweep samples;
  - high-pressure tile summaries;
  - artifact hashes;
  - first overflow diagnostics if any.

`full`

- `audit` plus:
  - optional full TileOp snapshot;
  - component census and heavy diagnostics;
  - large artifact hashes/sizes when snapshots are emitted.

`full` is not for broad sweeps unless explicitly budgeted. It does not upgrade
`MOAT` to proof status.

#### Prediction Telemetry

For K36 50/60/70M and K40+ sweeps, telemetry should support prediction rather
than only run acceptance.

Add C++ profile fields for:

- candidate count distribution;
- Gaussian prime count distribution;
- group count distribution;
- total port count distribution;
- max face port count distribution;
- high-pressure tile top-N coordinates;
- `geo_I` and `geo_O` tile/port populations;
- component census when available:
  - `I-only` component count;
  - `O-only` component count;
  - `I&O` component count;
  - largest component sizes;
  - largest boundary-touching components.

This is how future sweeps distinguish real connectivity failure from boundary
population artifacts.

## Snapshot Artifact Size

Current TileOp snapshot size is:

```text
120 + tile_count * 256 bytes
```

Existing profile tile counts imply:

| Row scale | Approx tiles | Snapshot size |
|---|---:|---:|
| lower K36 `R ~= 73.36M`, W32768 | 29.1M | 7.45 GB |
| `R ~= 100M`, W32768 | 39.7M | 10.15 GB |
| `R ~= 300M`, W32768 | 119.0M | 30.46 GB |
| `R ~= 980M`, W32768 | 388.7M | 99.5 GB |
| `R ~= 1B`, W32768 | 396.6M | 101.5 GB |

Full snapshots are technically feasible for lower K36 and roughly `R ~= 100M`.
Near `R ~= 1B`, they need explicit artifact budget, local-disk planning,
streaming processing, and likely compression/summary strategy.

Because snapshots become very large and do not close TileOp faithfulness by
themselves, they are optional forensic/debug artifacts in this plan rather than
required post-flight gates.

## Migration Plan

Keep or fold:

- `verification/` as the independent verifier home;
- `span_cert_check`, folded into post-flight;
- `tile_sample_check`, upgraded and folded into post-flight;
- `bz_exact_check`, folded into post-flight/run contract;
- no-campaign-import check;
- schemas.

Demote:

- Tsuchimura gates;
- K34 regression;
- golden JSON smoke;
- CPU/CUDA diff;
- tiny global-UF smoke.

Replace:

- `validate_campaign_run.py` with `verification/postflight` after parity.

Delete/deprecate later:

- runner-specific validator flags that encode old gate-board assumptions;
- docs that present Tsuchimura/K34/goldens/tiny-UF as the official acceptance
  spine.

## Implementation Waves

### Wave 1 - Post-Flight Skeleton

Create `verification/postflight/` with:

- C++ CLI entrypoint;
- schemas for post-flight report and normalized row;
- run contract checks covering the current lower-K36 bundle;
- hostile fixtures for mismatch, unclean BZ, overflow, incomplete MOAT,
  missing certificate, missing artifacts.

Acceptance:

- current lower-K36 evidence bundle emits precise statuses:
  `SPAN_PROOF_PASS` for the SPANNING row with valid certificate, and
  `CLAIM_PROOF_MISSING` or `TILE_SAMPLE_AUDIT_PASS` for MOAT;
- hostile fixtures hard reject;
- no campaign/cuda imports in post-flight code.

### Wave 2 - Telemetry Level Enum

Add `--telemetry=none|profile|audit|full`.

Acceptance:

- `profile` preserves existing profile fields and adds stable telemetry level;
- `audit` and `full` are accepted only with their required telemetry artifacts;
- old `--stats-level profile` is mapped or deprecated deliberately.

### Wave 3 - Stratified Tile Sampling

Replace current sample emission with quota-based deterministic stratified
sampling.

Acceptance:

- accepted rows emit 4096 samples;
- cheap audit rows emit 1024 samples;
- manifest class quotas match emitted sample class counts;
- post-flight C++ independently regenerates all sampled TileOps;
- any mismatch hard rejects.

### Wave 4 - SPANNING Certificate Post-Flight

Move SPANNING certificate checking under post-flight.

Acceptance:

- lower-K36 SPANNING coordinate certificate passes post-flight;
- corrupted coordinate, non-prime point, out-of-annulus point, too-long step,
  wrong endpoint band, or wrong artifact binding hard rejects.

### Wave 5 - Telemetry And Sweep Rows

Make telemetry useful for prediction and future moat selection.

`sweep_rows.jsonl` rows must keep:

- `detector_status`: what the static-annulus detector observed
  (`ANY_SPAN_DETECTED`, `ANY_SHELL_MOAT_DETECTED`, or
  `UNKNOWN_DETECTOR_STATUS`);
- `proof_status`: whether the mathematical claim proof has been accepted
  (`SPAN_PROOF_PASS`, future `MOAT_PROOF_PASS`, or
  `CLAIM_PROOF_MISSING`);
- post-flight/audit health as separate fields such as `postflight_status`,
  `run_contract_status`, and `tile_sample_audit_status`.

Profile-only rows may be normalized for analysis, but nullable fields must stay
nullable and missing proof artifacts must not be inferred from detector
verdicts. In particular, a current `MOAT` profile normalizes to detector
evidence plus `proof_status=CLAIM_PROOF_MISSING` unless a future negative-proof
gate explicitly accepts it.

Acceptance:

- profile/audit outputs can be normalized into `sweep_rows.jsonl`;
- K36 50/60/70M and K40+ rows can be represented with nullable fields for old
  evidence;
- C++ telemetry includes the pressure and component-census fields needed to
  explain connectivity transitions;
- snapshot fields are recorded only when optional forensic snapshots are
  explicitly emitted.

### Wave 6 - Docs And Gate Board Cleanup

Update repo references so post-flight is the official spine.

Acceptance:

- `reference/README.md` points to this plan;
- old gate-board language is marked superseded or scoped to historical/dev
  sanity;
- lower-K36 evidence report names the remaining MOAT proof gap precisely without
  implying replay is the next official gate.

## GPT-5.5 High Worker Execution Strategy

Implementation should proceed through bounded GPT-5.5 high workers. Every worker
prompt must include:

```text
You are not alone in the codebase. Do not revert or overwrite edits made by
others. Preserve unrelated dirty state. Keep writes inside your owned scope.
```

### Worker W1 - Post-Flight Spine

**Scope:** `verification/postflight/**`,
`verification/schemas/postflight-report.schema.json`,
post-flight tests/fixtures, and `verification/CMakeLists.txt`.

**Non-goals:** no CUDA changes, no telemetry enum, no replay engine.

**Gate:** lower-K36 mirrored bundle gets precise statuses; hostile fixtures
reject mismatched K/radii/verdict/tile counts/command, unclean BZ, overflows,
incomplete MOAT, missing cert/artifact/hash/schema; verification CTest and
no-campaign-import checks pass.

### Worker W2 - Telemetry Levels

**Scope:** CUDA CLI/profile emission in
`tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp`, profile
schemas, and CUDA help/tests.

**Non-goals:** no stratified sampler rewrite, no MOAT replay.

**Gate:** `--telemetry=none|profile|audit|full` has stable behavior;
`--stats-level profile` is deliberately mapped or rejected with migration text;
profile rows include `telemetry_level`; post-flight rejects insufficient
telemetry for claimed rows.

### Worker W3A - CUDA Stratified Sampling

**Scope:** CUDA sample planning/emission helpers plus campaign app integration.

**Non-goals:** no post-flight checker implementation beyond emitting agreed
schema.

**Gate:** small CUDA smoke emits manifest and JSONL with deterministic seed,
class quotas, class counts, and exact sample count.

### Worker W3B - Independent Sample Audit

**Scope:** `verification/src/tile_sample_check.cpp`, post-flight sample audit
integration, sample schemas, fixtures.

**Non-goals:** no CUDA emission changes.

**Gate:** valid samples pass; one-byte, label, metadata, duplicate, under-quota,
and out-of-grid fixtures reject.

### Worker W4 - SPANNING Certificate Post-Flight

**Scope:** post-flight certificate checker/integration, span cert schema and
binding, hostile cert fixtures. Touch CUDA cert emission only if binding
metadata is required.

**Non-goals:** no stitch discovery redesign, no MOAT replay.

**Gate:** lower-K36 coordinate cert passes; corrupted coordinate, non-prime,
out-of-annulus, too-long step, wrong endpoint band, wrong binding, and missing
hash reject.

### Worker W5 - Telemetry And Sweep Rows

**Scope:** telemetry normalization code, schemas, and report integration for
`sweep_rows.jsonl`; C++ telemetry fields needed for prediction; docs/examples
for K36 50/60/70M and K40+ rows.

**Non-goals:** no MOAT replay, no large snapshot parser, no negative proof
claim.

**Gate:** existing evidence can be normalized with nullable fields; new
profile/audit rows include pressure/component telemetry; rows distinguish
detector evidence from proof status.

### Worker W6 - Docs Cleanup

**Scope:** `reference/README.md`, `reference/current-gate-board.md`,
`verification/README.md`, lower-K36 evidence note, plan references.

**Non-goals:** no code, no tickets.

**Gate:** search finds no misleading “official acceptance spine” language for
deprecated gates; docs do not present MOAT replay as the next official gate;
docs do not claim `MOAT_PROOF_PASS`.

## Global Stop Rules

Stop if a worker needs to change methodology semantics, claim meanings, or
accepted row definitions. Stop if post-flight links against campaign/CUDA code.
Stop if CUDA telemetry adds overhead outside the accepted `5-10%`
profile/audit budget. Stop before reintroducing MOAT replay as an official
gate. Stop before pushing, publishing, deleting artifacts, mutating remote
services, or staging generated large files.

## Immediate Next Gate

The immediate next gate is implementation Wave 1: post-flight skeleton with
precise statuses and hostile fixtures, without touching CUDA.
