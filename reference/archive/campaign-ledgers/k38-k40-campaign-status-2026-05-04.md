# K38/K40 Campaign Status - 2026-05-04

## Summary

This note records the build, audit, preflight, and first K40 campaign state for the
K38/K40 local static-annulus campaign. These runs are local annulus connectivity
tests only. A `SPANNING` verdict means the streamed annulus has a local
inner-to-outer crossing; it is not evidence of connectivity to the origin.

Do not treat any result here as an origin-component verifier result. The origin
component verifier is explicitly out of delivery scope for this campaign.

Follow-up correctness audit:

- `reference/k38-k40-correctness-audit-2026-05-04.md`
- records the K38 no-early endpoint diagnostics, same-annulus monotonicity
  sentinel, and residual hardening work.

Local branch:

- `debug/k34-static-annulus-gauntlet`

Local readiness commit:

- `f110566 Add K38 campaign anchor and refresh K40 golden`

Remote workspace:

- `/workspace/gaussian-moat-cuda-k34-gauntlet-20260504T010013Z`

GPU:

- NVIDIA GeForce RTX 4090 on Vast instance `36021982`

## Prepared Builds

K40 CUDA build:

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k40/campaign_main_cuda`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k40/cuda_vs_cpu_diff`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k40/cuda_golden_dump`

K40 CPU build:

- `tiles-maxxing/cpp-campaign-v2/build-k40/campaign_main`

K38 CUDA build:

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k38/campaign_main_cuda`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k38/cuda_vs_cpu_diff`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k38/cuda_golden_dump`

K38 CPU build:

- `tiles-maxxing/cpp-campaign-v2/build-k38/campaign_main`

## Local Readiness Changes

The readiness commit:

- adds a K38 build anchor to `tiles-maxxing/cpp-campaign-v2/scripts/bz_config.json`
- documents that K38/K40 sweep radii still require per-run BZ logs
- adds `ceil_isqrt(38) == 7` coverage
- clarifies that non-square K values such as 38 and 40 use the canonical norm-form annulus test
- adds K38 CUDA constants assertions
- refreshes `golden/k40-r100m.json`

`git diff --check` passed before commit.

## Auditor Findings

K40 general/adversarial auditors marked the campaign not ready until the following
gates were clean:

- K40 CPU and CUDA builds exist
- CPU and CUDA CTests pass
- K40 golden is coherent
- snapshot smoke matches CPU/CUDA
- per-run BZ is required for exact radii
- CUDA/CPU diff probes must be split across geo, M4, and K5 modes
- any overflow counter or `SPANNING_TRACE event=overflow` invalidates the verdict
- K40 evidence is only local annulus evidence, not origin connectivity

K38 auditor found the K38 anchor and constants coherent, with the same requirement
for split diff probes and per-run BZ.

## K40 Preflight

Remote preflight tag:

- `k40-preflight-20260504T033150Z`

Remote preflight dir:

- `/workspace/k40-preflight-20260504T033150Z`

Passed gates:

- CPU CTest: `115/115`
- CUDA CTest: `13/13`
- K40 golden after refresh: `OK k40-r100m 2edf26469ccd21ae0c662423165feac2cdc915f4bc2a98696326abad859611fd`
- snapshot smoke: CPU/CUDA SHA matched `cb4ba01d0c55aec894ffdb0fa9f70a968399d77666bdd0529fe473b2409757a9`
- snapshot smoke `K_SQ: 40`
- snapshot smoke overflow counters: all zero

BZ/diff probes all returned zero status:

| label | R_inner | width | R_outer | BZ | geo | M4 | K5 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| R800000000_W8192 | 800000000 | 8192 | 800008192 | 0 | 0 | 0 | 0 |
| R800000000_W10000 | 800000000 | 10000 | 800010000 | 0 | 0 | 0 | 0 |
| R1000000000_W8192 | 1000000000 | 8192 | 1000008192 | 0 | 0 | 0 | 0 |
| R1000000000_W10000 | 1000000000 | 10000 | 1000010000 | 0 | 0 | 0 | 0 |
| R1500000000_W8192 | 1500000000 | 8192 | 1500008192 | 0 | 0 | 0 | 0 |
| R1500000000_W10000 | 1500000000 | 10000 | 1500010000 | 0 | 0 | 0 | 0 |

