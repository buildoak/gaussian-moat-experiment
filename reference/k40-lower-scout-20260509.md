# K40 Lower Scout - 2026-05-09

## Scope

This records the first current-gate `K_SQ=40`, `W=32768` scout below the
archived K40 `978M SPANNING -> 980M MOAT` static-annulus bracket.

Generated bundle:

```text
results/k40-lower-scout-20260509/
```

Remote source:

```text
/workspace/runs/k40-lower-scout-20260509
```

Claim semantics:

- `SPANNING` = `ANY-SPAN` from `geo_I` to `geo_O` in the tested static annulus.
- `MOAT` = `ANY-SHELL-MOAT` in the tested static annulus.
- This campaign is diagnostic static-annulus evidence, not an origin-component
  or global Tsuchimura-style proof.

## Generator

```text
commit=fc52edce6f48d312220f1f4082fc17dee332be56
GPU=NVIDIA GeForce RTX 4090, driver 535.230.02
K_SQ=40, S=256, C=6
W=32768
region=full-octant
chunk_size=200000
early_exit=disabled
telemetry=audit
tile_sample_count=512
```

The K40 binary was built remotely under:

```text
tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k40/campaign_main_cuda
```

Runtime CUDA BZ exactness is not an accepted proof surface for non-square K.
Rows used `--allow-uncertified-boundary-band`, and each row separately ran
external `bz_check.py --k-sq 40 --r-inner <R> --r-outer <R+32768>`.

## Gates Run

All 12 data rows passed the diagnostic full-ingest gate:

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
K40: 12 SPANNING, 0 MOAT
bad rows: 0
total measured row runtime: 1.028h
```

## Main Finding

K40 erased the refined K36/K37-K39 lower static-annulus moat island at
`W=32768`.

| Role | R_inner | R_outer | K36/K39 pattern | K40 verdict |
|---|---:|---:|---|---|
| deep lower control | 60,000,000 | 60,032,768 | `SPANNING` control | `SPANNING` |
| lower control | 72,625,000 | 72,657,768 | `SPANNING` | `SPANNING` |
| last span before island | 72,739,496 | 72,772,264 | `SPANNING` | `SPANNING` |
| first moat | 72,739,560 | 72,772,328 | `MOAT` | `SPANNING` |
| interior moat | 72,739,688 | 72,772,456 | `MOAT` | `SPANNING` |
| last moat | 72,740,648 | 72,773,416 | `MOAT` | `SPANNING` |
| first span after island | 72,740,712 | 72,773,480 | `SPANNING` | `SPANNING` |
| prior sentinel moat | 72,875,000 | 72,907,768 | `MOAT` | `SPANNING` |
| older K36 span endpoint | 73,339,843 | 73,372,611 | `SPANNING` | `SPANNING` |
| older K36 moat endpoint | 73,359,375 | 73,392,143 | `MOAT` | `SPANNING` |
| K38 moat endpoint | 73,437,500 | 73,470,268 | `MOAT` | `SPANNING` |
| coarse upper probe | 75,000,000 | 75,032,768 | coarse probe | `SPANNING` |

The new K40 lattice steps are therefore sufficient to reconnect all tested
K36/K38 moat annuli between 72.739M and 75M.

## Interpretation

This does not prove there is no lower K40 moat below the archived `980M` row.
It rules out the specific current-gate sentinels and the transferred K36 island
geometry at `W=32768`.

The result makes the next step a sparse upward search, not further densification
around 72.74M.

## Next Gates

Completed follow-up is recorded in
`reference/k40-current-gate-campaign-20260509.md`.

Tier 2 sparse sentinels:

```text
90M, 110M, 140M, 180M, 230M, 300M, 390M, 500M, 650M, 780M
```

all returned clean `SPANNING`. The high bracket was then rerun at current gate:

```text
979M SPANNING -> 980M MOAT
```
