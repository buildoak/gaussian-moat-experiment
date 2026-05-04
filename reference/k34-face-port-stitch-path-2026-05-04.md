# K34 Face-Port Stitch-Path Provenance - 2026-05-04

Goal: report the actual face-port stitch chain behind the first K34
static-annulus spanning witness, without changing production verdict semantics
or implementing an origin-component verifier.

## Patch

The streaming compositor now has an opt-in path provenance mode exposed by
`campaign_main_cuda --trace-spanning-path`.

- Records every `bridge_io` / `bridge_lr` stitch edge while path tracing is
  enabled.
- Vertices are `(tile_index, group_label)`.
- Edge payload records event, lhs/rhs tile/group, lhs/rhs face, and exact
  ordinal used for the `unite()`.
- On first spanning, reconstructs both pre-bridge paths while excluding the
  final bridge edge, then emits the compact stdout summary and structured
  profile JSON.
- Adds a host-side `emitted_overflow_bit_count` over returned TileOps, separate
  from CUDA overflow summary counters.

## Artifacts

Remote artifact root:
`/workspace/gaussian-moat-cuda-k34-gauntlet-20260504T010013Z/tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/k34-deeper-20260504T012500Z/`

Local copied artifact root:
`tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/k34-deeper-20260504T012500Z/`

- `R24289452_shell_spanning_path.log`
- `R24289452_shell_spanning_path.profile.json`
- `R24289452_shell_spanning_path.validation.json`

Report predecessor: `reference/k34-deeper-static-annulus-2026-05-04.md`.

## 4090 Witness Command

```bash
tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k34/campaign_main_cuda \
  --k-sq=34 \
  --r-inner=24289452 \
  --r-outer=24297644 \
  --region full-octant \
  --chunk-size=200000 \
  --no-early-exit \
  --timing \
  --trace-spanning \
  --trace-spanning-path \
  --overflow-diagnostics \
  --profile tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/k34-deeper-20260504T012500Z/R24289452_shell_spanning_path.profile.json
```

Result:

- `VERDICT: SPANNING`
- active/produced/ingested tiles: `2479915`
- ingested columns: `67114`
- app batches: `13`
- wall time: `real 29.01`
- CUDA K1-K5: `21.8109s`
- compositor: `6.08502s`

First spanning trace:

```text
event=bridge_lr column_i=14 tile_j=94890
lhs_tile_index=439 lhs_group_label=1
rhs_tile_index=472 rhs_group_label=2
inner_source_tile_index=462 inner_source_group_label=6
outer_source_tile_index=296 outer_source_group_label=2
```

## Stitch Path

Structured path reconstruction:

- recorded stitch edges before first spanning: `17107`
- inner source: `462/6`
- inner endpoint: `472/2`
- inner path length: `20` edges
- outer source: `296/2`
- outer endpoint: `439/1`
- outer path length: `61` edges
- final bridge: `bridge_lr 439/1:R#10 <-> 472/2:L#10`

Representative inner path edges:

```text
0:  bridge_io 462/6:O#6  <-> 463/4:I#6
1:  bridge_lr 430/4:R#3  <-> 463/4:L#3
2:  bridge_io 430/4:O#1  <-> 431/1:I#1
18: bridge_lr 438/4:R#8  <-> 471/2:L#8
19: bridge_io 471/2:O#2  <-> 472/2:I#2
```

Representative outer path edges:

```text
0:  bridge_io 295/4:O#2   <-> 296/2:I#2
1:  bridge_io 294/5:O#11  <-> 295/4:I#11
2:  bridge_io 293/5:O#12  <-> 294/5:I#12
59: bridge_lr 373/20:R#12 <-> 406/20:L#12
60: bridge_lr 406/20:R#4  <-> 439/1:L#4
```

Validation artifact `R24289452_shell_spanning_path.validation.json` reports:

- `validation_errors: []`
- max tile index checked: `472`
- every path edge has equal lhs/rhs ordinal
- every `bridge_io` edge has `O -> I` adjacent tile relation
- every `bridge_lr` edge has `R -> L` adjacent tile relation
- both reconstructed paths are continuous from source to endpoint
- final bridge endpoints match the reconstructed endpoint pair

## Overflow Sanity Gate

The K34 witness was run with `--overflow-diagnostics`, so overflow counters were
computed from copied device arrays rather than the summary kernel.

Overflow counters:

```text
k1_cand_overflow_count=0
k4_prime_overflow_count=0
k4_group_overflow_count=0
k5_port_overflow_count=0
emitted_overflow_bit_count=0
```

Interpretation:

- Summary-counter-wrong hypothesis: not supported by this run; diagnostics mode
  used the independent copied-array path and still returned zero.
- Actual `OVERFLOW_BIT` emitted hypothesis: not supported; host-side count over
  returned TileOps is zero.
- Missed-overflow malformed non-overflow TileOp hypothesis: not falsified solely
  by overflow counters, but this path witness itself is ordinal-coherent and
  face-adjacent. Existing source-group CPU/CUDA parity from the predecessor
  report remains the relevant evidence against malformed source TileOps.

## Gates

Local:

```text
git diff --check
```

Result: passed, no output.

```text
cmake --build tiles-maxxing/cpp-campaign-v2/build --target test_streaming_compositor -j ...
./tiles-maxxing/cpp-campaign-v2/build/tests/test_streaming_compositor
```

Result: built and passed `11/11` tests.

Vast 4090 build:

```text
cmake --build tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k34 \
  --target campaign_main_cuda cuda_vs_cpu_diff -j $(nproc)
```

Result: passed.

Vast 4090 CUDA CTest:

```text
cd tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k34
ctest --output-on-failure -j $(nproc)
```

Result: `100% tests passed, 0 tests failed out of 13`, total `48.49s`.

Vast 4090 focused CPU compositor test:

```text
cmake --build tiles-maxxing/cpp-campaign-v2/build-k34-gauntlet \
  --target test_streaming_compositor -j $(nproc)
tiles-maxxing/cpp-campaign-v2/build-k34-gauntlet/tests/test_streaming_compositor
```

Result: built and passed `11/11` tests.

## Conclusion

This evidence supports a coherent static-annulus witness, not an invalid stitch.
The first K34 full-shell bridge remains `439/1 <-> 472/2`; the new provenance
adds the exact face-port ordinals (`R#10 <-> L#10`) and reconstructs continuous,
ordinal-matched source-to-bridge chains from both reach sources. The result does
not prove the informal origin-moat expectation wrong; it narrows this specific
K34 artifact to a coherent static-annulus spanning witness rather than overflow
contamination or a face-port ordinal mismatch.
