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
| `stats/normalize_sweep_rows.py` | Converts profile/audit JSON into `sweep_rows.jsonl` rows with separate `detector_status` and `proof_status`. |
| `stats/anatomy_report.py` | Writes compact profile anatomy Markdown from profile JSON. |
| `stats/summarize_stats.py` | Prints quick profile stats for shell inspection. |

`tile_sample_check` strengthens confidence in sampled production TileOps. It is
not a global proof of a negative MOAT row.

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
