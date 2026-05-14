# K34 Tsuchimura-Bound Campaign Plan

Date: 2026-05-07

## Objective

Test `sqrt(34)` at Tsuchimura's published bound using the same style as the
recent K32 Tsuchimura-radius probes: fixed-width annuli whose outer boundary is
placed at or near the reported level, then shifted outward/inward to bracket the
static-annulus transition.

Tsuchimura reports for `sqrt(34)`:

```text
origin component: finite
farthest distance: < 24289452
```

Unlike K32, this is an upper bound rather than a reported exact farthest
Gaussian integer. The campaign anchor is therefore:

```text
R0 = 24289452
```

Claim semantics remain static-annulus only:

- `SPANNING` means some component crosses the tested annulus.
- `MOAT` means no component crosses the tested annulus.
- Neither verdict is an origin-component proof.

## Known Baseline

A prior corrected Tsuchimura-bound probe put the bound on the outer boundary:

```text
K=34
R_inner=24281260
R_outer=24289452
W=8192
verdict=SPANNING
overflow=0
```

That row should be rerun under the current audit/sample gate as the first
sanity check.

## Mini Campaign

Run fixed-outer-boundary probes around `R0=24289452`.

Widths:

```text
8192 32768 65536 100000
```

Offsets from `R0`:

```text
-128 0 16 32 64 128 512 1024
```

For each row:

```text
R_outer = 24289452 + offset
R_inner = R_outer - W
```

This is 32 primary rows. The expected first useful signal is whether the
transition is already near offset `0`, as it was for K32, or whether K34 still
spans far above the published origin-component bound.

Run contract:

```text
--k-sq=34
--r-inner=<R_outer - W>
--r-outer=<24289452 + offset>
--region full-octant
--chunk-size=200000
--no-early-exit
--timing
--telemetry profile
--overflow-diagnostics
--allow-uncertified-boundary-band
--profile <run>/profiles/<label>.profile.json
```

Every row must also run external BZ:

```text
python3 tiles-maxxing/cpp-campaign-v2/scripts/bz_check.py \
  --k-sq 34 --r-inner <R_inner> --r-outer <R_outer>
```

Acceptance for primary rows:

- `bz_rc == 0`;
- `run_rc == 0`;
- `produced == ingested == active`;
- `overflow_total == 0`;
- `emitted_overflow == 0`;
- clean status in the run summary.

## Boundary Audits

After the primary matrix, select boundary pairs:

- for each width, last clean `SPANNING`;
- for each width, first clean `MOAT`, if a `MOAT` appears.

Rerun selected rows with:

```text
--telemetry audit
--tile-sample-count=512
--sample-manifest <run>/sampled-audits/<label>.sample-manifest.json
--tile-sample-out <run>/sampled-audits/<label>.samples.jsonl
```

Then check with:

```text
verification/build/tile_sample_check \
  --samples <samples.jsonl> \
  --manifest <sample-manifest.json>
```

SPANNING rows should also get span certificates if the current binary supports
that cleanly for this campaign shape. If not, label them detector/sample-audit
rows rather than accepted SPAN-proof rows.

## Expansion If Needed

If all offsets through `+1024` remain `SPANNING`, widen the offset ladder before
increasing widths:

```text
2048 4096 8192 16384 32768 65536
```

If the transition is found within the mini ladder, refine with smaller offsets
around the bracket, using the K32 pattern:

```text
last SPANNING offset = a
first MOAT offset = b
search offsets inside (a, b]
```

If `W=100000` gives a clean near-bound bracket, add wide-width confirmation:

```text
W = 150000 200000 250000
offsets = -128 0 16 32 48 64 96 128
```

This mirrors the K32 wide-width run and gives the cleanest distance-to-bound
statement.

## Result Bundle

Create:

```text
results/k34-tsuchimura-bound-20260507/
```

with:

- `README.md` containing the generator, exact anchor, run contract, caveats,
  verdict matrix, and distance from `R0`;
- `summary.tsv`;
- `sampled-audits/summary.tsv`;
- BZ logs, CUDA logs, profiles, manifests, samples, and sample-check logs.

Update `results/README.md` after the bundle is pulled and locally verified.

Only update `reference/attached-static-annulus-moats.md` if the rows pass the
current rerun gate and are labeled as static-annulus evidence, not
Tsuchimura-origin proof.
