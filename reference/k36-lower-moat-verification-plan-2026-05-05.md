# K36 Lower Moat Verification Plan - 2026-05-05

## Purpose

Build the first paper-grade verification substrate for the current CUDA
static-annulus campaign, focused on the lower `K=36`, `W=32768` bracket found
below the K38 moat region.

This plan does not upgrade the result by itself. It defines the implementation
and evidence gates required before the lower-K36 bracket can be described as
substantially stronger than same-stack computational evidence.

## Target Rows

| Role | K | R_inner | R_outer | Current verdict | Current meaning |
|---|---:|---:|---:|---|---|
| Positive endpoint | 36 | 73,339,843 | 73,372,611 | `SPANNING` | `ANY-SPAN` in the tested static annulus |
| Negative endpoint | 36 | 73,359,375 | 73,392,143 | `MOAT` | `ANY-SHELL-MOAT` in the tested static annulus |

Current evidence pointers live in:

- `reference/2026-05-04-static-annulus-evidence-index.md`
- `reference/k36-below-k38-calibration-2026-05-04.md`

## Claim Boundary

The claim remains a static-annulus claim unless and until source/origin-connected
logic is implemented and accepted.

- `SPANNING` means some component connects the geometric inner and outer bands.
- Full-ingest `MOAT` means no component spans the tested static annulus.
- This plan does not claim an origin-component bound, a global threshold, or a
  full Tsuchimura-style source-connected upper-bound proof.
- Full K40 replay is explicitly out of scope for this wave.

## Build Layout

Add one independent verification surface:

```text
verification/
  README.md
  schemas/
    claim-row.schema.json
    bz-record.schema.json
    span-cert.schema.json
    sample-manifest.schema.json
    tile-sample.schema.json
    stats-profile-v2.schema.json
  exact-global-uf/
    CMakeLists.txt
    src/
    tests/
  bz-exact/
    src/
    tests/
  boundary-semantics/
    src/
    tests/
  span-cert/
    src/
    tests/
  production-tile-oracle/
    src/
    tests/
  compositor-replay/
    README.md
  stats/
    summarize_stats.py
    anatomy_report.py
  manifests/
    k36-lower-span-73339843-w32768.json
    k36-lower-moat-73359375-w32768.json
```

`compositor-replay/` is a reserved future surface in this wave, not an
acceptance gate. The current goal uses production tile sampling and bounded
global-UF, not full-row independent MOAT replay.

Production code touches should stay narrow:

```text
tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp
tiles-maxxing/cuda-campaign-v2-sqrt-36/src/host_driver.cpp
tiles-maxxing/cuda-campaign-v2-sqrt-36/src/streaming_compositor*.cpp
```

## Implementation Waves

1. Schema and spec lock.
   - Define stable row, BZ, certificate, sample, and stats schemas.
   - Document exact static-annulus semantics and accepted row metadata.

2. Exact BZ enforcement.
   - Add an exact integer/algebraic BZ checker.
   - Require production verdict profiles to record `BZ-clean: true`, or refuse
     accepted output unless an explicit uncertified override is used.

3. Boundary semantics suite.
   - Add direct tests for axis primes, the inclusive `y=x` diagonal, partial
     tile annulus clipping, `geo_I`, `geo_O`, collar width, and near-boundary
     integer comparisons.

4. Independent bounded global-UF verifier.
   - Implement a separate C++ verifier that enumerates Gaussian primes directly,
     builds the full graph for small/medium annuli, and runs global UF without
     reusing `Grid`, `TileOp`, campaign sieve, compositor, or CUDA code paths.
   - Target local/4090 runtime for the bounded suite: under 10 minutes.

5. SPANNING certificate checker.
   - Emit and independently verify coordinate path certificates for accepted
     `SPANNING` rows.
   - Check integer coordinates, annulus/octant inclusion, independent Gaussian
     primality, step lengths `<= K`, and endpoint membership in `geo_I`/`geo_O`.
   - Existing stitch traces are only tile/group/face-ordinal locators. They do
     not count as coordinate certificates unless materialized into a prime
     coordinate path and accepted by the independent checker.

