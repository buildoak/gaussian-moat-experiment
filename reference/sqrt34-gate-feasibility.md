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

The first candidate probe incorrectly put Tsuchimura's reported upper bound on
the inner boundary:

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

After review, the closer analogy to the K36 gate is to put Tsuchimura's upper
bound on the outer boundary, not the inner boundary. That corrected candidate
was also tested:

```bash
./build-k34/campaign_main_cuda \
  --k-sq=34 \
  --r-inner=24281260 \
  --r-outer=24289452 \
  --region full-octant \
  --chunk-size=200000 \
  --no-early-exit \
  --timing
```

Observed on the Vast RTX 4090 at commit `6222ddf`:

| Tiles | Total | CUDA K1-K5 | Compositor | Verdict | Overflow counters |
|---:|---:|---:|---:|---|---|
| `2,479,110` | `27.639s` | `22.5806s` | `4.08228s` | `SPANNING` | all zero |

So the initial inner/outer placement was wrong, but correcting it did not
produce a K34 MOAT gate.

The shell centered on Tsuchimura's reported bound was also tested:

```bash
./build-k34/campaign_main_cuda \
  --k-sq=34 \
  --r-inner=24285356 \
  --r-outer=24293548 \
  --region full-octant \
  --chunk-size=200000 \
  --no-early-exit \
  --timing
```

Observed on the Vast RTX 4090 at commit `2800dfa`:

| Tiles | Total | CUDA K1-K5 | Compositor | Verdict | Overflow counters |
|---:|---:|---:|---:|---|---|
| `2,479,579` | `27.7603s` | `22.8277s` | `4.01044s` | `SPANNING` | all zero |

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

## Current Practical Gate

Use the practical K34 regression script when a branch needs cross-K coverage:

```bash
cd tiles-maxxing/cuda-campaign-v2-sqrt-36
scripts/run_k34_regression_gate.sh \
  --cpu-bin ../cpp-campaign-v2/build-k34/campaign_main \
  --cuda-bin ./build-k34/campaign_main_cuda \
  --diff-bin ./build-k34/cuda_vs_cpu_diff \
  --chunk-size 200000 \
  --timing \
  --profile-dir /workspace/profiles-k34-regression
```

This gate checks snapshot parity, separate `cuda_vs_cpu_diff --m4` and
`cuda_vs_cpu_diff --k5` probes, and the observed Tsuchimura-scale shell
sentinel. It should fail on K-dependent implementation drift or nonzero
overflow counters. It should not be cited as proof of the published K34 moat.

Acceptance run at commit `fc70d43` on the Vast RTX 4090:

- Snapshot smoke: PASS, CPU/CUDA SHA
  `1dc6c4dc031690a8849a59d94f6d2253c4c5b02a0c1b3a2db5d0c9935c2001e5`.
- Separate `cuda_vs_cpu_diff --m4 --limit 16` and
  `cuda_vs_cpu_diff --k5 --limit 16`: PASS.
- Shell sentinel: `SPANNING`, zero overflow counters, `2,479,915` tiles,
  `26.3411s` total, `21.3462s` CUDA K1-K5, `4.04465s` compositor.

Verifier note, 2026-05-04: the combined `--m4 --k5` invocation was weaker than
intended because `--k5` takes an early branch. The M4 verifier also expected
labels for all UF roots, while the CPU and CUDA TileOp contract label only roots
visible through ports or geo flags. Commit containing this note fixes M4 to use
the CPU visible-root remap and the K34 script now runs separate `--m4` and
`--k5` probes.

Post-fix acceptance on the Vast RTX 4090:

- Centered shell `24285356..24293548`: separate M4 and K5 probes passed.
- K34 regression script: PASS, shell sentinel `SPANNING`, zero overflow
  counters, `2,479,915` tiles, `27.3446s` total, `22.3788s` CUDA K1-K5,
  `4.06292s` compositor.