## K38 Preflight

Remote preflight tag:

- `k38-preflight-20260504T033820Z`

Remote preflight dir:

- `/workspace/k38-preflight-20260504T033820Z`

Passed gates:

- CPU CTest: `115/115`
- CUDA CTest: `13/13`
- snapshot smoke: CPU/CUDA SHA matched `6de4754c1a443cf639eaadf4955f657d58cd19e1b0e631d82f54a7243f34cbdb`
- snapshot smoke `K_SQ: 38`
- snapshot smoke overflow counters: all zero

BZ/diff probes all returned zero status:

| label | R_inner | width | R_outer | BZ | geo | M4 | K5 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| R300000000_W10000 | 300000000 | 10000 | 300010000 | 0 | 0 | 0 | 0 |
| R800000000_W10000 | 800000000 | 10000 | 800010000 | 0 | 0 | 0 | 0 |
| R1000000000_W10000 | 1000000000 | 10000 | 1000010000 | 0 | 0 | 0 | 0 |
| R1500000000_W10000 | 1500000000 | 10000 | 1500010000 | 0 | 0 | 0 | 0 |

## K38 Broad Bracket Campaign

Campaign tag:

- `k38-broad-bracket-20260504T105733Z`

Remote campaign dir:

- `/workspace/k38-broad-bracket-20260504T105733Z`

Primary index:

- `/workspace/k38-broad-bracket-20260504T105733Z/run-index.tsv`

Strategy:

- width `32768`
- broad screen starting at `R_inner=300000000`
- adaptive follow-up at `500M`, `800M`, and bracket midpoints depending on
  whether the first rows SPAN or MOAT
- per-run BZ before CUDA
- stop on nonzero CUDA return code, nonzero overflow counter, or overflow trace

Confirmed K38 rows:

| R_inner | width | verdict | produced | ingested | overflow counters |
| ---: | ---: | --- | ---: | ---: | ---: |
| 300000000 | 32768 | MOAT | 118987826 | 118987826 | 0 |
| 200000000 | 32768 | MOAT | 79327833 | 79327833 | 0 |
| 100000000 | 32768 | MOAT | 39667163 | 39667163 | 0 |

The `R_inner=300000000, width=32768` row is a full-ingest MOAT:

- `active tiles = produced tiles = ingested tiles = 118987826`
- CUDA return code: `0`
- all overflow counters: `0`
- `SPANNING_TRACE detected=0`
- total runtime: `1446.47s`

The `R_inner=200000000, width=32768` and `R_inner=100000000, width=32768` rows
are also full-ingest MOATs with CUDA return code `0` and all overflow counters
at `0`.

The K38 broad-bracket campaign completed after the `100M` row. It did not find
a SPANNING lower endpoint, so the K38 width-32768 threshold is below `100M` in
this local-annulus sense.

## K38 Low Bracket Campaign

Campaign tag:

- `k38-low-bracket-20260504T115438Z`

Remote campaign dir:

- `/workspace/k38-low-bracket-20260504T115438Z`

Primary index:

- `/workspace/k38-low-bracket-20260504T115438Z/run-index.tsv`

Strategy:

- width `32768`
- lower-radius screen starting at `R_inner=50000000`
- adaptive follow-up at `25M`, `12.5M`, or `75M` depending on the first verdicts
- per-run BZ before CUDA
- stop on nonzero CUDA return code, nonzero overflow counter, or overflow trace

Confirmed low-bracket rows:

| R_inner | width | verdict | produced | ingested | overflow counters |
| ---: | ---: | --- | ---: | ---: | ---: |
| 50000000 | 32768 | SPANNING | 199959 | 1161 | 0 |
| 75000000 | 32768 | MOAT | 29752094 | 29752094 | 0 |

The `R_inner=75000000, width=32768` row is a full-ingest MOAT:

