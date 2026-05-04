# K34 Deeper Static-Annulus Witness Trace - 2026-05-04

## Scope

Goal: deepen the K34 static-annulus bug gauntlet without implementing an
origin-component verifier.

Branch: `debug/k34-static-annulus-gauntlet`

Remote GPU: Vast instance `36021982`, RTX 4090, workspace
`/workspace/gaussian-moat-cuda-k34-gauntlet-20260504T010013Z`.

Local ignored evidence bundle:
`tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/k34-deeper-20260504T012500Z/`

## New Instrumentation

- `campaign_main_cuda --trace-spanning` now records the first tile/group source
  for each reach bit in the first spanning component:
  `inner_source_tile_index`, `inner_source_group_label`,
  `outer_source_tile_index`, `outer_source_group_label`.
- `cuda_vs_cpu_diff --dump-tile --dump-label N` dumps a selected tile/group:
  tile metadata, group geo flags, TileOp inner/outer flags, face-port ordinals,
  and primes in the dense component.

The instrumentation preserves existing verdict logic. It only records
provenance attached to the already-maintained compositor reach bits.

## Width Ladder

Fixed `R_inner=24289452`. Tested widths:
`512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072`.

All nine runs returned `VERDICT: SPANNING`; all overflow counters were zero.

| Width | First spanning event |
| ---: | --- |
| 512 | `bridge_io column_i=2 tile_j=94882 lhs=7/3 rhs=8/5` |
| 1024 | `bridge_lr column_i=2 tile_j=94883 lhs=8/4 rhs=13/7` |
| 2048 | `bridge_lr column_i=4 tile_j=94885 lhs=32/20 rhs=41/6` |
| 4096 | `bridge_lr column_i=14 tile_j=94890 lhs=231/1 rhs=248/2` |
| 8192 | `bridge_lr column_i=14 tile_j=94890 lhs=439/1 rhs=472/2` |
| 16384 | `bridge_lr column_i=83 tile_j=94903 lhs=5353/32 rhs=5418/2` |
| 32768 | `bridge_lr column_i=83 tile_j=94903 lhs=10601/32 rhs=10730/2` |
| 65536 | `bridge_lr column_i=2463 tile_j=94883 lhs=632867/18 rhs=633124/2` |
| 131072 | `bridge_lr column_i=19422 tile_j=93010 lhs=10035501/6 rhs=10036025/1` |

## Radius Translation Sweep

Widths: `8192` and `32768`.

Radius shifts:
`-32768, -16384, -8192, -4096, -2048, -1024, -512, 0, 512, 1024, 2048, 4096, 8192, 16384, 32768`.

All 30 shifted runs returned `VERDICT: SPANNING`; all overflow counters were
zero.

Repeated stable witnesses:

- Width `8192`, shifts `-2048, -1024, -512, 0`: first bridge stays near
  `column_i=14 tile_j=94890`.
- Width `32768`, shifts `-16384, -8192, -4096, -2048, -1024, -512, 0, 512,
  1024, 2048`: first bridge stays at `column_i=83 tile_j=94903`.

## First Full-Shell Source Trace

Command shape:

```bash
campaign_main_cuda --k-sq=34 \
  --r-inner=24289452 --r-outer=24297644 \
  --region full-octant --chunk-size=200000 \
  --no-early-exit --timing --trace-spanning
```

Result:

```text
active tiles: 2479915
produced tiles: 2479915
ingested tiles: 2479915
ingested columns: 67114
cuda-k1-k5: 21.334 s
compositor: 5.393 s
total: 27.678 s
VERDICT: SPANNING
SPANNING_TRACE: event=bridge_lr column_i=14 tile_j=94890
lhs_tile_index=439 lhs_group_label=1
rhs_tile_index=472 rhs_group_label=2
reach_before=1 reach_after=3 added_bits=2
inner_source_tile_index=462 inner_source_group_label=6
outer_source_tile_index=296 outer_source_group_label=2
```

The final bridge groups themselves are not directly geo-flagged:

- LHS bridge tile `13,94890`, label `1`: `geo_bits=0`, `tile_inner=0`,
  `tile_outer=0`.
- RHS bridge tile `14,94890`, label `2`: `geo_bits=0`, `tile_inner=0`,
  `tile_outer=0`.

The bridge is therefore where two already-grown reach components meet, not
where the boundary flags are first introduced.

## Boundary Source Dumps

Inner source:

```text
DUMP_TILE global_idx=462 i=14 j=94880 a_lo=3584 b_lo=24289280
prime_count=903 max_label=28 overflow=0 tile_flags=0
GROUP label=6 geo_bits=1 tile_inner=1 tile_outer=0
face_I count=0 label_6_ordinals=-
face_O count=23 label_6_ordinals=6,9,10
face_L count=7 label_6_ordinals=2,4
face_R count=6 label_6_ordinals=-
```

Outer source:

```text
DUMP_TILE global_idx=296 i=8 j=94912 a_lo=2048 b_lo=24297472
prime_count=1836 max_label=20 overflow=0 tile_flags=0
GROUP label=2 geo_bits=2 tile_inner=0 tile_outer=1
face_I count=15 label_2_ordinals=2,4,6,7,8,14,15
face_O count=0 label_2_ordinals=-
face_L count=9 label_2_ordinals=3,4,6,7,8,9
face_R count=16 label_2_ordinals=1,2,3,5,7,11,12,14,15,16
```

Both source tiles passed full K1-K5 CPU/CUDA TileOp parity:

```text
cuda_vs_cpu_diff: 1 tile(s) matched K1-K5 full TileOp parity
cuda_vs_cpu_diff: 1 tile(s) matched K1-K5 full TileOp parity
```

## Verification

- `git diff --check`: PASS.
- Remote CUDA build: PASS.
- K34 full source trace on RTX 4090: `VERDICT: SPANNING`, zero overflows.
- K34 CUDA CTest on RTX 4090: 13/13 passed.
- K34 CPU CTest on RTX 4090: 113/113 passed when run serially; CTest reported
  `GeoTests.NonSquareKUsesCeilBoundary` skipped. The first parallel CPU CTest
  run had a wall-clock-only failure in `Grid.ScaleBuildCompletesUnder500ms`
  under test contention; the same test passed serially in `0.22 s`.
- Local CPU configure was not used as evidence because this Mac environment is
  missing OpenMP for that build.

## Interpretation

The current evidence lowers the probability of a narrow CUDA/compositor bug:

- K34 static-annulus spanning is robust over a width ladder from `512` through
  `131072`.
- It is also robust under the tested radius translations for widths `8192` and
  `32768`.
- The first full-shell spanning bridge is explained by concrete reach sources:
  one source group is genuinely inner-flagged and one source group is genuinely
  outer-flagged.
- The source TileOps agree with CPU on full K1-K5 parity and have zero overflow.

This still does not prove a mathematical regime change between K34 and K36.
What it does prove is narrower and more useful: the observed K34 static-annulus
SPANNING result is not currently explained by width choice, radius alignment,
overflow contamination, final-bridge local geo flags, or CUDA/CPU TileOp drift
on the boundary source tiles.

## Next Bug-Hunt Edge

The next useful falsifier is path provenance, not more scalar parity:

1. Trace the face-port chain from `inner_source_tile_index=462/group=6` to the
   bridge and from `outer_source_tile_index=296/group=2` to the bridge.
2. Verify each stitch edge by ordinal on the dumped path.
3. If every edge is locally valid, the static-annulus detector is likely doing
   what it says, and the remaining mismatch is semantic: static annulus
   spanning is not the same question as the informal origin-moat expectation.
