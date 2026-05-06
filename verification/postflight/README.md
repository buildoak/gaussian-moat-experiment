# Post-Flight Checker

`postflight_check` reads one compact JSON bundle and emits a JSON report:

```bash
verification/build/postflight_check BUNDLE.json --report postflight.report.json
```

It intentionally does not import campaign or CUDA implementation code. Inline
SPANNING coordinate certificates are checked through
`verification/include/independent_moat.hpp`.

Post-flight owns the active Exact Profile, Independent Tile Sample, and SPANNING
Cert evidence paths. For `MOAT`, it supports the hardening gate only:
full-ingest, overflow-clean, BZ-clean detector evidence plus independent sample
audit. It does not produce `MOAT_PROOF_PASS`.

## Bundle Contract

The row declares:

- `claim_id`, `k_sq`, `r_inner`, `r_outer`, `width`, and `region`;
- `verdict` plus `verdict_mode`;
- `row_class`: `accepted`, `profile`, `sweep`, or `audit`;
- `telemetry_level`: `none`, `profile`, `audit`, or `full`;
- `claim_proof_required`.

Profile checks cover row/profile/stdout agreement when present, full-octant
shape, BZ cleanliness, zero overflow counters, `MOAT` full-ingest evidence,
build/hash identity, artifact table shape, telemetry sufficiency, sample-audit
metadata, and SPANNING coordinate certificate validity.

## Sample Audit

Production post-flight sample audits normally require `512` deterministic
stratified samples unless the manifest reports population exhaustion. Larger
sample counts are allowed for deliberate stress/checker benchmarks.

Sample classes are `geo_I`, `geo_O`, `axis`, `diagonal`, `high_pressure`, and
`deterministic_random`.

## Status Meaning

- `RUN_CONTRACT_PASS`: the bundle is internally coherent, but no sample audit or
  claim proof was accepted.
- `TILE_SAMPLE_AUDIT_PASS`: run contract passed and sampled production TileOps
  matched the independent checker.
- `SPAN_PROOF_PASS`: a SPANNING coordinate certificate passed.
- `CLAIM_PROOF_MISSING`: run and optional sample checks passed, but the row
  required a mathematical claim proof that is not built for that verdict.
- `REJECT`: a contract, artifact, BZ, overflow, sample, or certificate check
  failed.

## Orchestration

`postflight_orchestrate.py` builds compact bundles from campaign profile JSON,
runs `tile_sample_check` when sample JSONL paths are present, runs
`postflight_check`, and emits normalized sweep rows.

```bash
verification/postflight/postflight_orchestrate.py \
  --profiles RUN/profiles/R80015782_span.profile.json \
  --sample-manifest RUN/samples/span.manifest.json \
  --samples RUN/samples/span.tiles.jsonl \
  --span-cert RUN/certs/span.span-cert.json \
  --out-dir RUN/postflight \
  --fail-on-reject
```

Useful options:

- `--sample-manifest` and `--samples` override sample path inference.
- `--span-cert` adds an inline span certificate to the compact bundle.
- `--log` includes a JSON log/stdout artifact when available.
- `--fail-on-reject` makes the wrapper return non-zero on a `REJECT` report.

Outputs are one `*.postflight.bundle.json`, one `*.postflight.report.json`,
`postflight-summary.json`, and `sweep_rows.normalized.jsonl` unless
`--no-normalize` is used.

Residual limits:

- Current `MOAT` bundles can pass profile coherence and sample audit, but there
  is no independent negative-proof gate.
- Compositor replay is reserved for emitted-TileOp debugging.
