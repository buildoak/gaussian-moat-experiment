# AGENTS.md - gaussian-moat-cuda

Read this before touching Gaussian Moat code. This repo is now in a compact
verification posture: methodology first, derived implementations second, old
campaign ledgers as provenance only.

## Authority

1. User direction in the current session.
2. `methodology/tile-operator-definition-v-claude.md` - strongest math canon.
3. `tiles-maxxing/` - current derived C++/CUDA implementation, evidence but
   not proof.
4. `verification/` - independent post-flight and stats verifier surface.
5. `reference/` - compact operational docs and archived evidence pointers.
6. `legacy/` and `reference/archive/` - prior art and provenance only.

If these disagree, follow the stronger layer and name the conflict. Do not let
old run ledgers, goldens, or generated profiles override methodology or current
independent verification.

## Active Surfaces

| Path | Role |
|---|---|
| `methodology/tile-operator-definition-v-claude.md` | TileOp/connectivity canon and proof obligations. |
| `tiles-maxxing/cpp-campaign-v2/` | C++ reference implementation used for local tests and parity work. |
| `tiles-maxxing/cuda-campaign-v2-sqrt-36/` | CUDA campaign implementation and 4090 execution surface. |
| `verification/` | Independent BZ, boundary, sample, span-cert, postflight, and stats checks. |
| `agents-directives/experiment-contract.md` | Operational contract for running/reporting experiments. |
| `reference/current-verification-spine.md` | Compact gate spine and status vocabulary. |
| `reference/archive/` | Historical evidence and implemented plans; not current authority. |

Do not recreate root-level `docs/`, `artifacts/`, `results/`, or old campaign
folders as authority surfaces. New durable operational docs go in `reference/`;
math changes start in `methodology/`; generated evidence stays out of git unless
explicitly accepted.

## Current Claim Semantics

Current campaign verdicts are static-annulus detector results:

- `SPANNING` means `ANY-SPAN`: some component connects `geo_I` to `geo_O`
  inside the tested static annulus.
- Full-ingest `MOAT` means `ANY-SHELL-MOAT`: no component connects `geo_I`
  to `geo_O` inside the tested static annulus.
- `SOURCE-SPAN`, `WIRED-SPAN`, `ORIGIN-SPAN`, exact global thresholds, and
  Tsuchimura-style origin-component bounds are not implemented claim modes.

Do not report current K38/K40 or sweep rows as origin-component moat proofs.
Current `MOAT` rows are detector evidence plus audit evidence unless a future
independent negative proof is built.

## Compact Verification Spine

Accepted campaign evidence is now this smaller set:

1. **Exact Profile Gate.** Accepted/profile/audit rows must be full-octant,
   exact on `K`, `R_inner`, `R_outer`, and width, BZ-clean with no override,
   zero-overflow with strict counter shapes, backed by build/hash identity, and
   carrying real `stats_v2` when telemetry claims `profile`, `audit`, or `full`.
2. **Independent Tile Sample Gate.** CUDA emits deterministic stratified tile
   samples; independent C++ post-flight code regenerates and compares them.
   Production sample audits use `512` manifested samples. Sample JSONL without a
   manifest is debug only and cannot satisfy acceptance.
3. **SPANNING Cert Gate.** Every accepted `SPANNING` must carry an independently
   checkable Gaussian-prime coordinate path from `geo_I` to `geo_O`. This is a
   hard post-flight requirement, not an optional flag.
4. **MOAT Hardening Gate.** Current `MOAT` rows require full ingest plus clean
   Exact Profile and Independent Tile Sample evidence. `MOAT_PROOF_PASS` remains
   reserved for a future independent negative certificate.

No MOAT replay is part of the official spine. Replaying emitted TileOps through
another compositor mostly checks the emitted surface, not TileOp mathematical
faithfulness. Keep it forensic/debug only.

Status vocabulary:

- `REJECT`: contract, artifact, BZ, overflow, sample, or certificate check failed.
- `RUN_CONTRACT_PASS`: coherent run, no accepted sample/proof.
- `TILE_SAMPLE_AUDIT_PASS`: run contract plus sampled TileOp regeneration passed.
- `SPAN_PROOF_PASS`: SPANNING coordinate certificate passed.
- `CLAIM_PROOF_MISSING`: detector/sample evidence exists but no accepted proof
  artifact for the claim.
- `MOAT_PROOF_PASS`: reserved; no executable path currently produces it.

## External Truth And Regression

The strongest current K36 executable hardening surface is the full-ingest
matrix anchored at `R_inner=80,000,000`, especially widths `17k`, `18k`, `19k`,
and `20k`, with a wider `32,768` confirmation row when run time permits. These
rows must pass exact BZ, zero overflow, stats_v2 telemetry, and persisted
512-tile postflight sample audit.

Tsuchimura's adjacent K36 pair is a calibration note, not the primary gate:

| R_inner | R_outer | Expected | Role |
|---:|---:|---|---|
| `80,000,000` | `80,015,782` | `SPANNING` | SPANNING cert sanity. |
| `80,000,000` | `80,015,790` | `MOAT` | Boundary-adjacent calibration. |

This is implementation-level evidence, not a proof source. K34 is not an
external annular truth gate; Tsuchimura's K34 result is about the
origin-connected component. K34 scripts may be used for cross-K regression only.

CPU/CUDA diff probes, snapshot SHA checks, exact bounded UF, and goldens are
useful development/localization tools. They do not outrank the compact spine.

If a narrower K36 matrix width is `MOAT`, a wider width becoming `SPANNING` is a
monotonicity violation requiring investigation. K37-K39 runs are boundary/BZ
stress telemetry only; they add no meaningful graph edges over K36.

## Experiment Defaults

- Use explicit `--r-inner`, `--r-outer`, and `--region full-octant`.
- The default shell-probe convention is `R_outer = R_inner + 8192`, full-octant.
- `SPANNING` may early-exit after a column-complete witness is latched.
- `MOAT` cannot early-exit; it requires full ingest.
- CUDA performance runs must be isolated on the GPU and report chunk size,
  produced/ingested tiles, total time, CUDA K1-K5 time, compositor time, and
  early-exit state.

## Implementation Rules

- Read the methodology before changing grid, TileOp, port, stitching, boundary,
  or verdict logic.
- Preserve closed tile boundaries: side `S` contains `S+1` lattice points.
- Preserve snapped-grid assumptions unless methodology changes first.
- Preserve deterministic face/port ordering and byte-stable semantics where
  tests expect them.
- Keep verification code independent from campaign/CUDA implementation imports.
- Do not promote generated CUDA goldens or old ledgers into proof authority.

## Git And Compute

- `_archive/` is local-only and ignored. `reference/archive/` is tracked
  historical documentation.
- Do not add generated binaries, build directories, profiles, snapshots, `.bin`,
  `.gprf`, `target/`, or `census_output/`.
- Goldens under `tiles-maxxing/cpp-campaign-v2/goldens/*.bin` are intentionally
  tracked.
- Inspect `git status --short --untracked-files=all` before staging; prefer
  exact-path staging.
- The historical pre-push and heavy-history runbooks live under
  `reference/archive/`; use them only when that specific operation is requested.
- The Mac Mini has no CUDA compiler. CUDA build/run work happens on remote CUDA
  hosts such as Vast; long remote jobs should run in `tmux`.
- Do not destroy cloud instances, push branches, publish results, or mutate
  remote services unless explicitly asked.
