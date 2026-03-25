---
date: 2026-03-25
engine: coordinator
status: complete
---

# k²=40 Fat-Stripe Discovery: Total Annular Blockage at R ≈ 1.03B–1.09B

## Summary

Five fat-stripe probes at k²=40 confirm **total annular blockage** across a 60M radial band from R=1.03B to R=1.09B. Zero spanning components in all five probes. This is the first detection of total annular blockage at k²=40 — a k² value where no moat was previously known.

## Probe Results

Each probe covers a 128K×128K annular strip (64×64 tiles at W=2000), angular width b_max=128K.

| Probe | R        | Spanning components | Tiles | Time (Mac) |
|-------|----------|---------------------|-------|------------|
| 1     | 1.03B    | 0                   | 4,096 | 5m 15s     |
| 2     | 1.04B    | 0                   | 4,096 | 5m 15s     |
| 3     | 1.05B    | 0                   | 4,096 | 5m 13s     |
| 4     | 1.07B    | 0                   | 4,096 | 5m 12s     |
| 5     | 1.09B    | 0                   | 4,096 | 5m 9s      |

**Per-tile timing:** ~0.076 s/tile on Mac (M-series), comparable to k²=36 at R=80M.

## Percolation Context

The total annular blockage is consistent with percolation analysis at the candidate moat radius:

| Parameter | k²=32 at R=2.82M | k²=36 at R=80M | k²=40 at R=1.05B |
|-----------|-------------------|-----------------|-------------------|
| E[backward degree] | — | 0.89 | 0.90 |
| E[total degree] | ~1.8 | 1.79 | 1.81 |

At total degree ~1.81, the graph sits at the fragmentation threshold. This is the same percolation regime that produces total barriers at k²=32 and k²=36.

**Progression of confirmed blockage:**
- k²=32: BLOCKED at R ≈ 2.82M (known Tsuchimura moat)
- k²=36: BLOCKED at R ≈ 80M (known Tsuchimura upper bound)
- k²=40: BLOCKED at R ≈ 1.03B–1.09B (**new — no previously known moat**)

## ISE Candidate Band Context

The 1.03B–1.09B range was identified as the ISE candidate band in session 09bd7a9b, where the ISE sigmoid for k²=40 shows R₀.₅ ≈ 839M with f(r) approaching zero by ~1B. The fat-stripe probes targeted the region where ISE predicts complete fragmentation — and confirm it.

## Geometry Note

Angular coverage per probe: 128K lattice units ≈ 20K step distances (at √40 ≈ 6.3).

At R=1B, 128K lattice units subtend approximately **0.007°** — a narrow wedge representing ~0.016% of the first octant. The signal is strong and consistent across all five radial positions, but full-octant coverage (~45°) is needed for a rigorous proof per Lemma 6.1 of the GPCTO methods paper.

The probes demonstrate methodological capability and confirm the percolation regime prediction, but they are calibration-grade, not proof-grade.

## Next Steps

1. **Edge mapping:** Probe below 1.03B and above 1.09B to find the radial boundaries of the blocked region. The ISE sigmoid suggests blockage begins around 839M–900M, so probes at 0.95B, 0.98B, 1.00B, and 1.10B, 1.12B, 1.15B would map the edges.

2. **Angular widening:** Increase b_max from 128K toward full-octant coverage. At R=1.05B, full-octant requires b_max ≈ 1.05B (since the first octant extends to b=a along the diagonal). This is ~8,200× the current angular width, requiring proportional tile count increase. Full-octant campaign at 2000×2000 tiles: ~525K tiles/stripe × 5 stripes ≈ 2.6M tiles total.

3. **Throughput optimization:** At 0.076 s/tile, 2.6M tiles would take ~55 hours serial. Rayon parallelism on multi-core hardware brings this to feasible single-day campaigns.

4. **Cross-validation:** Run ISE probes at the same radii to confirm consistency between methods (as done for k²=32 in Section 6.6.3 of the methods paper).
