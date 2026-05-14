# K26-K36 Telemetry Calibration Pilot - 2026-05-08

## Scope

This records the 25-row lower-K telemetry calibration pilot used to test
whether known static-annulus moat regions can teach a useful triage signal for
K40 search.

Local generated bundle:

```text
results/k26-k36-telemetry-calibration-20260508/
```

Remote source:

```text
/workspace/runs/k26-k36-telemetry-calibration-20260508
```

Claim semantics:

- `SPANNING` = `ANY-SPAN` from `geo_I` to `geo_O` in the tested annulus.
- `MOAT` = `ANY-SHELL-MOAT` in the tested annulus.
- This is static-annulus detector/sample telemetry, not an origin-component
  proof.

## Generator

```text
commit=fc52edce6f48d312220f1f4082fc17dee332be56
GPU=NVIDIA GeForce RTX 4090, driver 535.230.02
K_SQ=26/32/34/36
region=full-octant
chunk_size=200000
early_exit=disabled
telemetry=audit
tile_sample_count=512
```

For non-square `K=26/32/34`, rows used runtime
`--allow-uncertified-boundary-band` plus external `bz_check.py`. For `K=36`,
rows used `bz_exact_check` and no runtime BZ override.

## Gates Run

All 25 data rows passed the calibration gate:

```text
bz_rc=0
run_rc=0
produced == ingested == active
overflow_total=0
emitted_overflow=0
early_exit=0
sample check passed for 512 sampled TileOps
status=calibration_clean
```

Inventory:

```text
K26: 3 MOAT, 3 SPANNING
K32: 3 MOAT, 3 SPANNING
K34: 2 MOAT, 3 SPANNING
K36: 4 MOAT, 4 SPANNING

positive_moat: 12
near_span:      8
far_span:       5
bad rows:       0
```

The K26 endpoint span row again hit the known manifest-quota issue: manifest
mode rejected, while samples-only independent regeneration checked `512/512`.
This caveat is explicit in `summary.tsv`.

## Row Classes

The pilot intentionally uses three labels:

| Class | Meaning |
|---|---|
| `positive_moat` | known clean static-annulus `MOAT` row |
| `near_span` | adjacent or bracket-near clean `SPANNING` row |
| `far_span` | lower/far control `SPANNING` row |

This is important because current telemetry appears better at detecting
"near-region" pressure than distinguishing `MOAT` from adjacent `SPANNING`.

## First-Pass Model Check

After harvest, a simple leave-one-K-out linear scorecard was tested from
`features.tsv`.

Result:

- as a **near-region vs far-span** ranker, the telemetry has useful signal for
  K26, K32, and K36;
- as a **MOAT vs adjacent SPAN** classifier, the telemetry is weak and
  interleaves adjacent spans with moat rows.

Holding out K36, the far-span controls ranked below the refined-island rows,
but the adjacent `SPANNING` rows and `MOAT` rows were not cleanly separable.

## Interpretation

This supports the original idea only in the narrower form:

```text
Use telemetry to rank K40 regions worth probing.
Do not use it to predict a moat verdict.
```

The current `stats_v2` features are mostly pressure and population features:
group/port distributions, high-pressure tiles, geo boundary populations, and
run health. They are not yet direct fragility features. The biggest missing
signals are:

- boundary component sizes for full-ingest rows;
- frontier persistence and component survival across columns;
- near-connection/bottleneck distance proxies;
- span-path geometry for `SPANNING` rows;
- angular or radial low-bridge-density gaps.

## Next Gate

Run a small K40 scout before any 100-run campaign:

1. produce candidate K40 windows;
2. score them with the near-region score from this pilot;
3. run 10-12 top-ranked candidates plus 4-6 baseline controls;
4. continue only if top-ranked rows beat controls on near-region telemetry or
   produce clean `MOAT` rows.