6. Deterministic production-tile sampling oracle.
   - Emit sample manifests and sampled tile surfaces for the lower-K36
     `SPANNING` and `MOAT` rows.
   - Independently recompute sampled tile primes, local edges, local UF
     partitions, face strips, port partitions, canonical ordering, and boundary
     flags.
   - Sampling strengthens confidence in production TileOps; it is not a proof
     of the negative `MOAT` row.

7. Stats and Tsuchimura boundary anatomy.
   - Emit cheap profile-level distributions: candidate counts, active primes,
     ports, groups, face load, boundary flags, geo band population, component
     summaries, and high-pressure tile classes.
   - Explain whether the K36 moat transition is caused by real connectivity
     failure rather than `geo_O` thinning or boundary-band artifacts.

8. Same-commit gate-board preflight.
   - Re-run the relevant local/remote correctness gates on the commit that
     implements this wave. Historical gate-board evidence is not enough for new
     verification code and new campaign emission paths.

## 4090 Test Flow

Build on the CUDA host:

```bash
cd /workspace/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36
cmake -S . -B build-k36 \
  -DK_SQ=36 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-k36 -j"$(nproc)"
ctest --test-dir build-k36 --output-on-failure
```

Run the lower-K36 positive endpoint:

```bash
./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=73339843 \
  --r-outer=73372611 \
  --region full-octant \
  --chunk-size=200000 \
  --no-early-exit \
  --timing \
  --overflow-diagnostics \
  --profile /workspace/profiles/k36_lower_span.profile.json \
  --stats-level profile \
  --emit-span-cert /workspace/certs/k36_lower_span.cert.json \
  --sample-manifest /workspace/manifests/k36_lower_span.samples.json \
  --tile-sample-out /workspace/samples/k36_lower_span.tiles.jsonl
```

Run the lower-K36 negative endpoint:

```bash
./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=73359375 \
  --r-outer=73392143 \
  --region full-octant \
  --chunk-size=200000 \
  --no-early-exit \
  --timing \
  --overflow-diagnostics \
  --profile /workspace/profiles/k36_lower_moat.profile.json \
  --stats-level profile \
  --sample-manifest /workspace/manifests/k36_lower_moat.samples.json \
  --tile-sample-out /workspace/samples/k36_lower_moat.tiles.jsonl
```

Run independent verifiers:

```bash
verification/span-cert/build/span_cert_check \
  /workspace/certs/k36_lower_span.cert.json

verification/production-tile-oracle/build/tile_sample_check \
  --manifest /workspace/manifests/k36_lower_span.samples.json \
  --samples /workspace/samples/k36_lower_span.tiles.jsonl

verification/production-tile-oracle/build/tile_sample_check \
  --manifest /workspace/manifests/k36_lower_moat.samples.json \
  --samples /workspace/samples/k36_lower_moat.tiles.jsonl

verification/stats/anatomy_report.py \
  --profiles /workspace/profiles/k36_lower_span.profile.json \
             /workspace/profiles/k36_lower_moat.profile.json
```

## Acceptance For This Goal

This wave is accepted when:

- exact BZ enforcement is implemented and the lower-K36 rows are `BZ-clean`;
- same-commit local/remote gate-board preflight is rerun, including the
  Tsuchimura known-answer gate and CUDA CTest where available;
- boundary semantics tests pass locally and in CUDA-facing fixtures where
  available;
- bounded independent global-UF suite passes against TileOp verdicts;
- lower-K36 `SPANNING` path certificate validates independently;
- lower-K36 `SPANNING` and `MOAT` production tile samples validate
  independently;
- stats/anatomy report is written for the two lower-K36 rows;
- campaign profiles show zero overflow counters and no uncertified BZ override;
- the `SPANNING` endpoint is full-ingest if full-annulus samples or stats are
  claimed;
- code/docs are committed with an evidence report listing passed gates,
  non-paper-grade residual risks, and the next verification gate.
