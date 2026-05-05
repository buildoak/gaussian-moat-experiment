# K34/K36 Centered Annulus Sweep - 2026-05-04

## Summary

This sweep maps local static-annulus connectivity only. A `SPANNING` verdict here means the annulus has a local inner-to-outer crossing in the streamed annulus model; it is not evidence of connectivity to the origin.

Artifacts:

- Remote: `/workspace/k34-k36-centered-sweep-20260504T023422Z/`
- Local: `tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/k34-k36-centered-sweep-20260504T023422Z/`

Execution substrate:

- Remote workspace: `/workspace/gaussian-moat-cuda-k34-gauntlet-20260504T010013Z`
- GPU: NVIDIA GeForce RTX 4090, driver 560.35.03
- Local git head: `183e451 Trace K34 spanning stitch path provenance`
- Source check: remote `apps/campaign_main_cuda.cpp` SHA-256 matched local `f92f1afa111200374eb5d724c8f07de232fb1d6a9654c88808689f07d5bf7561`
- Build gate: both `build-k34/campaign_main_cuda` and `build-k36/campaign_main_cuda` rebuilt successfully before the sweep.
- Artifact pull gate: 40 profiles and 40 stdout logs pulled locally.
- Overflow gate: all profile overflow counters and emitted overflow-bit counters were zero; max overflow sum per run = 0, total overflow sum = 0.

The remote export's `.git-commit` marker was stale, but the relevant source hashes matched local `183e451` and the binaries were rebuilt from that source before execution.

## K36 Centered Width Ladder

Fixed center: `R_center=80015786`.

| width | R_inner | R_outer | verdict | early exit | active | produced | ingested | total s | CUDA s | comp s | overflows | trace |
| ---: | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1024 | 80015274 | 80016298 | SPANNING | taken | 1294408 | 200000 | 20 | 2.279 | 1.656 | 0.000 | 0 | `bridge_lr c=3 j=312563 lhs=14/6 rhs=19/3 inner=15/4 outer=19/3` |
| 2048 | 80014762 | 80016810 | SPANNING | taken | 2276322 | 199992 | 45 | 2.275 | 1.729 | 0.000 | 0 | `bridge_lr c=4 j=312559 lhs=29/22 rhs=38/1 inner=36/7 outer=35/4` |
| 4096 | 80013738 | 80017834 | SPANNING | taken | 4240220 | 199992 | 85 | 2.915 | 2.316 | 0.000 | 0 | `bridge_lr c=4 j=312559 lhs=57/22 rhs=74/1 inner=68/9 outer=50/4` |
| 8192 | 80011690 | 80019882 | SPANNING | taken | 8168032 | 199983 | 85680 | 2.521 | 1.811 | 0.204 | 0 | `bridge_lr c=2595 j=312564 lhs=85644/6 rhs=85677/42 inner=85482/9 outer=85679/7` |
| 16384 | 80007594 | 80023978 | SPANNING | taken | 16023748 | 15397003 | 15380935 | 182.130 | 142.144 | 36.114 | 0 | `bridge_lr c=213958 j=227894 lhs=15380841/6 rhs=15380932/2 inner=15377078/28 outer=15380933/4` |
| 32768 | 79999402 | 80032170 | MOAT | no | 31734859 | 31734859 | 31734859 | 378.532 | 295.769 | 75.252 | 0 | none |

Result: the centered K36 window still SPANS through width 16384, but width 32768 is MOAT. The previously known non-centered K36 behavior should not be read as a centered-window result.

## K34 Verdict Matrix

| R_center | 1024 | 2048 | 4096 | 8192 | 16384 | 32768 | bracket |
| ---: | --- | --- | --- | --- | --- | --- | --- |
| 30000000 | SPANNING | SPANNING | SPANNING | SPANNING | MOAT | MOAT | `8192 < threshold <= 16384` |
| 40000000 | SPANNING | SPANNING | SPANNING | MOAT | MOAT | MOAT | `4096 < threshold <= 8192` |
| 50000000 | SPANNING | SPANNING | MOAT | MOAT | MOAT | MOAT | `2048 < threshold <= 4096` |
| 80000000 | SPANNING | MOAT | MOAT | MOAT | MOAT | MOAT | `1024 < threshold <= 2048` |

The sampled K34 local crossing width weakens substantially with larger radius. Near the Tsuchimura-scale K34 bound the earlier ladder was robustly SPANNING; by 80M, only the 1024-wide centered annulus still SPANS.

## K34 First-Pass Details