- `active tiles = produced tiles = ingested tiles = 29752094`
- CUDA return code: `0`
- all overflow counters: `0`
- `SPANNING_TRACE detected=0`
- total runtime: `362.471s`

The active K38 radius bracket at width `32768` is now:

- `R_inner=50000000`: SPANNING
- `R_inner=75000000`: MOAT

The low-bracket campaign completed after the `75M` row.

## K38 Radius-Refine Campaign

Campaign tag:

- `k38-radius-refine-20260504T120611Z`

Remote campaign dir:

- `/workspace/k38-radius-refine-20260504T120611Z`

Primary index:

- `/workspace/k38-radius-refine-20260504T120611Z/run-index.tsv`

Strategy:

- width `32768`
- radius refinement inside the `50M` to `75M` bracket
- first probe: `R_inner=62500000`
- per-run BZ before CUDA
- stop on nonzero CUDA return code, nonzero overflow counter, or overflow trace

Confirmed radius-refine rows:

| R_inner | width | verdict | produced | ingested | overflow counters |
| ---: | ---: | --- | ---: | ---: | ---: |
| 62500000 | 32768 | SPANNING | 199956 | 10320 | 0 |
| 68750000 | 32768 | SPANNING | 199954 | 21801 | 0 |

The active K38 radius bracket at width `32768` is now:

- `R_inner=68750000`: SPANNING
- `R_inner=75000000`: MOAT

## K38 Radius-Refine 2 Campaign

Campaign tag:

- `k38-radius-refine2-20260504T121714Z`

Remote campaign dir:

- `/workspace/k38-radius-refine2-20260504T121714Z`

Primary index:

- `/workspace/k38-radius-refine2-20260504T121714Z/run-index.tsv`

Strategy:

- single midpoint at `R_inner=71875000`, width `32768`
- per-run BZ before CUDA
- stop on nonzero CUDA return code, nonzero overflow counter, or overflow trace

Confirmed row:

| R_inner | width | verdict | produced | ingested | overflow counters |
| ---: | ---: | --- | ---: | ---: | ---: |
| 71875000 | 32768 | SPANNING | 599915 | 459402 | 0 |

The active K38 radius bracket at width `32768` is now:

- `R_inner=71875000`: SPANNING
- `R_inner=75000000`: MOAT

## K38 Radius-Refine 3 Campaign

Campaign tag:

- `k38-radius-refine3-20260504T122839Z`

Remote campaign dir:

- `/workspace/k38-radius-refine3-20260504T122839Z`

Primary index:

- `/workspace/k38-radius-refine3-20260504T122839Z/run-index.tsv`

Strategy:

- single midpoint at `R_inner=73437500`, width `32768`
- per-run BZ before CUDA
- stop on nonzero CUDA return code, nonzero overflow counter, or overflow trace

Confirmed row:

| R_inner | width | verdict | produced | ingested | overflow counters |
| ---: | ---: | --- | ---: | ---: | ---: |
| 73437500 | 32768 | MOAT | 29132322 | 29132322 | 0 |

The `R_inner=73437500, width=32768` row is a full-ingest MOAT:

- `active tiles = produced tiles = ingested tiles = 29132322`
- CUDA return code: `0`
- all overflow counters: `0`
- `SPANNING_TRACE detected=0`
- total runtime: `355.267s`

The final K38 radius bracket from this campaign at width `32768` is:

- `R_inner=71875000`: SPANNING
- `R_inner=73437500`: MOAT

## K38 Endpoint Diagnostic Confirmation

Campaign tag:

- `k38-bracket-diag-20260504T163900Z`

Remote campaign dir:

- `/workspace/k38-bracket-diag-20260504T163900Z`

Pulled local evidence mirror:

- `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results/remote/k38-bracket-diag-20260504T163900Z`

Purpose:

- diagnostic confirmation of the K38 `71.875M SPANNING` / `73.4375M MOAT`
  bracket at width `32768`
- commands include `--no-early-exit` and `--overflow-diagnostics`
- the SPANNING endpoint also includes `--trace-spanning-path`
- per-run BZ before CUDA

