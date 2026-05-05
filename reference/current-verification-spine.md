# Current Verification Spine

Updated: 2026-05-06 for the compact post-flight verification spine.

This board defines the current executable gates for accepted static-annulus
campaign evidence. Long campaign ledgers are archived under `reference/archive/`.

## Accepted Evidence Package

An accepted campaign row needs:

| Gate | Fires | Answers |
|---|---|---|
| Methodology alignment | Before changing grid, TileOp, port, stitching, boundary, or verdict logic. | Does the implementation still match the math canon? |
| Exact BZ/profile enforcement | For every accepted/profiled row. | Are `geo_I`/`geo_O` boundary semantics clean and the run metadata coherent? |
| Local C++ CTest | Before trusting local/reference changes. | Does the C++ implementation still satisfy its executable invariants? |
| CUDA CTest on 4090/CUDA host | Before trusting CUDA campaign changes. | Does CUDA still pass parity, geometry, and campaign smoke coverage on real hardware? |
| Run contract | For every post-flight bundle. | Did the row run full ingest when needed, with zero overflow counters and coherent profile/build/hash fields? |
| Tile-sample audit | For accepted/profiled post-flight rows with emitted samples. | Do independently regenerated sampled TileOps match the CUDA-emitted TileOps? |
| SPANNING certificate | For every accepted `SPANNING`. | Is there an independently checkable Gaussian-prime coordinate path from `geo_I` to `geo_O`? |
| `stats_v2` telemetry | For sweeps and accepted/profile rows. | What did the run observe about geo bands, pressure distributions, timings, samples, and component state? |

Production post-flight sample audits normally use `512` deterministic
stratified samples. Larger counts are stress/checker benchmarks, not the
default gate.

## Status Vocabulary

- `REJECT`: evidence cannot be used for the requested row class.
- `RUN_CONTRACT_PASS`: run health is coherent, but no sample/proof artifact was
  accepted.
- `TILE_SAMPLE_AUDIT_PASS`: run health plus sampled TileOp regeneration passed.
- `SPAN_PROOF_PASS`: SPANNING coordinate certificate passed.
- `CLAIM_PROOF_MISSING`: detector/audit evidence exists, but no accepted proof
  artifact exists for the claim.
- `MOAT_PROOF_PASS`: reserved for a future independent negative proof; not
  currently reachable.

Current `MOAT` rows can reach `TILE_SAMPLE_AUDIT_PASS`; they are not independent
global negative proofs. MOAT replay is demoted to forensic/debug work and is not
an official gate.

## Canonical Commands

Local C++:

```bash
cd tiles-maxxing/cpp-campaign-v2
cmake -S . -B build -DK_SQ=36 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
ctest --test-dir build --output-on-failure
```

Independent verification:

```bash
cmake -S verification -B verification/build
cmake --build verification/build -j
ctest --test-dir verification/build --output-on-failure
```

CUDA on a CUDA host:

```bash
cd /workspace/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36
cmake -S . -B build-k36 -DK_SQ=36 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-k36 -j"$(nproc)"
ctest --test-dir build-k36 --output-on-failure
```

Tsuchimura K36 known-answer sanity:

| R_inner | R_outer | Mode | Expected | Required |
|---:|---:|---|---|---|
| `80,000,000` | `80,015,782` | full ingest or sound SPANNING early exit | `SPANNING` | zero overflows |
| `80,000,000` | `80,015,790` | full ingest | `MOAT` | zero overflows |

Accepted/profile post-flight rows should run with audit telemetry and persisted
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

For `MOAT`, omit `--emit-span-cert`; full ingest is mandatory.

## Demoted Tools

These remain useful, but are not claim-acceptance gates by themselves:

- `cuda_vs_cpu_diff --m4/--k5`: fault localization and CPU/CUDA drift checks.
- Snapshot SHA gates: forensic parity and host-orchestration checks.
- CUDA/JSON goldens: cheap regression tripwires, never proof.
- K34 scripts: cross-K regression only, not Tsuchimura external truth.
- `exact_global_uf`: bounded oracle for small/medium tests, not campaign proof.
- Compositor/MOAT replay: debug over emitted TileOps, not TileOp faithfulness.

## Telemetry Limits

`stats_v2` currently includes timings, stage timings, tile counts, BZ/overflow
state, geo tile/port populations, group/port pressure distributions,
high-pressure tiles, sample paths, and live-frontier component census.

Known limits:

- Candidate-count and Gaussian-prime-count distributions may be `null`; the
  current emitted TileOp stream does not expose exact per-tile counts.
- Component census is live-frontier telemetry, not full historical component
  anatomy.
