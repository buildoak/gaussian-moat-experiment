# K37-K39 Scout At K36 Lowest Moat - 2026-05-08

## Scope

This records a diagnostic `K_SQ=37`, `38`, and `39` scout at the refined
`K_SQ=36`, `W=32768` static-annulus moat island near `R_inner=72,740,000`.

Local generated bundle:

```text
results/k37-k39-k36-lowest-moat-scout-20260508/
```

Remote source:

```text
/workspace/runs/k37-k39-k36-lowest-moat-scout-20260508
```

Claim semantics:

- `SPANNING` = `ANY-SPAN` from `geo_I` to `geo_O` in the tested annulus.
- `MOAT` = `ANY-SHELL-MOAT` in the tested annulus.
- This is diagnostic static-annulus evidence, not an origin-component or
  global Tsuchimura-style proof.

## Generator

```text
commit=fc52edce6f48d312220f1f4082fc17dee332be56
GPU=NVIDIA GeForce RTX 4090, driver 535.230.02
K_SQ=37/38/39, S=256, C=6
W=32768
region=full-octant
chunk_size=200000
early_exit=disabled
telemetry=audit
tile_sample_count=512
```

Runtime CUDA BZ exactness is not an accepted proof surface for these non-square
K values. Rows used `--allow-uncertified-boundary-band`, and each row separately
ran external `bz_check.py --k-sq <K> --r-inner <R> --r-outer <R+32768>`.

The remote `bz_config.json` was patched only to make K37-K39 build anchors pass
CMake BZ gates. The per-row BZ logs in the result folder are the relevant
evidence.

## Gates Run

All 21 data rows passed the diagnostic full-ingest gate:

```text
external bz_check.py rc=0
run_rc=0
sample_check_rc=0
produced == ingested == active
overflow_total=0
emitted_overflow=0
sample_checked=512
status=diagnostic_full_clean
```

Row inventory:

```text
K37: 4 MOAT, 3 SPANNING
K38: 4 MOAT, 3 SPANNING
K39: 4 MOAT, 3 SPANNING
bad rows: 0
total measured row runtime: 1.873h
```

## Main Finding

K37, K38, and K39 all preserved the K36 pattern on the probed annuli:

| Role | R_inner | R_outer | K37 | K38 | K39 |
|---|---:|---:|---|---|---|
| last span before island | 72,739,496 | 72,772,264 | `SPANNING` | `SPANNING` | `SPANNING` |
| first moat | 72,739,560 | 72,772,328 | `MOAT` | `MOAT` | `MOAT` |
| interior moat | 72,739,688 | 72,772,456 | `MOAT` | `MOAT` | `MOAT` |
| last moat | 72,740,648 | 72,773,416 | `MOAT` | `MOAT` | `MOAT` |
| first span after island | 72,740,712 | 72,773,480 | `SPANNING` | `SPANNING` | `SPANNING` |
| prior sentinel moat | 72,875,000 | 72,907,768 | `MOAT` | `MOAT` | `MOAT` |
| lower control span | 72,625,000 | 72,657,768 | `SPANNING` | `SPANNING` | `SPANNING` |

This means the refined K36 island is not immediately erased by adding the
squared-distance 37 lattice steps. K38 and K39 add no new Gaussian lattice step
lengths beyond K37, so their agreement is expected and mainly confirms the
build/config path.

## Next Gates

1. Probe `K_SQ=40`, where new lattice steps appear.
2. If K40 still preserves the moat, refine the same island edges for K40.
3. Keep non-square K rows labeled diagnostic unless runtime BZ/certificate
   support is promoted to the accepted verification spine.
