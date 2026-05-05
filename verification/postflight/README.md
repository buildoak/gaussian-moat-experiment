# Post-Flight Checker

`src/postflight_check.cpp` is the independent post-flight runner for analytical
telemetry bundles. It reads one compact JSON bundle and emits a JSON report:

```bash
verification/build/postflight_check BUNDLE.json --report postflight.report.json
```

The emitted status is one of the statuses currently accepted by the runner:

- `REJECT`
- `RUN_CONTRACT_PASS`
- `TILE_SAMPLE_AUDIT_PASS`
- `SPAN_PROOF_PASS`
- `CLAIM_PROOF_MISSING`

The checker intentionally does not import campaign or CUDA implementation code.
It uses `verification/include/independent_moat.hpp` only for independent
Gaussian-prime, annulus, K-step, and geo-band checks on inline SPANNING
coordinate certificates.

## Bundle Contract

The bundle row declares the claim geometry and policy:

- `claim_id`, `k_sq`, `r_inner`, `r_outer`, `width`, and `region`.
- `verdict` plus `verdict_mode`; `SPANNING` must be `ANY-SPAN`, and `MOAT`
  must be `ANY-SHELL-MOAT`.
- `row_class`: `accepted`, `profile`, `sweep`, or `audit`.
- `telemetry_level`: `none`, `profile`, `audit`, or `full`.
- `claim_proof_required`: whether a detector-only pass should be reported as
  missing a claim proof.

Accepted rows require `audit` or `full` telemetry. Profile, sweep, and audit
rows require at least `profile` telemetry. These levels describe emitted
telemetry and artifacts; they do not by themselves prove a mathematical claim.

## Checks

Run-contract checks cover:

- row/profile/stdout/run-index agreement when present;
- width and full-octant contract;
- BZ-clean flags and zero overflow counters;
- MOAT full-ingest evidence;
- optional independently enumerated active tile-count agreement;
- build/hash identity presence;
- artifact table shape;
- telemetry-level sufficiency;
- sample-audit metadata;
- SPANNING coordinate certificate validity.

Sample audits use two budgets:

- `4096` sampled tiles minimum for `row_class=accepted`.
- `1024` sampled tiles minimum for non-accepted audit/profile/sweep bundles
  when `sample_audit` is present.

The sample manifest carries `target_tile_samples`, `sample_count`, class
counts, quotas, and population-exhaustion flags. The sample classes are
`geo_I`, `geo_O`, `axis`, `diagonal`, `high_pressure`, and
`deterministic_random`.

## Status Meaning

- `RUN_CONTRACT_PASS`: the bundle is internally coherent, but no sample audit
  or claim proof was accepted.
- `TILE_SAMPLE_AUDIT_PASS`: run contract passed and sampled production TileOps
  matched the independent checker.
- `SPAN_PROOF_PASS`: a SPANNING coordinate certificate passed.
- `CLAIM_PROOF_MISSING`: run and optional sample checks passed, but the row
  required a mathematical claim proof that is not built for that verdict.
- `REJECT`: a contract, artifact, BZ, overflow, sample, or certificate check
  failed.

## Normalized Rows

Pair post-flight reports with profiles through
`verification/stats/normalize_sweep_rows.py`. The resulting rows separate
`detector_status`, `postflight_status`, `run_contract_status`,
`tile_sample_audit_status`, and `proof_status`, and include analytical telemetry
fields such as geo-band counts, pressure distributions, component census,
sample artifact references, snapshot hashes, and timing fields.

Candidate-count and Gaussian-prime-count distributions are optional analytical
telemetry. Leave them null when the profile did not emit them.

## Orchestration Script

`postflight_orchestrate.py` builds compact bundles from campaign profile JSON,
runs the independent post-flight checker, runs `tile_sample_check` when sample
JSONL paths are present, and emits normalized sweep rows through
`verification/stats/normalize_sweep_rows.py`.

Example:

```bash
verification/postflight/postflight_orchestrate.py \
  --profiles RUN/profiles/R80015790_moat.profile.json \
  --out-dir RUN/postflight
```

Useful options:

- `--sample-manifest` and `--samples` override sample path inference.
- `--span-cert` adds an inline span certificate to the compact bundle.
- `--log` includes a JSON log/stdout artifact when available.
- `--fail-on-reject` makes the wrapper return non-zero on a `REJECT` report.

Outputs are one `*.postflight.bundle.json`, one `*.postflight.report.json`,
`postflight-summary.json`, and `sweep_rows.normalized.jsonl` unless
`--no-normalize` is used.

## Residual Limits

- A current MOAT bundle can pass run contract and sample audit, but it still
  reports missing claim proof when `claim_proof_required=true`.
- No independent negative-proof gate is built here.
- Compositor replay is not the official acceptance route for MOAT claims; it is
  reserved for forensic/debug work over emitted TileOps.

## Build Wiring

The target is already wired in `verification/CMakeLists.txt`:

```cmake
add_executable(postflight_check postflight/src/postflight_check.cpp)
target_link_libraries(postflight_check PRIVATE verification_common nlohmann_json::nlohmann_json)
```

The `no_campaign_includes` check covers `verification/postflight/src/*.cpp`.
