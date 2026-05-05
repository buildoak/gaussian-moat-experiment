# Gaussian Moat Verification

This directory is the independent verification surface for current
static-annulus CUDA campaign claims.

The code here is intentionally separate from `tiles-maxxing/` campaign code.
It may read JSON artifacts emitted by the campaign, but it must not link
against campaign `Grid`, `TileOp`, sieve, compositor, CUDA kernels, or
validator implementation. Shared concepts are duplicated in small, auditable
forms so the checks attack different failure modes.

Current first target:

- `K=36`, `W=32768`
- `73,339,843..73,372,611 SPANNING`
- `73,359,375..73,392,143 MOAT`

## What Is Built

The current analytical telemetry/post-flight path has three pieces:

- **Analytical profile fields.** Campaign profiles can carry `stats_v2`
  telemetry for geo-band tile/port counts, TileOp pressure distributions,
  high-pressure tiles, component census, sample artifact paths, snapshot
  hashes, and timing fields. Old or lower-telemetry profiles may leave these
  fields null; the normalizer must preserve nulls rather than inventing data.
- **Post-flight runner.** `postflight_check` is wired into the verification
  CMake build. It reads a compact bundle, checks row/profile/stdout/run-index
  agreement, full-octant row shape, BZ cleanliness, zero overflow counters,
  build/hash identity, artifact table shape, optional active-tile count,
  sample-audit metadata, MOAT full-ingest evidence, and SPANNING coordinate
  certificates when present.
- **Normalized rows.** `stats/normalize_sweep_rows.py` converts profile JSON
  plus optional post-flight reports into `sweep_rows.jsonl` records with
  separated detector, post-flight, run-contract, tile-sample-audit, and proof
  statuses.

Post-flight status vocabulary separates detector evidence from mathematical
claim proof. A profile verdict records static-annulus detector output:
`SPANNING` is `ANY-SPAN`, and full-ingest `MOAT` is `ANY-SHELL-MOAT`. It is
not by itself a proof status. Current negative `MOAT` rows should remain
`CLAIM_PROOF_MISSING` unless a future independent negative proof gate is built
and accepted.

## Build

```bash
cmake -S verification -B verification/build
cmake --build verification/build -j
ctest --test-dir verification/build --output-on-failure
```

## Tools

| Tool | Role |
|---|---|
| `bz_exact_check` | Exact integer BZ check for square `K` values, currently used for K36 rows. |
| `boundary_semantics_test` | Standalone assertions for axis, diagonal, collars, clipping, and geo predicates. |
| `exact_global_uf` | Independent bounded global-UF oracle for small/medium annuli. |
| `tile_sample_check` | Independent production tile sample checker. |
| `span_cert_check` | Independent coordinate path certificate checker. |
| `postflight_check` | Compact bundle checker for run contract, sample audit, and SPANNING certificate status. |
| `stats/normalize_sweep_rows.py` | Converts profile/audit JSON into `sweep_rows.jsonl` rows with separate `detector_status` and `proof_status`. |
| `stats/anatomy_report.py` | Writes compact profile anatomy Markdown from profile JSON. |
| `stats/summarize_stats.py` | Prints quick profile stats for shell inspection. |

`tile_sample_check` strengthens confidence in sampled production TileOps. It is
not a global proof of a negative MOAT row.

## Audit Levels And Sample Budgets

Telemetry levels are `none`, `profile`, `audit`, and `full`.

- Accepted rows require `audit` or `full` telemetry in post-flight bundles.
- Profile, sweep, and audit rows require at least `profile` telemetry.
- A passing sample audit requires at least `4096` sampled tiles for
  `row_class=accepted`; non-accepted audit/profile/sweep rows use the
  exploratory `1024` minimum when a sample audit is present.
- Sample manifests record `target_tile_samples`, `sample_count`, class counts,
  and population-exhaustion flags. A sample audit is still sampled TileOp
  evidence, not a global negative proof.

## Sweep Rows

Use the stats normalizer when profiles or post-flight/audit reports need to be
folded into sweep tables:

```bash
python3 verification/stats/normalize_sweep_rows.py \
  --profiles /path/to/profile.json \
  --audits /path/to/postflight.report.json \
  --out sweep_rows.jsonl
```

`--audits` is optional, but when present it must contain one JSON file per
profile. Old profiles are preserved with nullable telemetry fields rather than
backfilled guesses. Profile-only rows default to detector evidence plus
`proof_status=CLAIM_PROOF_MISSING`; proof acceptance is recorded only from an
explicit post-flight/audit status such as `SPAN_PROOF_PASS`.

Normalized rows include claim geometry, verdict mode, detector/proof/post-flight
statuses, telemetry level, build/device identifiers, tile counts, early-exit
flags, BZ and overflow summaries, analytical distributions, sample/snapshot
paths, and timing fields. Candidate-count and Gaussian-prime-count
distributions are best-effort telemetry; they may be null when the source
profile did not emit them.

## Residual Limits

- Current `MOAT` rows are static-annulus detector evidence plus audit evidence,
  not a mathematical negative proof.
- Candidate and Gaussian-prime distributions can be unavailable in old,
  low-telemetry, or partially populated profiles.
- Compositor replay is a forensic/debug surface only; it is not the official
  post-flight acceptance spine.