Confirmed diagnostic rows:

| R_inner | width | verdict | produced | ingested | overflow counters | early exit | note |
| ---: | ---: | --- | ---: | ---: | ---: | --- | --- |
| 71875000 | 32768 | SPANNING | 28512666 | 28512666 | 0 | disabled | path reconstructed |
| 73437500 | 32768 | MOAT | 29132322 | 29132322 | 0 | disabled | no spanning trace |

The K38 `71875000/32768` diagnostic strengthened the earlier early-exit
SPANNING row:

- `active tiles = produced tiles = ingested tiles = 28512666`
- CUDA return code: `0`
- BZ check: pass
- `early-exit: disabled`
- all overflow counters: `0`
- `SPANNING_PATH reconstructed=1`
- final bridge: `459215/42:R#19` to `459344/4:L#19`
- inner source: `452307/2`
- outer source: `455273/1`
- total runtime: `365.671s`

The K38 `73437500/32768` diagnostic confirmed the MOAT endpoint:

- `active tiles = produced tiles = ingested tiles = 29132322`
- CUDA return code: `0`
- BZ check: pass
- `early-exit: disabled`
- all overflow counters: `0`
- `SPANNING_TRACE detected=0`
- total runtime: `354.138s`

Same-annulus monotonicity control:

- `K=36, R_inner=73437500, R_outer=73470268, width=32768`
- remote dir: `/workspace/k36-same-annulus-control-20260504T165239Z`
- pulled mirror:
  `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results/remote/k36-same-annulus-control-20260504T165239Z`
- verdict: `MOAT`
- `active tiles = produced tiles = ingested tiles = 29132322`
- `early-exit: disabled`
- all overflow counters: `0`

This passes the same-annulus implication `K38 MOAT => K36 MOAT`.

Second same-annulus monotonicity control:

- `K=40, R_inner=71875000, R_outer=71907768, width=32768`
- remote dir: `/workspace/k40-same-annulus-control-20260504T170020Z`
- pulled mirror:
  `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results/remote/k40-same-annulus-control-20260504T170020Z`
- verdict: `SPANNING`
- `active tiles = produced tiles = ingested tiles = 28512666`
- `early-exit: disabled`
- all overflow counters: `0`

This passes the paired same-annulus implication `K38 SPANNING => K40 SPANNING`.

## K40 Overnight Campaign

Campaign tag:

- `k40-wide-overnight-20260504T034525Z`

Remote campaign dir:

- `/workspace/k40-wide-overnight-20260504T034525Z`

Primary index:

- `/workspace/k40-wide-overnight-20260504T034525Z/run-index.tsv`

Passive postprocess summary:

- `/workspace/k40-wide-overnight-20260504T034525Z/summary.txt`

Strategy:

- 8-hour budget
- per-run BZ before CUDA
- stop on nonzero CUDA return code
- stop on any nonzero overflow counter
- stop on `SPANNING_TRACE event=overflow`
- wide screen first at widths `32768`, `65536`, and `131072`
- radii include `800M`, `860M`, `1B`, `1.2B`, and `1.5B`
- if budget survives, focus around the old `~860M` signal and then narrow widths

Completed evidence rows from the broad run:

| R_inner | width | verdict | produced | ingested | overflow counters |
| ---: | ---: | --- | ---: | ---: | ---: |
| 800000000 | 32768 | SPANNING | 199951 | 16642 | 0 |
| 860000000 | 32768 | SPANNING | 199951 | 20770 | 0 |
| 1000000000 | 32768 | MOAT | 396611825 | 396611825 | 0 |

The `R_inner=1000000000, width=32768` row is a full-ingest MOAT:

- `active tiles = produced tiles = ingested tiles = 396611825`
- CUDA return code: `0`
- all overflow counters: `0`
- `SPANNING_TRACE detected=0`
- total runtime: `4635.01s`

The broad runner then started `R_inner=1200000000, width=32768`, but that row
was deliberately stopped after the `1B` MOAT established a more valuable bracket.
The aborted row is recorded as `NO_VERDICT`, `rc=143`, and must not be used as
mathematical evidence.

