# K36 Lower Moat Verification Evidence - 2026-05-05

## Scope

This report records the first verification-substrate hardening wave for the
lower `K=36`, `W=32768` static-annulus bracket:

| Role | R_inner | R_outer | Verdict | Meaning |
|---|---:|---:|---|---|
| Positive endpoint | 73,339,843 | 73,372,611 | `SPANNING` | `ANY-SPAN` in the tested static annulus |
| Negative endpoint | 73,359,375 | 73,392,143 | `MOAT` | `ANY-SHELL-MOAT` in the tested static annulus |

This remains a static-annulus claim. It is not an origin-component claim, not a
source-connected Tsuchimura-style upper bound, and not an exact global threshold.

## Durable Evidence

Local mirror:

```text
/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-05-k36-lower-verification
```

Remote source:

```text
/workspace/k36-lower-verification-20260505
```

Hardware:

- Vast instance `36021982`
- NVIDIA RTX 4090
- driver `560.35.03`
- CUDA arch build `89`

## Implemented Gates

| Gate | Status | Evidence |
|---|---|---|
| Schema/spec lock | PASS | `verification/schemas/*.schema.json`; `verification/manifests/k36-lower-*.json`; plan updated in `reference/k36-lower-moat-verification-plan-2026-05-05.md` |
| Exact BZ enforcement | PASS for K36 square-K rows | `campaign_main_cuda` now embeds `bz.checked`, `bz.clean`, `bz.override_used`, candidate counts, and bad norm count in profiles; lower rows have `clean=true`, `override_used=false`, `bad_norm_count=0` |
| Boundary semantics suite | PASS | local and remote `verification` CTest `5/5`; includes collar, axis, diagonal, partial clipping, geo predicates |
| Independent bounded global-UF | PASS bounded smoke | `exact_global_uf --k-sq 36 --r-inner 100 --r-outer 500 --expect SPANNING`; matched CPU campaign verdict for same tiny annulus |
| SPANNING certificate checker | PASS bounded smoke / BLOCKED at lower production row | `--emit-span-cert` now materializes a coordinate certificate from a reconstructed stitch path; remote small annulus `100..500` emitted `/tmp/small.cert.json`, and independent `span_cert_check` accepted it with `points=81`. The lower `73,339,843..73,372,611` row did not finish certificate-path tracing in bounded attempts and has no accepted coordinate certificate yet. |
| Same-commit CUDA preflight | PASS | remote verification CTest `5/5`; remote CUDA CTest `13/13`; after certificate/compositor edits, local C++ CTest `115/115` and remote CUDA CTest `13/13` still passed |
| Lower-K36 production reruns | PASS | full-ingest `--no-early-exit` runs for both rows; produced = ingested = active; zero overflows |
| Production tile-sample oracle | PASS | independent checker accepted `1024` sampled tiles for SPANNING and `1024` for MOAT, comparing semantic TileOp signatures modulo label renaming |
| Stats/anatomy report | PASS | `reports/k36_lower_anatomy.md` written; `geo_I`/`geo_O` tile populations are healthy on both rows |
| Bundle validator | PASS with explicit residual | `validate_campaign_run.py --require-profile-bz --allow-spanning-without-path --no-recompute-bz` accepts the mirrored bundle; the explicit allowance remains necessary until the lower SPANNING row has a coordinate certificate |

## Production Run Results

| Row | Active | Produced | Ingested | Verdict | Early exit | Overflows | BZ | Total |
|---|---:|---:|---:|---|---|---:|---|---:|
| `k36_lower_span` | 29,093,487 | 29,093,487 | 29,093,487 | `SPANNING` | disabled / not taken | 0 | clean, no override | 353.777 s |
| `k36_lower_moat` | 29,101,312 | 29,101,312 | 29,101,312 | `MOAT` | disabled / not taken | 0 | clean, no override | 354.268 s |

Profile stats:

| Row | geo_I tiles | geo_O tiles | tile samples |
|---|---:|---:|---:|
| `k36_lower_span` | 295,793 | 295,775 | 1,024 |
| `k36_lower_moat` | 295,920 | 296,184 | 1,024 |

The `geo_O` population did not thin away at the MOAT endpoint; at this level of
telemetry, the transition is consistent with a connectivity failure rather than
a missing outer boundary band. This is explanatory evidence, not a causality
proof.

## Validation Commands

Local verification:

```bash
cmake -S verification -B verification/build
cmake --build verification/build -j
ctest --test-dir verification/build --output-on-failure
python3 tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/validate_campaign_run.py \
  /Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-05-k36-lower-verification \
  --expected-k 36 \
  --require-profile-bz \
  --allow-spanning-without-path \
  --no-recompute-bz
```

Remote build/preflight:

```bash
cmake -S verification -B verification/build
cmake --build verification/build -j"$(nproc)"
ctest --test-dir verification/build --output-on-failure

cd tiles-maxxing/cuda-campaign-v2-sqrt-36
cmake -S . -B build-k36 -DK_SQ=36 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-k36 -j"$(nproc)"
ctest --test-dir build-k36 --output-on-failure
```

Certificate smoke test:

