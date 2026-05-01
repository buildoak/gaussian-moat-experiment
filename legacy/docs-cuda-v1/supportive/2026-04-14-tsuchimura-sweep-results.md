---
date: 2026-04-14
type: sweep-results
status: provisional
campaign: campaign-sqrt-36
diagonal_fix: false
---

# Tsuchimura Boundary Sweep Results (Pre-Diagonal-Fix)

> **PROVISIONAL:** These results were collected BEFORE the diagonal connectivity
> fix (commit cf2d0e6). The `is_tile_dead()` predicate skipped all tiles below
> the y=x diagonal, potentially severing cross-diagonal connectivity paths.
> A post-fix validation sweep is pending. Verdicts may shift — particularly
> in the 60M-70M transition zone.

## Configuration

- K_SQ=36, step distance sqrt(36)=6
- MAX_PRIMES_GPU=4096 (commit 62f216c, fixes the 27% overflow bug)
- Burst size: 56,000 tiles
- Hardware: RTX 4090, CUDA 12.4, sm_89
- Total overflow across all 29 runs: **0**

## Results

### Full Table (29 runs, sorted by R)

| R | Verdict | Overflow | Towers | Wall time |
|---|---------|----------|--------|-----------|
| 50,000,000 | SPANNING | 0 | 138,131 | 13s |
| 60,000,000 | SPANNING | 0 | 165,753 | 29s |
| 61,000,000 | MOAT | 0 | 168,515 | 52s |
| 62,000,000 | SPANNING | 0 | 171,277 | 30s |
| 63,000,000 | MOAT | 0 | 174,039 | 54s |
| 64,000,000 | MOAT | 0 | 176,801 | 55s |
| 65,000,000 | MOAT | 0 | 179,563 | 55s |
| 66,000,000 | MOAT | 0 | 182,325 | 56s |
| 67,000,000 | MOAT | 0 | 185,088 | 57s |
| 68,000,000 | MOAT | 0 | 187,850 | 58s |
| 69,000,000 | MOAT | 0 | 190,612 | 59s |
| 70,000,000 | MOAT | 0 | 193,374 | 60s |
| 75,000,000 | MOAT | 0 | 207,185 | 64s |
| 76,000,000 | MOAT | 0 | 209,947 | 64s |
| 77,000,000 | MOAT | 0 | 212,709 | 65s |
| 78,000,000 | MOAT | 0 | 215,471 | 66s |
| 78,500,000 | MOAT | 0 | 216,852 | 67s |
| 79,000,000 | MOAT | 0 | 218,233 | 66s |
| 79,250,000 | MOAT | 0 | 218,924 | 67s |
| 79,500,000 | MOAT | 0 | 219,614 | 67s |
| 79,750,000 | MOAT | 0 | 220,305 | 68s |
| 80,000,000 | MOAT | 0 | 220,995 | 68s |
| 80,015,000 | MOAT | 0 | 221,037 | 69s |
| 80,016,000 | MOAT | 0 | 221,040 | 68s |
| 80,100,000 | MOAT | 0 | 221,272 | 68s |
| 80,250,000 | MOAT | 0 | 221,686 | 68s |
| 80,500,000 | MOAT | 0 | 222,376 | 67s |
| 81,000,000 | MOAT | 0 | 223,758 | 68s |
| 800,000,000 | MOAT | 0 | 2,209,733 | 650s |

### Transition Zone Detail

The SPANNING-to-MOAT transition is non-monotonic:

- R=60M: SPANNING (early exit at 56K/166K towers)
- R=61M: MOAT (first moat)
- R=62M: SPANNING (isolated island — connectivity briefly restored)
- R=63M+: MOAT (stable from here up)

Last definitive SPANNING: R=62,000,000
First stable MOAT: R=63,000,000

### Comparison with Tsuchimura (2004)

Tsuchimura established an upper bound of 80,015,782 for the K_SQ=36 connected
component. Our results show the component dies much earlier (~60-63M), consistent
with his bound being a loose upper bound rather than a tight one.

Note: Our pipeline checks annular bands [R, R+8192], not the full component
from origin. Tsuchimura's upper-bound probe traces the entire component.
The methods are not directly comparable but should agree on the moat location
if the implementation is correct.

## Known Limitations

1. **Diagonal gap (pre-fix).** Tiles below y=x were skipped entirely.
   Cross-diagonal connectivity paths penetrating >6 units could be severed.
   Fix deployed in commit cf2d0e6 — validation pending.

2. **Annular band method.** Each run checks a single 8192-unit band, not the
   full component from origin. A connectivity path that weaves through the
   interior without crossing the specific band would be missed.

3. **Non-monotonicity.** The R=62M SPANNING island demonstrates that moat
   existence is not monotonic in R. Fine-grained sweeps in the 60-63M range
   would reveal the full structure.

## Next Steps

- [ ] Post-diagonal-fix sweep (same R values)
- [ ] Fine sweep in 60-63M range at 256-unit resolution
- [ ] Compare post-fix vs pre-fix verdicts
- [ ] If transition shifts significantly, the diagonal gap was a real correctness issue