| R_center | width | R_inner | R_outer | verdict | early exit | active | produced | ingested | total s | CUDA s | comp s | overflows |
| ---: | ---: | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 30000000 | 1024 | 29999488 | 30000512 | SPANNING | taken | 485203 | 199996 | 65 | 1.989 | 1.550 | 0.000 | 0 |
| 30000000 | 2048 | 29998976 | 30001024 | SPANNING | taken | 853473 | 199999 | 117 | 2.054 | 1.647 | 0.000 | 0 |
| 30000000 | 4096 | 29997952 | 30002048 | SPANNING | taken | 1589941 | 199993 | 3213 | 2.107 | 1.694 | 0.008 | 0 |
| 30000000 | 8192 | 29995904 | 30004096 | SPANNING | taken | 3062295 | 199988 | 85801 | 2.444 | 1.768 | 0.208 | 0 |
| 30000000 | 16384 | 29991808 | 30008192 | MOAT | no | 6007739 | 6007739 | 6007739 | 69.832 | 53.446 | 14.673 | 0 |
| 30000000 | 32768 | 29983616 | 30016384 | MOAT | no | 11898297 | 11898297 | 11898297 | 138.682 | 106.516 | 29.131 | 0 |
| 40000000 | 1024 | 39999488 | 40000512 | SPANNING | taken | 647053 | 200000 | 546 | 2.089 | 1.570 | 0.001 | 0 |
| 40000000 | 2048 | 39998976 | 40001024 | SPANNING | taken | 1138030 | 199995 | 982 | 2.118 | 1.659 | 0.002 | 0 |
| 40000000 | 4096 | 39997952 | 40002048 | SPANNING | taken | 2119891 | 200000 | 193735 | 2.673 | 1.706 | 0.466 | 0 |
| 40000000 | 8192 | 39995904 | 40004096 | MOAT | no | 4083366 | 4083366 | 4083366 | 47.555 | 35.762 | 9.927 | 0 |
| 40000000 | 16384 | 39991808 | 40008192 | MOAT | no | 8010350 | 8010350 | 8010350 | 93.960 | 72.075 | 19.651 | 0 |
| 40000000 | 32768 | 39983616 | 40016384 | MOAT | no | 15864281 | 15864281 | 15864281 | 186.376 | 142.968 | 38.991 | 0 |
| 50000000 | 1024 | 49999488 | 50000512 | SPANNING | taken | 808963 | 199999 | 385 | 2.009 | 1.562 | 0.001 | 0 |
| 50000000 | 2048 | 49998976 | 50001024 | SPANNING | taken | 1422518 | 199996 | 177755 | 2.567 | 1.645 | 0.426 | 0 |
| 50000000 | 4096 | 49997952 | 50002048 | MOAT | no | 2649535 | 2649535 | 2649535 | 30.795 | 23.371 | 6.376 | 0 |
| 50000000 | 8192 | 49995904 | 50004096 | MOAT | no | 5104026 | 5104026 | 5104026 | 59.347 | 45.328 | 12.415 | 0 |
| 50000000 | 16384 | 49991808 | 50008192 | MOAT | no | 10012713 | 10012713 | 10012713 | 115.130 | 88.008 | 24.555 | 0 |
| 50000000 | 32768 | 49983616 | 50016384 | MOAT | no | 19830392 | 19830392 | 19830392 | 229.939 | 176.291 | 48.897 | 0 |
| 80000000 | 1024 | 79999488 | 80000512 | SPANNING | taken | 1294191 | 199996 | 58743 | 2.276 | 1.587 | 0.137 | 0 |
| 80000000 | 2048 | 79998976 | 80001024 | MOAT | no | 2275976 | 2275976 | 2275976 | 25.041 | 18.650 | 5.378 | 0 |
| 80000000 | 4096 | 79997952 | 80002048 | MOAT | no | 4239576 | 4239576 | 4239576 | 48.549 | 36.853 | 10.247 | 0 |
| 80000000 | 8192 | 79995904 | 80004096 | MOAT | no | 8166476 | 8166476 | 8166476 | 94.094 | 71.789 | 19.970 | 0 |
| 80000000 | 16384 | 79991808 | 80008192 | MOAT | no | 16020289 | 16020289 | 16020289 | 186.208 | 142.540 | 39.614 | 0 |
| 80000000 | 32768 | 79983616 | 80016384 | MOAT | no | 31728545 | 31728545 | 31728545 | 369.838 | 283.877 | 78.574 | 0 |

For SPANNING rows, full `SPANNING_TRACE` fields are in the paired stdout logs and JSON profiles. The profile fields include event, column, tile/group, and inner/outer source tile/group labels.

## Confirmed Diagnostic Reruns

All diagnostic reruns used `--no-early-exit --overflow-diagnostics --profile --trace-spanning --timing`.