```bash
./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner 100 \
  --r-outer 500 \
  --region full-octant \
  --timing \
  --chunk-size 64 \
  --trace-spanning-path \
  --emit-span-cert /tmp/small.cert.json \
  --profile /tmp/small.cert.profile.json

/workspace/gaussian-moat-cuda/verification/build/span_cert_check \
  /tmp/small.cert.json
```

Result:

```text
VERDICT: SPANNING
SPANNING_PATH: reconstructed=1 recorded_edges=1 inner_path_edges=0 outer_path_edges=0
span certificate PASS: points=81
```

Remote endpoint runs:

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
  --profile /workspace/k36-lower-verification-20260505/profiles/k36_lower_span.profile.json \
  --stats-level profile \
  --sample-manifest /workspace/k36-lower-verification-20260505/manifests/k36-lower-span-73339843-w32768.json \
  --tile-sample-out /workspace/k36-lower-verification-20260505/samples/k36_lower_span.tiles.jsonl

./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=73359375 \
  --r-outer=73392143 \
  --region full-octant \
  --chunk-size=200000 \
  --no-early-exit \
  --timing \
  --overflow-diagnostics \
  --profile /workspace/k36-lower-verification-20260505/profiles/k36_lower_moat.profile.json \
  --stats-level profile \
  --sample-manifest /workspace/k36-lower-verification-20260505/manifests/k36-lower-moat-73359375-w32768.json \
  --tile-sample-out /workspace/k36-lower-verification-20260505/samples/k36_lower_moat.tiles.jsonl
```

## Artifact Hashes

```text
c568d3359147919cf93c5c50a3fa046076b9af3179b174c4e105b1852131890d  profiles/k36_lower_span.profile.json
f8c5ee6de57ae9dc042120c7fea06c8c3b7f7333ebe2dc2daf2dcda5db114bbb  profiles/k36_lower_moat.profile.json
ecb2f37c7e518ebbb433b1f5c1b734c2adccf84dc9379f4681f40f2dbbb1072e  samples/k36_lower_span.tiles.jsonl
77837f0d721a6d21d3f5febe4d24e04c6f523f9a26925a2641ffbf0a31ddeee0  samples/k36_lower_moat.tiles.jsonl
3c65414989269f653b80ece89d1b1b2b01cf518acaa000ee0899c5996b6792a4  run-index.tsv
d2412d702107150e4f382848d19a269a4262f159e02198f40402806ff9558406  reports/k36_lower_anatomy.md
a7777563db6747fee26b069d0857dfbd63968dc915aeaa8d3a0dd612bae3e33d  logs/build-and-ctest.log
09cf13ae3c4278fa88204e7d7d50d5b46fb4d4b3b172ce740f64fe752bd424ea  logs/post-verifiers.log
095375cc1d92bd172a7eb97da4473306a9552a05f5bc1084f4ecd53e6d639c7e  certs/k36_small_smoke.cert.json
75f34f8b72f7f95d998ec2c16c521cfb9c86ddb6742d7381f0ba5602d5125323  profiles/k36_small_smoke.profile.json
f684f38ceb204d0e59889e918696d6ce41c4132629379126303d182c233b03a9  logs/k36_small_smoke.cert.log
320f889d1e66f6c85553e3a63d4f3811df6a89aa33f58129350139548ba1dada  logs/k36_small_smoke.cert_check.log
9af2140dead37f9193268e6f518bf59cd5133e0d63957c194e3eb4379e99c0c6  logs/local-cpp-ctest-after-cert.log
147cbda953ce33d45b3f51dc0f2eb93a27b02e43b169d5ca1d13afbf290b3fea  logs/k36_final_build_after_cert_help.log
4fb3df9be7ed98a016836f9cdb0f3cbea859e353ecf20eeb20b6fd86caab9286  logs/k36_final_cuda_ctest_after_cert_help.log
```

## Residual Gaps

- Coordinate SPANNING certificate emission is implemented and passes a bounded
  remote smoke test, but the lower-K36 production SPANNING row still has no
  accepted coordinate certificate. With `--trace-spanning-path`, the production
  run remained CPU-bound in stitch tracing/materialization attempts and was
  stopped without emitting a certificate.
- Therefore this wave does not satisfy the plan's full acceptance condition
  requiring an independently validated lower-K36 SPANNING coordinate path.
- Production tile sampling strengthens local TileOp confidence but does not
  prove the negative MOAT row globally.
- Full independent MOAT compositor replay is reserved for a later wave in
  `verification/compositor-replay/`.
- The bounded global-UF oracle currently covers small/medium annuli only. It
  does not run at the 73M lower-K36 production radius.
- Local C++ CTest after the compositor/certificate edits passed `115/115`
  with the expected two skipped debug/non-square cases.

## Next Gate

Make lower-production coordinate path certification fast enough to accept:

1. Replace production `--trace-spanning-path` with an incremental predecessor
   certificate mode that does not record/search the full stitch graph.
2. Emit the lower `73,339,843..73,372,611` coordinate certificate.
3. Validate it with `verification/span-cert/span_cert_check`.
4. Only then remove `--allow-spanning-without-path` from the bundle validator
   acceptance command for SPANNING rows.
