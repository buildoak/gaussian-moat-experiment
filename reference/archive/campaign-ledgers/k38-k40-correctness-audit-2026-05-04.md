# K38/K40 Correctness Audit - 2026-05-04

Scope: local static-annulus detector correctness for the K38/K40 campaign.
These results are not origin-component verifier results and must not be reported
as origin-component moat proofs.

## Current Judgment

The K38/K40 campaign results are credible as local static-annulus detector
evidence, but they are not yet publication-grade mathematical claims.

For K38, the suspicious width-32768 bracket survived the strongest immediate
checks available during this audit:

- `K=38, R_inner=71,875,000, R_outer=71,907,768`: `SPANNING`
- `K=38, R_inner=73,437,500, R_outer=73,470,268`: `MOAT`

The main comparison worry, "K38 looks lower than K36", is not a direct
contradiction. The previous K36 result is a fixed-radius/variable-width ladder;
the K38 bracket is a fixed-width/variable-radius bracket. Same-annulus
monotonicity is the correct falsifier.

## New Diagnostic Evidence

Pulled local evidence mirror:

- `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results/remote/k38-bracket-diag-20260504T163900Z`
- `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results/remote/k36-same-annulus-control-20260504T165239Z`

Remote K38 diagnostic tag:

- `/workspace/k38-bracket-diag-20260504T163900Z`

Rows:

| K | R_inner | width | verdict | produced | ingested | early exit | overflow counters | note |
| ---: | ---: | ---: | --- | ---: | ---: | --- | ---: | --- |
| 38 | 71,875,000 | 32,768 | SPANNING | 28,512,666 | 28,512,666 | disabled | 0 | reconstructed stitch path |
| 38 | 73,437,500 | 32,768 | MOAT | 29,132,322 | 29,132,322 | disabled | 0 | no spanning trace |

K38 SPANNING path summary:

- `SPANNING_TRACE detected=1`
- final event: `bridge_lr`
- final bridge: `459215/42:R#19` to `459344/4:L#19`
- inner source: `452307/2`
- outer source: `455273/1`
- path reconstruction: `reconstructed=1`
- inner path edges: `211`
- outer path edges: `184`
- recorded stitch edges: `16,987,808`

K38 MOAT diagnostic summary:

- `active = produced = ingested = 29,132,322`
- `early_exit_enabled=false`
- `early_exit_taken=false`
- all CUDA overflow counters are `0`
- `SPANNING_TRACE detected=false`

## Same-Annulus Monotonicity Sentinels

For the exact annulus where K38 reports `MOAT`, K36 also reports `MOAT`:

| K | R_inner | width | verdict | produced | ingested | early exit | overflow counters |
| ---: | ---: | ---: | --- | ---: | ---: | --- | ---: |
| 36 | 73,437,500 | 32,768 | MOAT | 29,132,322 | 29,132,322 | disabled | 0 |

This passes the key monotonicity implication:

- if `K38` is `MOAT` on an annulus, `K36` must also be `MOAT` on that same
  annulus.

For the exact annulus where K38 reports `SPANNING`, K40 also reports
`SPANNING`:

| K | R_inner | width | verdict | produced | ingested | early exit | overflow counters |
| ---: | ---: | ---: | --- | ---: | ---: | --- | ---: |
| 40 | 71,875,000 | 32,768 | SPANNING | 28,512,666 | 28,512,666 | disabled | 0 |

Pulled local evidence mirror:

- `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results/remote/k40-same-annulus-control-20260504T170020Z`

This passes the paired monotonicity implication:

- if `K38` is `SPANNING` on an annulus, `K40` must also be `SPANNING` on that
  same annulus.

## Audit Findings

Independent read-only workers audited:

- math/spec assumptions
- evidence/log consistency
- BZ and annulus conventions
- streaming compositor/finalization
- CUDA silent-corruption risks
- oracle/certificate strategy

No worker found a concrete current-scale bug explaining a false K38 `MOAT`.

Positive evidence:

- Tile collar for non-square K is `C = floor(sqrt(K))`; for K38/K40 this is
  `6`. `ceil(sqrt(K)) = 7` is only the conservative prefilter radius.
- K38/K40 CUDA constants assert the expected non-square K constants.
- Accepted MOAT rows are full-ingest rows, not early-exit rows.
- Early exit can only return `SPANNING`; `MOAT` comes from compositor
  finalization.
- The compositor refuses `MOAT` until all columns are ingested.
- K38/K40 accepted rows are BZ-clean and overflow-clean.
- Annulus convention is closed/inclusive:
  `R_inner^2 <= x^2 + y^2 <= R_outer^2`.

Residual risks:

- BZ is enforced by wrapper/report discipline, not by `campaign_main_cuda`
  itself.
- The face encode kernel assumed one warp per face. A compile-time guard was
  added after this audit to prevent future under-threaded builds.
- Current radii are safe from `int32_t` coordinate truncation, but future
  searches approaching the 2.1B coordinate scale need integer-width hardening.
- MR/K4 signed arithmetic is safe for current K38/K40 radii but should be
  audited before treating the full `R_outer < 2^32` envelope as supported.
- A publication-grade claim still needs an independent certificate/checker
  layer, not only CPU/CUDA implementation agreement.

## Confidence Ladder

What is strongly supported now:

- K38/K40 local static-annulus detector rows are internally consistent.
- The K38 low bracket is not falsified by K36 same-annulus monotonicity at the
  MOAT endpoint.
- The K38 MOAT endpoint is not an early-exit or overflow artifact.
- The K38 SPANNING endpoint now has a reconstructed stitch path under no-early
  full-ingest diagnostics.

What remains engineering trust:

- CUDA/CPU sampled parity does not prove every tile at the endpoint.
- The compositor audit found no false-MOAT path, but it is still code, not a
  theorem.
- BZ acceptance remains external to the executable.

What remains mathematical/spec work:

- This is not an origin-component result.
- This is not an exact K38 threshold.
- This is not a proof of global moat behavior across all radii.
- The I4/tower-closing proof chain in the methodology should be cleaned up or
  explicitly stated as a checked invariant condition.

## Next Gates

1. Add a post-run acceptance validator that recomputes BZ from profile radii/K,
   verifies region/radii/width agreement, requires zero overflows, rejects
   stale BZ logs, and enforces:
   `MOAT => active == produced == ingested && !early_exit_taken`.
2. Run endpoint sampled parity:
   - K38 `71,875,000..71,907,768`
   - K38 `73,437,500..73,470,268`
3. Build an independent SPANNING path certificate verifier for the K38
   `71,875,000` reconstructed path.
4. Before larger-radius K40/K42 work, harden integer widths for K1/MR/K4.

## Code Hardening Applied

File:

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/src/kernel_face_encode_v2.cu`

Change:

- added compile-time guards requiring `FACE_ENCODE_BLOCK_THREADS >= NUM_FACES *
  32` and warp alignment.

Remote verification:

- normal K38 build: `cmake --build build-k38 --target campaign_cuda -j2`
  passed.
- negative guard build: `FACE_ENCODE_BLOCK_THREADS=96` failed at the expected
  static assertion: `face encode needs one full warp per face`.
- negative guard logs:
  `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results/face-encode-guard-96.configure.log`
  and
  `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results/face-encode-guard-96.build.log`
