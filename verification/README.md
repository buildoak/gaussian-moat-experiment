# Gaussian Moat Verification

`verification/` is the independent checker surface for current static-annulus
campaign evidence. It reads campaign artifacts, but it must not link against or
import campaign `Grid`, `TileOp`, sieve, compositor, CUDA kernels, or validator
implementations.

The active compact spine is exactly:

| Gate | Accepted evidence |
|---|---|
| Exact Profile | Profile/run metadata is internally coherent, BZ-clean, full-octant, and overflow-clean. |
| Independent Tile Sample | Deterministic emitted tile samples match independent regeneration. |
| SPANNING Cert | Accepted `SPANNING` rows carry an independently checkable coordinate certificate. |
| MOAT Hardening | Current `MOAT` rows are hardened detector/audit evidence only; `MOAT_PROOF_PASS` is reserved for a future negative certificate. |

C++ 5-tile goldens, campaign-run validators, exact bounded UF, and compositor or
MOAT replay remain useful regression/debug tools outside the compact spine.

## Build

```bash
cmake -S verification -B verification/build
cmake --build verification/build -j
ctest --test-dir verification/build --output-on-failure
```

## Tools

| Tool | Role |
|---|---|
| `bz_exact_check` | Exact integer BZ check for square `K` values, currently K36. |
| `boundary_semantics_test` | Axis, diagonal, collar, clipping, and geo predicate assertions. |
| `tile_sample_check` | Independent production tile-sample checker. |
| `span_cert_check` | Independent Gaussian-prime coordinate path checker. |
| `postflight_check` | Compact bundle checker for profile coherence, sample audit, and SPANNING cert status. |
| `stats/normalize_sweep_rows.py` | Normalizes profiles and post-flight reports into sweep rows. |
| `stats/summarize_stats.py` | Quick shell summary for `stats_v2` profiles/rows. |
| `exact_global_uf` | Bounded oracle for small/medium annuli; not a campaign gate. |

`tile_sample_check` strengthens confidence in sampled production TileOps. It is
not a global proof of a negative `MOAT` row.

## Status Semantics

Profile verdicts are detector output:

- `SPANNING` means `ANY-SPAN`.
- Full-ingest `MOAT` means `ANY-SHELL-MOAT`.

Post-flight statuses are separate:

- `REJECT`
- `RUN_CONTRACT_PASS`
- `TILE_SAMPLE_AUDIT_PASS`
- `SPAN_PROOF_PASS`
- `CLAIM_PROOF_MISSING`

`MOAT_PROOF_PASS` is reserved for future work. Current negative `MOAT` rows can
pass profile coherence and sample audit, but remain detector/audit evidence
rather than independent global negative proof.

## Sample Policy

Telemetry levels are `none`, `profile`, `audit`, and `full`.

- Accepted/profile post-flight rows should use `audit` or `full`.
- Production sample audits normally use `512` deterministic stratified samples.
- Larger sample counts are stress/checker benchmarks and should be named as
  such.
- Sample manifests record `target_tile_samples`, `sample_count`, class counts,
  quotas, and population-exhaustion flags.

Sample classes are `geo_I`, `geo_O`, `axis`, `diagonal`, `high_pressure`, and
`deterministic_random`.

## Normalized Rows

Use the stats normalizer when profiles or post-flight reports need to be folded
into sweep tables:

```bash
python3 verification/stats/normalize_sweep_rows.py \
  --profiles /path/to/profile.json \
  --audits /path/to/postflight.report.json \
  --out sweep_rows.jsonl
```

Normalized rows preserve nullable telemetry instead of inventing data. They
include claim geometry, verdict mode, detector/proof/post-flight statuses,
telemetry level, build/device identifiers, tile counts, early-exit flags, BZ and
overflow summaries, analytical distributions, sample/snapshot paths, and timing
fields.

Known limits:

- Candidate-count and Gaussian-prime-count distributions may be null.
- Component census is currently live-frontier telemetry, not full historical
  component anatomy.
- Compositor/MOAT replay is forensic/debug only, not the official post-flight
  spine.
