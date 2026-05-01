---
title: Grid Coverage Verifier
date: 2026-04-20
engine: codex
type: report
status: complete
refs: [methodology/lemmas_v2/campaign-blueprint.md, methodology/lemmas_v2/tile-operator-definition-v-claude.md, tools/coverage/coverage_verifier.py]
---

## Summary

Implemented `tools/coverage/coverage_verifier.py`, a dependency-free PEP 723 Python script for snapped-grid coverage checks. The verifier:

- Enumerates blueprint-oriented octant towers for `0 <= a <= b` using exact integer lattice predicates.
- Uses the conservative coverage envelope `[R_inner - C, R_outer + C]`, where `C = floor(sqrt(K))`, because the requested theta sweep explicitly probes those collar radii.
- Folds requested theta samples from `(R cos(theta), R sin(theta))` into the blueprint octant by sorting coordinates before checking the containing cell.
- Checks annulus thickness, I1 tower interval validity, I2 bounded endpoint shift, and I4 diagonal orphan absence.

## Results

| Case | Command parameters | Towers | Active tiles | Result |
|---|---:|---:|---:|---|
| Project K=40 | `--r-inner 80000000 --r-outer 80008192 --k-sq 40 --theta-samples 1000000` | 220994 | 8178529 | PASS |
| Project K=36 | `--r-inner 80000000 --r-outer 80008192 --k-sq 36 --theta-samples 1000000` | 220994 | 8178529 | PASS |
| Small K=36 | `--r-inner 1000000 --r-outer 1001024 --k-sq 36 --theta-samples 1000000` | 2765 | 16329 | PASS |
| Small K=40 | `--r-inner 1000000 --r-outer 1001024 --k-sq 40 --theta-samples 1000000` | 2765 | 16329 | PASS |

## Invariants

All four runs passed:

- Thickness: `R_outer - R_inner > S * sqrt(2) + 2 * sqrt(K)`.
- I1: every tower is represented as a closed integer interval.
- I2: adjacent tower low/high endpoints shift by at most one.
- I4: every diagonal active pair has at least one active common face-neighbor.

## Violations

No uncovered theta sample cells and no invariant violations were found in the final runs.

## Notes

The mission text names the theta sweep octant as `0 <= theta <= pi/4` with points `(R cos(theta), R sin(theta))`, while the canonical blueprint/math proof sections use the tower orientation `0 <= a <= b`. The verifier resolves this by folding each theta point into the blueprint octant before checking coverage, matching the reflection-closure model.
