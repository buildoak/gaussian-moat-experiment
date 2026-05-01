---
title: Hard-Negative Sweep Voided: R_inner Semantic Error
date: 2026-04-23
engine: codex
type: report
status: complete
refs: [tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/run_sweep.py, docs/supportive/2026-04-22-moat-boundary-final-sweep.md]
---

# Hard-Negative Sweep Voided: R_inner Semantic Error

## Summary

The hard-negative sweep launched on 2026-04-22 used incorrect `R_inner` semantics, making all results invalid for moat detection.

The sweep invoked the solver as a narrow shell test instead of a full-annulus origin-reachability test. It therefore answered "is this thin shell self-connected?" rather than "can the origin-side component reach `R_outer`?"

## Root Cause

`run_sweep.py` line 62 used:

```text
--r-inner=R --r-outer=R+8192
```

This describes a narrow 8192-unit shell.

It should have used:

```text
--r-inner=80000000 --r-outer=R
```

This describes the full annulus from the origin-containing radius out to the tested `R_outer`.

At `R = 80,015,790`, the shell `80,015,790 -> 80,023,982` is above the moat and self-connected, so it returned `SPANNING`. That verdict was correct for the wrong question.

## Results Voided

- Total radii tested: 2 (`80,015,782` and `80,015,790`)
- Both returned `SPANNING`
- Zero `MOAT` verdicts
- Sweep halted early due to "failure" at `R = 80,015,790`, where the sweep expected `MOAT` but got `SPANNING`

These hard-negative sweep results are void for moat detection.

## Impact

The hard-negative sweep results are invalid for moat detection.

The boundary-final sweep from 2026-04-22 remains valid because it used `--r-inner=80000000`. The Tsuchimura reproduction at bracket `(80,015,782, 80,015,790)` stands:

- `R_outer = 80,015,782`: `SPANNING`
- `R_outer = 80,015,790`: `MOAT`

The CUDA solver remains correct and deterministic. This was an invocation error, not a code bug.

## Lessons

MOAT detection requires the full annulus from the origin-containing radius, not narrow radial shells.

Future sweeps must anchor `R_inner` at an origin-containing radius, for example `80,000,000`, and vary `R_outer`.
