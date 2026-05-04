# K34 Static-Annulus Bug Gauntlet - 2026-05-04

## Scope

Goal: test the narrow K34 static-annulus bug prior without building or delivering
an origin-component verifier.

Branch: `debug/k34-static-annulus-gauntlet`

Tested source: local tree based on `1ecf94a Add compact prime parity diff` plus
the gauntlet instrumentation in this change.

Remote GPU: Vast instance `36021982`, RTX 4090, workspace
`/workspace/gaussian-moat-cuda-k34-gauntlet-20260504T010013Z`.

Local evidence bundle:
`tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/k34-gauntlet-20260504T010708Z/`

## Commands

Builds on the 4090:

```bash
cmake --build tiles-maxxing/cpp-campaign-v2/build-k34 --target campaign_main compare_snapshots -j $(nproc)
cmake --build tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k34 --target campaign_main_cuda cuda_vs_cpu_diff -j $(nproc)
cmake --build tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k36 --target campaign_main_cuda -j $(nproc)
```

Main K34 gate:

```bash
tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/run_k34_regression_gate.sh \
  --cpu-bin tiles-maxxing/cpp-campaign-v2/build-k34/campaign_main \
  --cuda-bin tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k34/campaign_main_cuda \
  --diff-bin tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k34/cuda_vs_cpu_diff \
  --timing \
  --profile-dir /workspace/k34-gauntlet-20260504T010708Z/profiles \
  --work-dir /workspace/k34-gauntlet-20260504T010708Z/work \
  --keep
```

Additional probes:

```bash
cuda_vs_cpu_diff --r-inner 24289452 --r-outer 24297644 --geo-flags --sample 257 --verbose

for tile in 0,94880 14,94890 34780,88287 67113,67113; do
  cuda_vs_cpu_diff --r-inner 24289452 --r-outer 24297644 --tile "$tile" --geo-flags --verbose
  cuda_vs_cpu_diff --r-inner 24289452 --r-outer 24297644 --tile "$tile" --m4 --verbose
  cuda_vs_cpu_diff --r-inner 24289452 --r-outer 24297644 --tile "$tile" --k5 --verbose
done
```

Monotonicity matrix:

```bash
campaign_main_cuda --k-sq=34 --r-inner=24289452 --r-outer=$((24289452 + delta)) \
  --region full-octant --chunk-size=200000 --timing --trace-spanning

campaign_main_cuda_k36 --k-sq=36 --r-inner=24289452 --r-outer=24297644 \
  --region full-octant --chunk-size=200000 --timing --trace-spanning
```

## Results

| Gate | Result | Evidence |
| --- | --- | --- |
| K34 CPU/CUDA snapshot smoke | PASS | SHA-256 match on smoke shell |
| K34 compact prime parity | PASS | 64 deterministic shell samples |
| K34 geo-flag parity | PASS | 64 deterministic shell samples |
| K34 M4 parity | PASS | 32 deterministic shell samples |
| K34 K5 full TileOp parity | PASS | 32 deterministic shell samples |
| K34 full shell sentinel | SPANNING | `R_inner=24289452`, `R_outer=24297644` |
| Overflow counters | PASS | all four overflow counters zero |
| Boundary geo sweep | PASS | 257 deterministic shell samples |
| Named boundary/tip tiles | PASS | geo/M4/K5 for `0,94880`, `14,94890`, `34780,88287`, `67113,67113` |
| Same-geometry K34 -> K36 monotonicity | PASS | K34 SPANNING, K36 SPANNING |
| Fixed-inner larger outer sequence | no contradiction | K34 SPANNING at deltas 512, 1024, 2048, 4096, 8192 |
| K34 CUDA CTest | PASS | 13/13 tests passed |
| K34 CPU CTest | PASS | 113/113 tests passed; two tests reported skipped by CTest |

Full K34 shell profile:

```text
active tiles: 2479915
produced tiles: 2479915
ingested tiles: 2479915
ingested columns: 67114
cuda-k1-k5: 21.195 s
compositor: 4.665 s
total: 26.794 s
VERDICT: SPANNING
```

First-spanning trace on the full no-early-exit shell:

```text
event=bridge_lr
column_i=14 tile_j=94890
lhs_tile_index=439 lhs_group_label=1
rhs_tile_index=472 rhs_group_label=2
component=1
reach_before=1 reach_after=3 added_bits=2
```

Same-geometry K36 check:

```text
K_SQ=36 R_inner=24289452 R_outer=24297644
VERDICT: SPANNING
SPANNING_TRACE: event=bridge_lr column_i=1 tile_j=94907
lhs_tile_index=27 lhs_group_label=33 rhs_tile_index=60 rhs_group_label=11
reach_before=1 reach_after=3 added_bits=2
```

## Interpretation

No concrete K34 CUDA/compositor bug was found in this gauntlet.

The strongest evidence against the current bug hypothesis is that the final
SPANNING verdict survives all of these independent layers:

- CUDA compacted prime order matches an independent CPU sieve order.
- CUDA geo bits match the exact CPU integer predicate on both wide deterministic
  samples and named boundary/tip tiles.
- M4 dense remap and K5 TileOp bytes match CPU expectations.
- The full shell has zero K1/K4/K5 overflow counters.
- The first spanning event is now explainable at the compositor level as an L/R
  bridge between two concrete group labels whose reach bits combine from inner
  to outer.
- Existing K36-oriented golden tests were made K-aware and then both K34 test
  suites passed on the 4090.

This does not prove K34 and K36 are in different mathematical regimes. It does
lower the likelihood that the observed K34 SPANNING result is caused by the
known narrow implementation risks: compact-order drift, geo-flag threshold
errors, M4 label/flag bugs, K5 TileOp drift, overflow contamination, or an
unattributed compositor latch.

The fixed-inner monotonicity check is weak: every tested K34 annulus already
spanned, so it found no forbidden MOAT -> SPANNING flip as `R_outer` increased.
It should be treated as a consistency check, not as explanatory evidence.

## Next Experiment

The next bug hunt should move from pipeline parity to predicate semantics:

1. Extract a small local neighborhood around the first K34 spanning bridge
   (`lhs_tile_index=439`, `rhs_tile_index=472`) and inspect the actual component
   structure in that neighborhood.
2. Compare the static-annulus spanning component against the informal
   Tsuchimura/origin mental model without building a production origin verifier.
3. Look for a nearby fixed-inner or fixed-outer K34 sequence that contains both
   MOAT and SPANNING outcomes; that would make monotonicity tests informative
   instead of vacuous.
