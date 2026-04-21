---
date: 2026-03-24
status: planning
engine: coordinator
---

# Multi-Metric ISE: Beyond Binary Traversability

## Core Idea

Current ISE measures one thing per tile: f(r) = can a k-path cross it? (0 or 1)

The tile graph (primes as nodes, k-edges between neighbors within √k) is already built during each probe. We can extract additional graph-theoretic metrics at marginal cost and calibrate them against known Tsuchimura moats for k²=26, 32, 36.

Multiple calibrated metrics → tighter moat prediction for k²=40 → smaller target band for tile-UB campaign → less CUDA compute needed.

## Context

- Current f(r) prediction for k²=40 moat: [1.06B, 1.20B] — 140M wide band
- UB tile campaign ideally needs ≤100K wide target area
- CUDA UB path needs several sessions of optimization work
- Stripes are cheap; graph construction is the bottleneck, not metric extraction

## Candidate Metrics (pending mathematical analysis)

To be populated from Opus + Codex xhigh analysis dispatched 2026-03-24.

## Implementation Notes

- Metrics computed inside existing tile processing loop (after union-find)
- Output via --export-detail JSON (extend existing schema)
- Calibration: same known-moat workflow as moat-calibration-2026-03-23
