---
date: 2026-03-19
engine: gpt-5.4-pro
type: followup-deep-math
status: complete
prompt: z_eff tightening + corridor formalization + UB sensitivity
---

# GPT-5.4 Pro Follow-up: Deeper Mathematics for sqrt(40) Prediction

## Refined Prediction

R_moat(40) ≈ 4.5 × 10^8 (UNCHANGED from initial)

- Working band: [3.5 × 10^8, 6.0 × 10^8] (tightened from [2e8, 9e8])
- Conservative band: [2 × 10^8, 8 × 10^8]
- Norm scale: R² ≈ 2 × 10^17

---

## 1. Why d(ln R)/d(z_eff) Should Be Constant — THEORETICAL DERIVATION

Off-axis Gaussian primes live on the checkerboard sublattice (one coordinate even, one odd). Site occupancy on this sublattice:

    q(R) ~ 4/(π log R)

Local usable mean degree:

    μ(R, k) ≈ z_eff(k²) × 4/(π log R)

If moat forms when μ drops to critical value μ_c:

    μ(R_moat, k) ≈ μ_c
    ⟹ log R_moat ≈ log R_0 + (4/(π μ_c)) × z_eff(k²)

**This IS linearity in z_eff.** The checkerboard-percolation heuristic predicts it directly.

Calibrating from data:
- Slope s ≈ 0.25408
- μ_c ≈ 4/(π × 0.25408) ≈ 5.01
- R_0 = e^2.6552 ≈ 14.23

Fit: R ≈ 14.23 × e^(0.25408 × z_eff)

Predictions:
- R_34 ≈ 2.15 × 10^7 (UB: 2.43 × 10^7) ✓
- R_36 ≈ 5.94 × 10^7 (UB: 8.00 × 10^7) ✓
- R_40 ≈ 4.53 × 10^8

## 2. Why Slopes Oscillate for k² < 20 and Stabilize After

Using the transformation log(R/R_0) instead of log(R):

| k² | log(R/R_0) / z_eff |
|----|---------------------|
| 16 | 0.2381              |
| 18 | 0.2367              |
| 20 | 0.25411             |
| 26 | 0.25399             |
| 32 | 0.25413             |

Three things happen at k² ≈ 20:

1. **Axis primes become negligible.** Off-axis term ~ R²/log R dominates axis term ~ R/log R. Ratio is 2/R. At R ≈ 1.34 × 10^5 (k²=20), axis contribution is microscopic.

2. **Frontier band self-averages.** Band size at k²=18: ~2,460 primes. At k²=20: ~25,300. At k²=32: ~538,000. Once frontier contains tens of thousands of primes, local irregularity stops dominating.

3. **Shell noise becomes a correction.** For small k, one new shell changes everything. By k² ≥ 20, the coarse-grained variable μ = z_eff × q(R) wins.

## 3. Expected Second Derivative (Curvature)

From the heuristic: if q_c(z) = μ_c/z + α/z² + O(z^-3), then:

    d²(log R)/d(z_eff)² = O(z^-3)

From data: slopes 0.25348 (interval [36,44]) and 0.25558 (interval [44,48]).
Representative curvature: |log R''| ≈ 0.00210/6 ≈ 3.5 × 10^-4

Propagated over Δz=20 extrapolation:
    ½ × |log R''| × 20² ≈ 0.07

**This is only ~8% multiplicative.** Mild concavity pulls 4.5 × 10^8 to ~4.2 × 10^8, NOT to 2 × 10^8.

**The dominant uncertainty is R_36, not curvature.**

## 4. Shell-by-Shell Decomposition (Key Argument)

The 32→40 jump is NOT one monolithic Δz_eff = 20. It decomposes:

- 32→34: add norm-34 shell, 8 vectors (±5, ±3)
- 34→36: add norm-36 shell, 4 axis vectors
- 36→40: add norm-40 shell, 8 vectors (±6, ±2)

From calibration data:
- 20→26 is one 8-vector off-axis shell: R_26/R_20 = 7.5976
- 26→32 is one 4-vector shell: R_32/R_26 = 2.7796

