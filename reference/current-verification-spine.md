# Current Verification Spine

Updated: 2026-05-06 for the compact verification spine.

This board defines the current active gates for accepted static-annulus campaign
evidence. Long campaign ledgers and stale gate boards are provenance under
`reference/archive/`; they do not define current acceptance.

## Active Gates

The active gates are exactly:

| Gate | Fires | Answers |
|---|---|---|
| Exact Profile | For every accepted/profiled row. | Is the profile/run evidence full-octant, BZ-clean, overflow-clean, internally coherent, and backed by matching build/hash/artifact metadata? |
| Independent Tile Sample | For accepted/profiled rows with emitted samples. | Do deterministic emitted tile samples match independent TileOp regeneration? |
| SPANNING Cert | For every accepted `SPANNING`. | Is there an independently checkable Gaussian-prime coordinate path from `geo_I` to `geo_O`? |
| MOAT Hardening | For current `MOAT` rows. | Did the detector run full ingest with clean profile/sample evidence, while clearly remaining short of an independent negative certificate? |

Anything else is support, regression, telemetry, or provenance unless this file
is deliberately updated.

## Status Vocabulary

- `REJECT`: evidence cannot be used for the requested row class.
- `RUN_CONTRACT_PASS`: profile/run health is coherent, but no sample/proof
  artifact was accepted.
- `TILE_SAMPLE_AUDIT_PASS`: profile/run health plus sampled TileOp regeneration
  passed.
- `SPAN_PROOF_PASS`: SPANNING coordinate certificate passed.
- `CLAIM_PROOF_MISSING`: detector/audit evidence exists, but no accepted proof
  artifact exists for the claim.
- `MOAT_PROOF_PASS`: reserved for a future independent negative certificate;
  not currently reachable.

Current `MOAT` rows can reach `TILE_SAMPLE_AUDIT_PASS`; they are not independent
global negative proofs.

## Current MOAT Matrix

Current MOAT-hardening work uses the same inner radius and four shell widths:

| K scope | R_inner | Widths | Role |
|---|---:|---|---|
| K36 | `80,000,000` | `17k`, `18k`, `19k`, `20k`; optional `32,768` | Current MOAT hardening matrix. |
| K37-K39 | `80,000,000` | optional same widths | Boundary/BZ stress telemetry only. |

For a row at width `w`, `R_outer = R_inner + w`. Full ingest is mandatory for
`MOAT`; `SPANNING` rows still need the SPANNING Cert gate.

## Canonical Post-Flight Shape

Accepted/profile post-flight rows should use audit telemetry and persisted
sample/cert artifacts, for example:

```bash
./build-k36/campaign_main_cuda \
  --r-inner 80000000 \
  --r-outer 80015782 \
  --region full-octant \
  --chunk-size 200000 \
  --no-early-exit \
  --telemetry audit \
  --tile-sample-count 512 \
  --sample-manifest RUN/samples/span.manifest.json \
  --tile-sample-out RUN/samples/span.tiles.jsonl \
  --emit-span-cert RUN/certs/span.span-cert.json \
  --profile RUN/profiles/span.profile.json

verification/postflight/postflight_orchestrate.py \
  --profiles RUN/profiles/span.profile.json \
  --sample-manifest RUN/samples/span.manifest.json \
  --samples RUN/samples/span.tiles.jsonl \
  --span-cert RUN/certs/span.span-cert.json \
  --out-dir RUN/postflight \
  --fail-on-reject
```

For `MOAT`, omit `--emit-span-cert`; full ingest is mandatory and the result
remains hardened detector/audit evidence, not `MOAT_PROOF_PASS`.

Production post-flight sample audits normally use `512` deterministic
stratified samples. Larger counts are stress/checker benchmarks, not the
default gate.

## Demoted Surfaces

These remain useful, but are not claim-acceptance gates by themselves:

- C++ 5-tile golden and snapshot parity tests: local byte-format regression.
- `validate_campaign_run.py`: legacy local run validator.
- C++ and CUDA CTest: implementation sanity before code changes.
- Tsuchimura K36 adjacent pair: calibration note only; prefer the 17k+ matrix
  for current MOAT hardening evidence.
- `cuda_vs_cpu_diff --m4/--k5`: fault localization and CPU/CUDA drift checks.
- Snapshot SHA checks and CUDA/JSON goldens: forensic parity and cheap
  regression tripwires.
- K34 scripts: cross-K regression only, not Tsuchimura external truth.
- `exact_global_uf`: bounded oracle for small/medium tests, not campaign proof.
- Compositor or MOAT replay: debug over emitted TileOps, not TileOp
  faithfulness or claim acceptance.

## Telemetry Limits

K37-K39 telemetry is observation only until promoted by the active gates.

`stats_v2` currently includes timings, stage timings, tile counts, BZ/overflow
state, geo tile/port populations, group/port pressure distributions,
high-pressure tiles, sample paths, and live-frontier component census.

Known limits:

- Candidate-count and Gaussian-prime-count distributions may be `null`; the
  current emitted TileOp stream does not expose exact per-tile counts.
- Component census is live-frontier telemetry, not full historical component
  anatomy.
