---
title: TileOp v3 Sizing Study
date: 2026-04-20
engine: codex
type: report
status: complete
refs: [methodology/lemmas_v2/campaign-blueprint.md, methodology/lemmas_v2/tile-operator-definition-v-claude.md, tools/sizing/tileop_sizing_study.py]
---

## Scope

Implemented and ran `tools/sizing/tileop_sizing_study.py` against the v3 snapped-grid rules:

- tile proper is the closed `257 x 257` lattice square;
- `S = 256`, offset `(0, 0)`;
- collar/halo `C = floor(sqrt(K)) = 6`;
- Gaussian primes are enumerated by norm-form rational primality using `gmpy2`;
- `G_tile` DSU runs over the full halo;
- ports are connected components of each face-strip graph and ordered by canonical lex `(h, p_perp)`.

Run command:

```bash
uv run tools/sizing/tileop_sizing_study.py --default-regimes --r-halfwidth 4096 --n-tiles 100 --offset-x 0 --offset-y 0 --output-prefix results/tileop_sizing_study_2026-04-20 --progress 25
```

Artifacts:

- `results/tileop_sizing_study_2026-04-20.json`
- `results/tileop_sizing_study_2026-04-20.csv`

`dsu_groups` below is all halo DSU components. `max_label` is the compacted count of distinct `G_tile` UF roots referenced by at least one face port, which is the relevant TileOp flag-label budget. P99.9 with 100 samples is effectively near-max interpolation, not a true tail estimate.

## Percentiles

### R = 60M

| K | metric | P50 | P90 | P99 | P999 | max |
|---|---:|---:|---:|---:|---:|---:|
| 36 | primes | 2572 | 2628 | 2654.2 | 2670.2 | 2672 |
| 36 | dsu_groups | 100 | 118 | 126.1 | 132.3 | 133 |
| 36 | ports_I/O/L/R max | 18 | 23 | 27.0 | 29.5 | 30 |
| 36 | sum(n) | 74 | 82 | 90.0 | 90.0 | 90 |
| 36 | max_label | 41 | 52 | 58.0 | 58.9 | 59 |
| 40 | primes | 2576 | 2636 | 2653.1 | 2661.1 | 2662 |
| 40 | dsu_groups | 48 | 59 | 77.0 | 77.9 | 78 |
| 40 | ports_I/O/L/R max | 14 | 18 | 24.0 | 24.9 | 25 |
| 40 | sum(n) | 57 | 64 | 68.0 | 69.8 | 70 |
| 40 | max_label | 23 | 30 | 33.0 | 34.8 | 35 |

### R = 80M

| K | metric | P50 | P90 | P99 | P999 | max |
|---|---:|---:|---:|---:|---:|---:|
| 36 | primes | 2531 | 2588 | 2635.2 | 2653.0 | 2655 |
| 36 | dsu_groups | 110 | 125 | 140.1 | 144.5 | 145 |
| 36 | ports_I/O/L/R max | 19 | 24 | 28.0 | 28.9 | 29 |
| 36 | sum(n) | 74 | 83 | 88.0 | 90.7 | 91 |
| 36 | max_label | 45 | 53 | 60.0 | 60.9 | 61 |
| 40 | primes | 2537 | 2581 | 2600.7 | 2662.2 | 2669 |
| 40 | dsu_groups | 52 | 61 | 72.1 | 81.0 | 82 |
| 40 | ports_I/O/L/R max | 15 | 20 | 23.0 | 24.8 | 25 |
| 40 | sum(n) | 58 | 66 | 73.0 | 73.0 | 73 |
| 40 | max_label | 24 | 32 | 35.0 | 35.0 | 35 |

### R = 800M

| K | metric | P50 | P90 | P99 | P999 | max |
|---|---:|---:|---:|---:|---:|---:|
| 36 | primes | 2250 | 2302 | 2343.2 | 2360.1 | 2362 |
| 36 | dsu_groups | 168 | 190 | 198.0 | 198.0 | 198 |
| 36 | ports_I/O/L/R max | 22 | 26 | 31.0 | 32.8 | 33 |
| 36 | sum(n) | 87 | 94 | 103.0 | 103.9 | 104 |
| 36 | max_label | 62 | 71 | 80.0 | 80.0 | 80 |
| 40 | primes | 2246 | 2287 | 2320.0 | 2323.6 | 2324 |
| 40 | dsu_groups | 92 | 105 | 119.0 | 120.8 | 121 |
| 40 | ports_I/O/L/R max | 17 | 21 | 25.0 | 27.7 | 28 |
| 40 | sum(n) | 67 | 77 | 82.1 | 88.3 | 89 |
| 40 | max_label | 38 | 48 | 53.1 | 61.1 | 62 |

## Budget Findings

For a hypothetical compact 128 B v3-like format, I used 96 port bytes and 64 group flag labels as the budget. Under that assumption, 128 B fits all sampled P99/P999 at 60M and 80M for both K values. It fails by R = 800M, K = 36: `sum(n)` P99 is `103.0` and `max_label` P90 is already `71`, with 40/100 sampled tiles overflowing.

The 256 B v3 budget is slack in every sampled regime. Worst observed tile was `sum(n) = 104` versus budget `192`, and `max_label = 80` versus budget `128`. No 256 B overflow occurred in 600 sampled tiles. Overflow onset was not observed for 256 B through R = 800M.

K = 40 has roughly the same prime count as K = 36 but substantially fewer components and ports because the larger edge radius connects more local clusters. At R = 800M, K = 40 max `sum(n)` was `89` versus `104` for K = 36, and max `max_label` was `62` versus `80`.

For project scale R around 80M, the expected 256 B overflow rate is effectively zero in this sample: 0/200 tiles across K = 36 and K = 40. The simple 95% rule-of-three upper bound is about 1.5% for the combined 80M sample, and 3% per individual K, but observed headroom is large: K = 36 max `sum(n)=91`, `max_label=61`; K = 40 max `sum(n)=73`, `max_label=35`. This does not reproduce the v1 observed 22% K = 36 overflow; the v3 snapped/canonical port budget appears materially less stressed.

## Recommendation

Use the single 256 B TileOp v3 format. It has large empirical headroom at R ~ 80M and remains overflow-free in this sample at R = 800M. A 128 B plus overflow path looks viable at R ~ 80M in this small sample, but it is brittle for K = 36 at larger R and would preserve dual-path complexity that v3 is trying to remove.
