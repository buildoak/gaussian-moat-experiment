# sqrt(34) Gate Feasibility

Updated: 2026-05-03.

## Result

The naive Tsuchimura-derived `sqrt(34)` annulus probe is rejected as a
correctness gate.

Tsuchimura reports that the `sqrt(34)` component of the origin is finite and
has farthest distance `< 24,289,452`:

- Source: https://www.keisu.t.u-tokyo.ac.jp/data/2004/METR04-13.pdf
- Relevant table: `sqrt(34)`, farthest distance `< 24289452`, total size
  `Finite`.

That statement is about the origin-connected component. The current campaign
compositor answers a shell question: whether any Gaussian-prime component spans
from the shell's inner boundary to its outer boundary. Those are not equivalent.

## Rejected Probe

The candidate probe was:

```bash
./build-k34/campaign_main_cuda \
  --k-sq=34 \
  --r-inner=24289452 \
  --r-outer=24297644 \
  --region full-octant \
  --chunk-size=200000 \
  --no-early-exit \
  --timing
```

Observed on the Vast RTX 4090 at commit `9e69542`:

| Tiles | Total | CUDA K1-K5 | Compositor | Verdict | Overflow counters |
|---:|---:|---:|---:|---|---|
| `2,479,915` | `27.2406s` | `22.1916s` | `4.10526s` | `SPANNING` | all zero |

The zero overflow result is useful: K34 itself does not immediately stress the
current capacities. The `SPANNING` verdict means this shell is not a cheap
external truth gate.

## Why It Failed

A shell-spanning component outside radius `24,289,452` does not have to be
connected to the origin. Tsuchimura's result only rules out origin reach past
the bound. It does not rule out disconnected prime components that span a later
annulus.

The current TileOp/compositor pipeline is designed for annular moat barriers,
not for computing the global connected component of the origin.

## What Would Make K34 A Strong Gate

One of these is needed:

1. Direct origin-component verification for K34.
   This should model Tsuchimura's actual graph process or an equivalent
   proof-preserving algorithm. A naive disk campaign is not practical: a
   full-octant disk out to radius `24,289,452` would be billions of 256x256
   tiles.

2. An externally justified exact K34 annular boundary.
   This would need a known pair or bracket analogous to the K36 gate:
   one full-octant annulus expected `SPANNING`, and a nearby one expected
   `MOAT`.

Until then, K34 is useful for compile/generalization checks and CPU/CUDA parity,
but it is not an external correctness gate.