The resulting K40 radius bracket at width `32768` is:

- `R_inner=860000000`: SPANNING
- `R_inner=1000000000`: MOAT

## K40 Focused Bracket Campaign

Campaign tag:

- `k40-focused-bracket-20260504T051242Z`

Remote campaign dir:

- `/workspace/k40-focused-bracket-20260504T051242Z`

Primary index:

- `/workspace/k40-focused-bracket-20260504T051242Z/run-index.tsv`

Strategy:

- adaptive radius narrowing between `860M` and `1B` at width `32768`
- then width narrowing at the confirmed MOAT radius `1B` if budget remains
- per-run BZ before CUDA
- stop on nonzero CUDA return code, nonzero overflow counter, or overflow trace

Initial focused row:

| R_inner | width | verdict | produced | ingested | overflow counters |
| ---: | ---: | --- | ---: | ---: | ---: |
| 900000000 | 32768 | SPANNING | 999761 | 892301 | 0 |
| 930000000 | 32768 | SPANNING | 27592008 | 27567314 | 0 |
| 960000000 | 32768 | SPANNING | 245918441 | 245896833 | 0 |
| 980000000 | 32768 | MOAT | 388679866 | 388679866 | 0 |

The `R_inner=930000000, width=32768` row is a late SPANNING result:

- `active tiles = 368850023`
- `produced tiles = 27592008`
- `ingested tiles = 27567314`
- CUDA return code: `0`
- all overflow counters: `0`
- total runtime: `325.172s`

The `R_inner=960000000, width=32768` row is an extremely late SPANNING result:

- `active tiles = 380746894`
- `produced tiles = 245918441`
- `ingested tiles = 245896833`
- CUDA return code: `0`
- all overflow counters: `0`
- total runtime: `2873.14s`

The `R_inner=980000000, width=32768` row is a full-ingest MOAT:

- `active tiles = produced tiles = ingested tiles = 388679866`
- CUDA return code: `0`
- all overflow counters: `0`
- `SPANNING_TRACE detected=0`
- total runtime: `4533.3s`

The active K40 radius bracket at width `32768` is now:

- `R_inner=960000000`: SPANNING
- `R_inner=980000000`: MOAT

The focused runner then started width narrowing at `R_inner=1000000000,
width=24576`, but that row was deliberately stopped after the `980M` MOAT made
radius refinement the higher-value next experiment. The aborted width row is
recorded as `NO_VERDICT`, `rc=143`, and must not be used as mathematical
evidence.

## K40 Radius-Refine Campaign

First attempted campaign tag:

- `k40-radius-refine-20260504T073258Z`

This first attempt failed before launching CUDA because the runner script used
`set -u` with a same-line local variable initialization bug. It produced no
evidence rows and should be ignored except as an operational record.

Active campaign tag:

- `k40-radius-refine2-20260504T073415Z`

Remote campaign dir:

- `/workspace/k40-radius-refine2-20260504T073415Z`

Primary index:

- `/workspace/k40-radius-refine2-20260504T073415Z/run-index.tsv`

Strategy:

- adaptive radius refinement between `960M` and `980M` at width `32768`
- first probe: `R_inner=970000000`
- per-run BZ before CUDA
- stop on nonzero CUDA return code, nonzero overflow counter, or overflow trace

Confirmed radius-refine rows:

| R_inner | width | verdict | produced | ingested | overflow counters |
| ---: | ---: | --- | ---: | ---: | ---: |
| 970000000 | 32768 | SPANNING | 378065743 | 377947112 | 0 |
| 975000000 | 32768 | SPANNING | 310493669 | 310334578 | 0 |
| 978000000 | 32768 | SPANNING | 76775720 | 76599114 | 0 |

The `R_inner=970000000, width=32768` row is a near-full late SPANNING result:

- `active tiles = 384713681`
- `produced tiles = 378065743`
- `ingested tiles = 377947112`
- CUDA return code: `0`
- all overflow counters: `0`
- total runtime: `4419.79s`

