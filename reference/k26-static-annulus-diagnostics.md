# K26 Static-Annulus Diagnostics

Updated: 2026-05-06.

This note records the current K26 diagnostic campaign around Tsuchimura's
published `sqrt(26)` origin-component endpoint. It is a static-annulus detector
campaign, not an origin-component proof.

## Tsuchimura Anchor

Tsuchimura reports the farthest point in the `sqrt(26)` origin-connected
component as:

```text
943460 + 376039i
```

Its norm data:

```text
943460^2 + 376039^2 = 1031522101121
sqrt(1031522101121) = 1015638.765...
ceil radius = 1015639
```

Tsuchimura's statement is about the component reachable from the origin.
Current CUDA rows are static-annulus rows:

- `SPANNING` means some component connects `geo_I` to `geo_O` inside the
  tested annulus.
- Full-ingest `MOAT` means no component connects `geo_I` to `geo_O` inside the
  tested annulus.

Therefore, static-annulus rows can sit very close to the published origin
endpoint without proving or refuting the origin-component result.

## Main Fixed-Inner Diagnostic

At the endpoint-adjacent fixed inner radius:

```text
R_inner = 1015639
```

the exact observed static-annulus transition is:

| Role | W | R_outer | Verdict |
|---|---:|---:|---|
| Last spanning row | 22682 | 1038321 | `SPANNING` |
| First moat row | 22683 | 1038322 | `MOAT` |

All rows in the transition pin were full-ingest and overflow-clean. External
non-square-K BZ auditing passed for the transition rows. One nearby row,
`W=22766`, failed external BZ due to a Gaussian-prime norm in `BZ_O` and must
remain rejected/unclean evidence.

## R_inner = 950000 Reconstruction

The most interpretable static-annulus reconstruction starts lower:

```text
R_inner = 950000
```

Tsuchimura's endpoint radius corresponds to:

```text
1015638.765... - 950000 = 65638.765...
```

The pinned static-annulus transition at this inner radius is:

| Role | W | R_outer | Verdict |
|---|---:|---:|---|
| Last spanning row | 65643 | 1015643 | `SPANNING` |
| First moat row | 65644 | 1015644 | `MOAT` |

This places the static-annulus transition about `4.235` to `5.235` radius units
above Tsuchimura's reported endpoint radius. That is close enough to be a useful
implementation sanity signal, while still remaining a different claim type.

## Hard Negatives And Radial Sensitivity

The radial hard-negative campaign tested 244 unique geometries around and below
the endpoint anchor:

```text
SPANNING rows: 216
MOAT rows:      28
BZ failures:     4
overflow failures: 0
```

The deep lower block, 40k to 100k below the anchor, behaved as desired: all rows
were `SPANNING`, including near-edge widths up to `W=22682` where sampled.
Four deep/lower rows failed external BZ and are retained only as unclean
diagnostics.

Positive radial translations showed real static-annulus sensitivity. Rows below
the fixed-anchor `W=22683` moat can become `MOAT` after translating `R_inner`
outward:

| delta from 1015639 | R_inner | First observed MOAT W | Last observed SPANNING W |
|---:|---:|---:|---:|
| +256 | 1015895 | 22528 | 22016 |
| +512 | 1016151 | 22528 | 22016 |
| +1024 | 1016663 | 22016 | 21504 |
| +2048 | 1017687 | 22016 | 21504 |
| +16384 | 1032023 | 22016 | 20480 |
| +32768 | 1048407 | 16384 | 12288 |

This is not a contradiction of Tsuchimura; it is the static-annulus detector
showing that disconnected shell components and radial placement matter.

## Sample-Audit Status

Representative K26 sampled rows have passed independent regeneration:

| Row | Verdict | Sample check |
|---|---|---|
| `R_inner=1015639, W=22682` | `SPANNING` | 512 TileOps match independent regeneration; manifest has a diagonal quota metadata issue. |
| `R_inner=1015639, W=22683` | `MOAT` | 512 TileOps pass with manifest. |
| `R_inner=915639, W=22682` | `SPANNING` | 512 TileOps pass with manifest. |
| `R_inner=1015895, W=22528` | `MOAT` | 512 TileOps pass with manifest. |
| `R_inner=1048407, W=16384` | `MOAT` | 512 TileOps pass with manifest. |

The `R_inner=1015639, W=22682` manifest issue is a sampler accounting problem:
one diagonal tile is selected under `geo_I`, so the manifest reports 62
diagonal-labeled samples even though all 63 semantic diagonal tiles are covered.
The sampled TileOps themselves match the independent checker.

## Evidence Locations

Bulky evidence is local and ignored under `results/`:

- `results/k26-tsuchimura-endpoint-20260506/`
- `results/k26-radial-hard-negatives-20260506/`
- `results/k26-r1000000-width-sweep-20260506/`
- `results/k26-r950000-width-sweep-20260506/`

These folders contain generator descriptions, summaries, logs, profiles, BZ
logs, sampled TileOps, sample manifests, and independent checker logs.

The K26 rows are diagnostic until the non-square-K BZ story is fully integrated
into runtime/postflight acceptance. For non-square `K`, current CUDA runtime BZ
records are not accepted authority because the built-in exact BZ guard only
certifies square `K`; external `bz_check.py` logs carry the diagnostic BZ
evidence.

## Next Meaningful Lower-K Target

For Gaussian-prime differences, allowed vectors satisfy:

```text
x^2 + y^2 <= K
x == y (mod 2)
```

Under this rule, `K=26`, `K=28`, and `K=30` have the same allowed edge-vector
set. Running `sqrt(28)` or `sqrt(30)` is useful as non-square plumbing and
regression coverage, but it is not a new graph target.

The next meaningful lower-K moat target is `K=32`, because it adds the `(4,4)`
edge vector. Tsuchimura reports an exact `sqrt(32)` endpoint:

```text
2106442 + 1879505i
radius ~= 2823054.542
```

So the next campaign should be K32, with the same separation of claim types:
static-annulus diagnostics first, then only later origin-component machinery if
we build it.

