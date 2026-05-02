# Optimization Safety Checklist

Do not accept an optimization if any item below is unproven.

## Non-Negotiable Semantics

- Methodology remains stronger than implementation.
- Closed tile boundaries stay closed: a side `S` tile contains `S + 1`
  lattice points per axis.
- Face and port ordering remain byte-stable where parity tests expect it.
- `TileOp` byte layout remains stable unless all CPU/CUDA consumers and tests
  are changed together.
- Overflow counters remain visible in normal CUDA output.
- Detailed overflow diagnostics may be optional, but zero-overflow gates must
  remain cheap to check.
- `SPANNING` may early-exit only after a valid connected witness is latched.
- `MOAT` is a whole-region verdict and must not early-exit.
- Snapshot mode must write every TileOp and must disable early exit.
- Sparse/explicit region semantics must not be silently substituted for
  full-octant Tsuchimura gates.

## Required Proof Surface

- CPU/CUDA parity for changed surfaces.
- Tsuchimura two-case gate for campaign-level acceptance.
- Snapshot SHA smoke unless the change is strictly script/documentation only.
- First-divergence tooling remains usable:
  `cuda_vs_cpu_diff --m4 --k5 --verbose`.

## High-Risk Areas

- face representative extraction,
- port sorting and label remapping,
- MR primality path,
- cross-column stitching,
- partial-column ingestion,
- dispatcher synchronization and buffer reuse.