The `R_inner=975000000, width=32768` row is another late SPANNING result:

- `active tiles = 386697074`
- `produced tiles = 310493669`
- `ingested tiles = 310334578`
- CUDA return code: `0`
- all overflow counters: `0`
- total runtime: `3636.51s`

The `R_inner=978000000, width=32768` row is a SPANNING result:

- `active tiles = 387886841`
- `produced tiles = 76775720`
- `ingested tiles = 76599114`
- CUDA return code: `0`
- all overflow counters: `0`
- total runtime: `901.436s`

The final K40 radius bracket from this campaign at width `32768` is:

- `R_inner=978000000`: SPANNING
- `R_inner=980000000`: MOAT

The radius-refine campaign completed after the `978M` row. It did not run a
`979M` midpoint, so the current bracket width is `2M`.

## K40 980M Diagnostic Confirmation

Campaign tag:

- `k40-980m-diag-20260504T124022Z`

Remote campaign dir:

- `/workspace/k40-980m-diag-20260504T124022Z`

Purpose:

- diagnostic confirmation of the K40 `R_inner=980000000, width=32768` MOAT
  endpoint
- command includes `--no-early-exit`, `--overflow-diagnostics`, and
  `--trace-spanning-path`
- per-run BZ before CUDA

This first diagnostic attempt passed BZ but the CUDA process was killed before
writing a verdict. The failure appears tied to `--trace-spanning-path` overhead
on the full K40 MOAT case. It is not mathematical evidence.

Relaunched diagnostic tag:

- `k40-980m-diag2-20260504T140207Z`

Remote campaign dir:

- `/workspace/k40-980m-diag2-20260504T140207Z`

Purpose:

- diagnostic confirmation of the same K40 `980M/32768` MOAT endpoint
- command includes `--no-early-exit` and `--overflow-diagnostics`
- omits path reconstruction to avoid the killed diagnostic mode
- per-run BZ before CUDA

Confirmed diagnostic row:

| R_inner | width | verdict | produced | ingested | overflow counters | early exit |
| ---: | ---: | --- | ---: | ---: | ---: | --- |
| 980000000 | 32768 | MOAT | 388679866 | 388679866 | 0 | disabled |

The diagnostic confirmation succeeded:

- CUDA return code: `0`
- BZ check: pass
- `active tiles = produced tiles = ingested tiles = 388679866`
- `early-exit: disabled`
- all overflow counters: `0`
- `SPANNING_TRACE detected=0`
- total runtime: `4571.39s`

## Completion Audit

Objective checklist:

- Sartre K34/K36 centered runs documented: `reference/k34-k36-centered-annulus-sweep-2026-05-04.md`
- K38 and K40 builds prepared: `build-k38` and `build-k40` CPU/CUDA artifacts listed above
- canonical non-square K handling guarded: K38 anchor, K38 static assertions, K40 golden refresh in commit `f110566`
- auditors used before campaign runs: K38/K40 auditor findings summarized above
- K38 and K40 preflight gates passed: CTests, snapshot smokes, golden/diff/BZ gates listed above
- per-run BZ preserved: every campaign row links its BZ log in the remote index
- overflow gate passed: all accepted rows have all overflow counters at `0`
- origin-component verifier not delivered: this report explicitly scopes results to local annulus connectivity only
- K40 proper run completed: width `32768` bracket `978M SPANNING` / `980M MOAT`
- K40 MOAT diagnostic confirmation completed: `k40-980m-diag2-20260504T140207Z`, no early exit, overflow diagnostics, zero overflow
- K38 proper run completed: width `32768` bracket `71.875M SPANNING` / `73.4375M MOAT`
- remote artifacts preserved by path: every run index links profile, stdout, and BZ logs

No completion-blocking work remains for this K38/K40 campaign-preparation and
first-run objective.

Optional follow-up work:

- pull remote profiles/logs locally if Vast teardown is planned
- run a K40 `979M` midpoint if a `1M` radius bracket is desired
- run K38 width-ladder probes at the final radius bracket
- prepare the next sqrt(40)/sqrt(42) campaign plan from these brackets
