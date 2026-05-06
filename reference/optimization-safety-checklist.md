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
  full-octant accepted/profile rows.

## Required Proof Surface

- Compact verification spine in `reference/current-verification-spine.md`.
- Exact BZ/profile enforcement and zero-overflow visibility.
- Local C++ CTest for C++/shared semantics.
- CUDA CTest on real CUDA hardware for CUDA changes.
- Post-flight sample audit and SPANNING certificate when producing accepted rows.
- CPU/CUDA diff, snapshot SHA, and goldens remain usable for localization and
  regression, but are not standalone claim-acceptance gates.

## High-Risk Areas

- face representative extraction,
- port sorting and label remapping,
- MR primality path,
- cross-column stitching,
- partial-column ingestion,
- dispatcher synchronization and buffer reuse.
