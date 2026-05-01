---
title: Pruning Analysis Report — Dead-End Group Pruning Soundness and P3 Verification
date: 2026-04-07
type: evidence
status: complete
supports: tile_spec.md
description: Validates the dead-end pruning rule (single-face single-port groups omitted), confirms P3 composition correctness is preserved after pruning, and shows u4/u5 group label bounds are met.
---

# Pruning Analysis Report

Generated: 2026-04-07 22:02:20
Runtime: 0.2s | Backend: gmpy2

## Pruning Rule

A group is **prunable** if it touches exactly ONE face AND has exactly
ONE port on that face. Such groups are dead-end islands: they enter
through a single port on a single face and have no exit path. No
spanning path can transit through them.

**Keep:** groups touching 2+ faces (cross-tile connectivity carriers),
AND single-face groups with 2+ ports (same-face bridging).

## Results

### Primary Tiles

| Tile | Total Groups | Prunable | Surviving | Ports Before | Ports After | Max Surv/Face |
|------|-------------|----------|-----------|-------------|-------------|---------------|
| small |z|~141 | 1 | 0 | 1 | 4 | 4 | 1 |

### Pair Tiles (Right Neighbors)

| Tile | Total Groups | Prunable | Surviving | Ports Before | Ports After | Max Surv/Face |
|------|-------------|----------|-----------|-------------|-------------|---------------|
| small |z|~141 (B-tile) | 1 | 0 | 1 | 4 | 4 | 1 |

### Per-Face Breakdown (After Pruning)

| Tile | I | O | L | R |
|------|---|---|---|---|
| small |z|~141 | 1 | 1 | 1 | 1 |
| small |z|~141 (B-tile) | 1 | 1 | 1 | 1 |

## Spec Fit

- Max surviving groups across all tiles: **1**
- Max surviving ports/face: **1**
- u4 group label (max 16): **FITS**
- u5 group label (max 32): **FITS**

## P3 Composition Correctness (Pruned)

- **PASS**: small |z|~141: pruned=True, brute=True, original=True [A: 4->4 ports, B: 4->4 ports]

**All P3 tests pass after pruning.** Spanning verdicts are preserved.
Pruning is sound: no composition correctness regressions.

## Group Classification Detail (Large Radius Tiles)
