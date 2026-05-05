# Post-Flight Verification And Telemetry Plan - 2026-05-05

## Purpose

Replace the current verification sprawl with a small post-flight spine that is
stronger, easier to explain, and closer to paper-grade evidence.

The goal is not more gates. The goal is fewer gates with clearer authority:

1. build sanity before runs;
2. hard post-flight run contract after runs;
3. claim-specific proof for `SPANNING` or `MOAT`;
4. first-class telemetry for prediction and anomaly understanding.

Post-flight reports must use explicit status vocabulary:

- `REJECT` - evidence is invalid for the requested row class.
- `RUN_CONTRACT_PASS` - bundle/run health is coherent, but no mathematical
  claim proof has been accepted.
- `SPAN_PROOF_PASS` - `SPANNING` coordinate certificate passed.
- `MOAT_SNAPSHOT_REPLAY_PASS` - emitted TileOps independently replay to
  `MOAT`.
- `MOAT_PROOF_PASS` - reserved for a future stronger negative proof that also
  closes TileOp mathematical faithfulness beyond sampled audits.
- `CLAIM_PROOF_MISSING` - run health passed, but the required proof artifact
  is absent or not implemented.

Only `SPAN_PROOF_PASS` and future `MOAT_PROOF_PASS` should be described as
claim-proof acceptance. `MOAT_SNAPSHOT_REPLAY_PASS` is strong negative evidence
over the emitted TileOp surface, not by itself a full mathematical proof that
every emitted TileOp is correct.

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
- `MOAT` replay should use a full TileOp snapshot first, while explicitly
  tracking artifact size. Near `R ~= 1B`, full snapshots require an explicit
  artifact budget and streaming replay.
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
- accepted `MOAT` without required snapshot/replay artifacts once replay is
  implemented;
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
- optional TileOp snapshot and snapshot manifest.

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

For every accepted `MOAT` claim once replay exists:

1. The run must be full ingest.
2. CUDA/campaign emits a full TileOp snapshot and manifest.
3. Post-flight independently parses the snapshot.
4. Post-flight independently replays the TileOp port graph.
5. The row is accepted only if no replay component carries both `geo_I` and
   `geo_O` reach.

The replay checker must verify:

- exact file size `120 + tile_count * 256`;
- snapshot header magic, version, bytes-per-tile, and reserved padding;
- snapshot manifest/header hash agreement;
- payload hash;
- tile count and active tile order;
- no overflow bits;
- every TileOp structural invariant;
- allowed `tile_flags` only;
- zero reserved bytes;
- zero padding past `sum(n)`;
- `sum(n) <= 192`;
- active labels are in `1..128`;
- canonical empty/overflow encodings;
- port count equality on shared faces;
- group-label validity;
- I/O and L/R bridge replay;
- final component reach census;
- `I&O` component count is zero for `MOAT`.

Tile samples alone are not a `MOAT` proof. Snapshot replay alone proves the
emitted TileOp surface composes to `MOAT`; it still depends on the TileOps being
faithfully emitted. For now, the accepted negative status after replay is
`MOAT_SNAPSHOT_REPLAY_PASS`. Promotion to `MOAT_PROOF_PASS` requires either
full independent TileOp regeneration plus replay or another explicitly accepted
negative-proof strategy.

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
  - full TileOp snapshot;
  - replay manifest;
  - replay output;
  - component census.

`full` is not for broad sweeps unless explicitly budgeted.
For `MOAT`, `full` currently enables `MOAT_SNAPSHOT_REPLAY_PASS`, not
`MOAT_PROOF_PASS`.

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

## MOAT Artifact Size

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

Full snapshot/replay is acceptable for lower K36 and roughly `R ~= 100M`.
Near `R ~= 1B`, it needs explicit artifact budget, local-disk planning,
streaming replay, and likely compression/summary strategy.

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
  `CLAIM_PROOF_MISSING` or `RUN_CONTRACT_PASS` for MOAT until replay exists;
- hostile fixtures hard reject;
- no campaign/cuda imports in post-flight code.

### Wave 2 - Telemetry Level Enum

Add `--telemetry=none|profile|audit|full`.

Acceptance:

- `profile` preserves existing profile fields and adds stable telemetry level;
- `audit` and `full` are accepted only with their required artifacts;
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

### Wave 5 - MOAT Replay

Implement streaming independent replay over full TileOp snapshots.

Acceptance:

- lower-K36 MOAT full snapshot replays to `MOAT_SNAPSHOT_REPLAY_PASS`;
- injected snapshot mutation that creates an `I&O` bridge hard rejects;
- injected malformed TileOp/port-count/group-label cases hard reject;
- replay can run streaming without holding a 100 GB snapshot in memory.

### Wave 6 - Docs And Gate Board Cleanup

Update repo references so post-flight is the official spine.

Acceptance:

- `reference/README.md` points to this plan;
- old gate-board language is marked superseded or scoped to historical/dev
  sanity;
- lower-K36 evidence report names the remaining MOAT replay gap precisely.

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

### Worker W5A - Snapshot Parser

**Scope:** independent `verification/postflight/snapshot_*`, parser fixtures,
CMake/tests.

**Non-goals:** no connectivity replay yet.

**Gate:** valid tiny snapshot parses; malformed header, length, schema,
overflow, reserved bytes, padding, and group-label fixtures reject.

### Worker W5B - MOAT Replay

**Scope:** independent `verification/postflight/moat_replay_*`, replay tests,
fixtures, post-flight `full` integration.

**Non-goals:** no CUDA optimization, no compression strategy beyond streaming
read.

**Gate:** lower-K36 MOAT snapshot reaches `MOAT_SNAPSHOT_REPLAY_PASS`; injected
`I&O` bridge rejects; replay never requires loading a large snapshot into
memory.

### Worker W6 - Docs Cleanup

**Scope:** `reference/README.md`, `reference/current-gate-board.md`,
`verification/README.md`, lower-K36 evidence note, plan references.

**Non-goals:** no code, no tickets.

**Gate:** search finds no misleading “official acceptance spine” language for
deprecated gates; docs do not claim `MOAT_PROOF_PASS` until the stronger
negative proof exists.

## Global Stop Rules

Stop if a worker needs to change methodology semantics, claim meanings, or
accepted row definitions. Stop if post-flight links against campaign/CUDA code.
Stop if CUDA telemetry adds overhead outside the accepted `5-10%`
profile/audit budget. Stop if MOAT replay needs non-streaming full-snapshot
memory. Stop before pushing, publishing, deleting artifacts, mutating remote
services, or staging generated large files.

## Immediate Next Gate

The immediate next gate is implementation Wave 1: post-flight skeleton with
precise statuses and hostile fixtures, without touching CUDA.