Shell-calculus prediction:

    R_40 ≈ R_32 × (R_26/R_20)² × (R_32/R_26)
         = 2.823 × 10^6 × 7.5976² × 2.7796
         ≈ 4.53 × 10^8

**This is not a vague long extrapolation. It is "repeat the observed 8-shell gain, then the observed 4-shell gain, then the observed 8-shell gain."**

## 5. Corridor Structure at the Moat Boundary

### Formal definition:
C_δ(R) = number of connected components of C_0 ∩ {R-δ ≤ |z| ≤ R+δ} that meet both boundaries

**Prediction:** Near R ≈ R_moat, C_δ(R) is O(1), probably 2-5 in the first octant, then drops to 0. Not measurable from published paper (only aggregate stats), but measurable from saved k²=32 component data.

### The moat is NOT an annular prime desert:
At k²=32 moat radius: ~538,000 first-octant primes in the frontier band.
At predicted k²=40 moat: ~73 million primes in width-√40 annulus.

### Annular crossing probability:
    P_cross(R) ≈ 1 - exp(-C_δ(R) × q_br(R))

Survival law:
    Pr(R_moat ≥ R) ≈ exp[-1/k ∫^R dr/ξ_corr(r)]

**Not polynomial. Exponential in shell number, with rate drifting as 1/log R.**

## 6. Bridge-Building Power of the (6,2) Vectors

### Three measures:

1. **Raw coordination jump:** 60→68 = +13.3%

2. **Long-range part increase:** Shells 34+36 contribute 12 long vectors. Adding shell 40: total 20. **+66.7% increase in long-range neighborhood.**

3. **Support-optimal in every generic direction.** For 0 < θ < π/4:

    h_40(θ) = max{6cos θ, 5cos θ + 3sin θ, 6cos θ + 2sin θ}

The new (6,2) vector strictly beats every k²=36 hull vector for every generic first-octant outward normal (for cos θ > sin θ, i.e., θ < π/4).

- Max radial gain: 2/√10 ≈ 0.632 (at θ = arctan(1/3))
- Max relative gain: ~11.1%
- Average gain over [0, π/4]: ~5.5%

**For lattice gaps in (6.0, 6.32]: the (6,2) vectors bridge 100% of them.** Only norm-40 vectors have norms in this range.

## 7. Sensitivity to R_36 (THE Key Unknown)

| Assumed R_36 | R_40 (local) | R_40 (global) |
|--------------|-------------|---------------|
| 3.0 × 10^7  | 1.45 × 10^8 | 2.05 × 10^8  |
| 4.0 × 10^7  | 2.34 × 10^8 | 2.87 × 10^8  |
| 5.0 × 10^7  | 3.40 × 10^8 | 3.71 × 10^8  |
| 6.0 × 10^7  | 4.60 × 10^8 | 4.59 × 10^8  |  ← convergence point
| 7.0 × 10^7  | 5.95 × 10^8 | 5.49 × 10^8  |
| 8.0 × 10^7  | 7.44 × 10^8 | 6.41 × 10^8  |

**R_36 ≈ 6.0 × 10^7 is the special value** where local and global extrapolations meet, AND it matches the z_eff exponential law prediction.

## 8. Dead End: UB/R_exact Modeling

Cannot model UB/exact as a function of k² from published data alone. The overshoot depends on the hidden start radius y and subcritical tail structure.

**Recommended instead:** Calibrate the same UB code path on solved k²=32 with the same time budget planned for k²=40. This directly measures UB tightness.

## 9. Summary

Cleanest justification:
    R ≈ R_0 × e^(s × z_eff)
    R_0 ≈ 14.2, s ≈ 0.2541, μ_c ≈ 5.0

Shell-by-shell version:
    32→40 = (8-shell) × (4-shell) × (8-shell)
    = 7.5976² × 2.7796 ≈ 160.5× factor
    R_40 ≈ 2.823M × 160.5 ≈ 4.53 × 10^8

The single highest-value precursor computation: calibrate Tsuchimura UB mode on solved k²=32 to measure UB tightness before trusting k²=34,36 bounds.