| K | R_center | width | R_inner | R_outer | reason | verdict | active | produced | ingested | total s | CUDA s | comp s | overflows | trace |
| ---: | ---: | ---: | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 36 | 80015786 | 16384 | 80007594 | 80023978 | K36 widest SPANNING boundary side | SPANNING | 16023748 | 16023748 | 16023748 | 191.667 | 149.813 | 37.727 | 0 | `bridge_lr c=213958 j=227894 lhs=15380841 rhs=15380932 inner=15377078 outer=15380933` |
| 36 | 80015786 | 32768 | 79999402 | 80032170 | K36 first MOAT boundary side | MOAT | 31734859 | 31734859 | 31734859 | 381.380 | 298.723 | 75.206 | 0 | none |
| 34 | 30000000 | 8192 | 29995904 | 30004096 | K34 widest SPANNING at 30M | SPANNING | 3062295 | 3062295 | 3062295 | 36.305 | 27.730 | 7.441 | 0 | `bridge_lr c=2598 j=117148 lhs=85741 rhs=85774 inner=85735 outer=85073` |
| 34 | 30000000 | 16384 | 29991808 | 30008192 | K34 first MOAT at 30M | MOAT | 6007739 | 6007739 | 6007739 | 69.898 | 53.418 | 14.717 | 0 | none |
| 34 | 40000000 | 4096 | 39997952 | 40002048 | K34 widest SPANNING at 40M | SPANNING | 2119891 | 2119891 | 2119891 | 24.865 | 18.893 | 5.095 | 0 | `bridge_lr c=11361 j=155832 lhs=193705 rhs=193722 inner=193701 outer=193700` |
| 34 | 40000000 | 8192 | 39995904 | 40004096 | K34 first MOAT at 40M | MOAT | 4083366 | 4083366 | 4083366 | 47.726 | 36.381 | 10.022 | 0 | none |
| 34 | 50000000 | 2048 | 49998976 | 50001024 | K34 widest SPANNING at 50M | SPANNING | 1422518 | 1422518 | 1422518 | 16.176 | 12.020 | 3.348 | 0 | `bridge_lr c=19609 j=194328 lhs=177744 rhs=177753 inner=177673 outer=177745` |
| 34 | 50000000 | 4096 | 49997952 | 50002048 | K34 first MOAT at 50M | MOAT | 2649535 | 2649535 | 2649535 | 30.982 | 23.593 | 6.390 | 0 | none |
| 34 | 80000000 | 1024 | 79999488 | 80000512 | K34 widest SPANNING at 80M | SPANNING | 1294191 | 1294191 | 1294191 | 13.977 | 10.266 | 2.854 | 0 | `bridge_lr c=11700 j=312281 lhs=58736 rhs=58741 inner=58733 outer=58742` |
| 34 | 80000000 | 2048 | 79998976 | 80001024 | K34 first MOAT at 80M | MOAT | 2275976 | 2275976 | 2275976 | 25.144 | 18.726 | 5.340 | 0 | none |

The diagnostic reruns match the first-pass verdicts exactly.

## Interpretation

K36 centered at `80015786` has a local crossing threshold between widths `16384` and `32768`. The centered 32768-wide annulus is a confirmed MOAT with full ingestion and overflow diagnostics. Narrower centered windows are not moats in this local sense.

K34 does weaken at larger radii in this sampled centered-annulus ladder:

- At 30M, the bracket is `8192 < threshold <= 16384`.
- At 40M, the bracket is `4096 < threshold <= 8192`.
- At 50M, the bracket is `2048 < threshold <= 4096`.
- At 80M, the bracket is `1024 < threshold <= 2048`.

This is not an origin-connectivity statement, and it does not prove a smooth monotone law. It does show that the earlier robust K34 spanning near the Tsuchimura-scale radius does not persist unchanged at larger radii for centered local annuli.

## K40-Oriented Recommendation

Use centered-annulus screening as a local percolation calibration step, separate from any later origin-component verifier.

For K40, start with a width ladder that is wide enough to bracket the local crossing threshold rather than assuming K34-like widths. Since K36 at roughly 80M still SPANS at 16384 and only MOATs at 32768, a practical K40 first screen at large radius should include at least `32768`, `65536`, and possibly `131072` widths. Use early exit first; confirm the widest SPANNING and first MOAT with `--no-early-exit --overflow-diagnostics --profile`.

Search strategy:

1. Pick a fixed `R_center` and run powers-of-two widths until a `SPANNING -> MOAT` bracket appears.
2. Binary-search even widths inside that bracket only after both bracket endpoints have diagnostic confirmations.
3. Repeat at multiple radii before extrapolating, because the K34 ladder shows strong radius dependence in local crossing width.
4. Treat MOAT runs as the cost driver: they ingest all tiles, so schedule the widest K40 MOAT confirmations in small batches with profiles enabled.
