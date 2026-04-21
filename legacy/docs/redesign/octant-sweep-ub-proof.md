# Lean Upper Bound Proof: Octant Sweep with Narrow Strips

## Concept

Prove that a Gaussian moat exists at radius R by demonstrating blockage across one full octant (0° to 45°). By the 8-fold symmetry of Gaussian primes (units ±1, ±i plus conjugation), one octant proves the full ring.

## Architecture

### Annular Ring Composition

The annulus at radius R with radial thickness ΔR = 8 × 256 = 2,048 lattice units is decomposed into many **narrow vertical strips** that tile along the arc from θ=0° to θ=45°.

Each strip:
- **Height (radial):** 8 tiles = 2,048 lattice units (perpendicular to arc)
- **Width (tangential):** 256K lattice units = 1,000 tiles (along arc)
- **a-range:** Computed per-strip to track the annulus: `r_min = floor(sqrt(R² - b_max²)) - pad`, `r_max = ceil(sqrt((R + ΔR)² - b_min²)) + pad`
- **Tiles per strip:** ~12 × 1,000 = 12,000

### Why Narrow Strips

At R=836M, the annulus shifts only ~39 lattice units in the a-direction across a 256K b-strip (<1 tile). Narrow strips track the curvature with zero waste. Wide strips include massive dead space (primes outside the annulus that still get sieved and UF'd).

### Strip Count

For R=836M, the octant spans b = 0 to R/√2 ≈ 591M.
- Strip width: 256K → ~2,309 strips
- Tiles per strip: ~12K
- Time per strip: ~2-3 seconds
- **Total: ~2 hours for the full octant**

### Early Stopping

The sweep proceeds strip-by-strip from θ=0° rightward. If ANY strip shows connectivity (spanning from r_inner to r_outer), the moat has a gap at that angle → R is NOT a valid upper bound → STOP immediately.

For radii where the moat truly exists, all strips will be blocked. For radii near the moat boundary, the first connected strip stops the sweep early.

### Spanning Verdict: Per-Prime Radial Check (Required)

The existing face-based spanning check (left-face to right-face) is only correct when the radial direction aligns with the a-axis (θ ≈ 0°). At θ > 10°, the radial direction diverges from the face normals, causing false positives for blockage (missed spanning paths). This is **dangerous for a UB proof**.

**Required fix:** Replace face-based spanning with per-prime radial spanning:
- For each boundary prime in a component, compute `r = sqrt(a² + b²)`
- A component **spans** if it contains primes at `r ≤ r_inner` AND `r ≥ r_outer`
- Correct at any angle, any strip position

This is one function change in `run_compact_merge()` in `fat_stripe_cuda.cu`.

### Cross-Strip Connectivity (Seam Handling)

A spanning path may cross between adjacent strips ("entered left, exited right"). Two approaches:

**Option A: Overlap (simple, conservative)**
Adjacent strips overlap by ≥ 2,048 lattice units (8 tiles). Any spanning path that wanders laterally by less than ΔR is fully contained in at least one strip. Independent per-strip verdicts are sufficient.

Caveat: paths CAN wander more than ΔR laterally. This makes overlap a strong heuristic, not a rigorous proof.

**Option B: Cross-strip seam merge (rigorous)**
Save boundary component data from each strip. After all strips complete, merge components across strip boundaries. Check global spanning after full merge. This is equivalent to running one giant campaign and gives a rigorous result.

### L(θ) — Angle-Dependent Strip Width

Near θ=0° (real axis): the radial direction aligns with a-axis. 8 tiles of a-height gives full ΔR radial coverage.

Near θ=45°: the radial direction is diagonal. 8 tiles of a-height only gives ΔR × cos(45°) ≈ 1,448 radial units. Options:
1. Increase a-height to `ceil(8 / cos(θ))` tiles at each angular position
2. Use a fixed 12-tile a-height everywhere (gives ≥ 2,048 radial units up to 45°)

Option 2 is simpler: 12 tiles × 256 = 3,072. At θ=45°: 3,072 × cos(45°) = 2,172 > 2,048. ✓

### Implementation Plan

1. **Implement per-prime radial spanning check** in `run_compact_merge()` (CUDA)
2. **Write octant sweep script** — Python/bash that:
   - Computes (r_min, r_max, b_min, b_max) for each strip based on R
   - Runs fat-stripe-cuda sequentially for each strip
   - Implements early stopping on first connected verdict
   - Logs results per strip
3. **Validate** on known radii:
   - R=826M (connected on-axis) → should find connected strip quickly
   - R=836M (blocked on-axis, all angles to 10°) → should complete full octant blocked
4. **Cross-strip seam merge** (Phase 2) for rigorous proof

### Efficiency Summary

| R | Octant b-range | Strips | Tiles/strip | Total tiles | Est. time |
|---|---------------|--------|-------------|-------------|-----------|
| 836M | 591M | 2,309 | 12K | 27.7M | ~2h |
| 1.0B | 707M | 2,762 | 12K | 33.1M | ~2.3h |
| 1.3B | 919M | 3,590 | 12K | 43.1M | ~3h |

Early stopping makes non-moat radii much cheaper (seconds, not hours).
