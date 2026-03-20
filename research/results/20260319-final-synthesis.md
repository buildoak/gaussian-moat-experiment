---
date: 2026-03-19
engine: claude-opus-4.6
status: complete
type: campaign-synthesis
---

# R_moat(40) Multi-Model Prediction Ensemble: Final Synthesis

Synthesis of 8 independent AI model predictions, 1 Codex swarm posterior, and 1 prior Opus audit. Produced for the RTX 3090 computational campaign on the sqrt(40) Gaussian Moat Problem.

---

## 1. Prediction Ensemble

| Model | Central R | 90% CI | Key Method | Key Innovation |
|-------|-----------|--------|------------|----------------|
| MiniMax M2.5 | 1.5e7 | [5e6, 4e7] | Increment extrapolation + ratio method | Branching-process dominance via 75% fill fraction |
| MiMo v2-Pro | 1.8e8 | [4e7, 8e8] | 5-model Bayesian averaging | Honest functional-form instability analysis |
| DeepSeek v3.1 | 1.9e8 | [6e7, 7e8] | Percolation threshold: k^2/ln(R) = lambda_c | Cleanest single-equation: lambda_c ~ 2.1 calibrated from data |
| Codex Swarm | 2.27e8 | [6.5e7, 9.9e8] | 6-model Bayesian posterior, 10 iterations | Origin-lineage extinction mechanism identification |
| Grok 4.1 Fast | 2.8e8 | [8.5e7, 9.5e8] | Modified Tsuchimura + UB extrapolation | UB ratio log(UB)/p ~ 1.13-1.14 as independent evidence |
| Gemini 3 Flash | 4.2e8 | [8.5e7, 1.4e9] | Tsuchimura + density decay correction | log R ~ alpha*p(k) - beta*ln(ln R) |
| Grok 4.20 Beta | 4.0e8 | [8e7, 3e9] | Branching-process + empirical coeff decline | Most complete mechanism discussion (offspring ~ 3.2) |
| GPT-5.4 Pro | 4.5e8 | [8e7, 2e9] | z_eff parity-compatible coordination | z_eff(40)=68; slope 0.2535-0.2556; UB consistency check |

Excluded: GLM-5 (byte-identical duplicate of Grok 4.1), Gemini 3.1 Pro (truncated), Gemini Pro reasoning (no output).

**Ensemble statistics (quality-weighted):**
- Geometric mean: 2.9e8
- Median: 3.1e8
- IQR (log10): [8.28, 8.62] = [1.9e8, 4.2e8]

---

## 2. Where Models Converge (Signal)

**Order of magnitude.** Every model with a completed prediction places R_moat(40) in [10^7, 10^9]. Excluding the one outlier (MiniMax at 1.5e7, which violates its own UB constraints), all predictions fall in [1.8e8, 4.5e8] -- a factor of 2.5 spread on a problem spanning 17 orders of magnitude in theory.

**Deceleration is real.** All models agree that the growth rate d(ln R)/dk^2 is declining. The raw data shows: 0.338 (k^2: 20-26) then 0.170 (k^2: 26-32). Unanimous agreement this trend continues.

**Tsuchimura overestimates.** The naive linear fit ln|xi| ~ 1.160*p(k) gives 1.17e9 for k^2=40. Every model applies a downward correction, typically 3-10x, based on the declining ratio ln(R)/p(k): 1.311 (p=9), 1.152 (p=12), 1.061 (p=14).

**Moat is not a prime desert.** Universal agreement. At k^2=32, the origin component contains 75% of all first-octant primes to that distance. The moat forms in a region where primes are still locally abundant. Expected local degree at R ~ 3e8 is still ~ 4.3 -- above percolation threshold.

**Component is finite.** All models assign >95% probability that the k^2=40 component is finite.

---

## 3. Where Models Diverge (Uncertainty)

The predictions cluster into two camps:

**Camp A -- "Low 2e8" (DeepSeek, Codex, MiMo): central ~ 2.0e8.**
Heavy weight on observed deceleration. Use percolation threshold calibration or Bayesian model averaging anchored to the declining coefficient ratio.

**Camp B -- "Mid 4e8" (GPT-5.4, Gemini Flash, Grok 4.20): central ~ 4.0e8.**
Incorporate UB data for k^2=34,36 as evidence against extreme deceleration. Use z_eff coordination or corrected Tsuchimura with UB consistency checks.

**The driver of disagreement:** How much to trust the k^2=34,36 upper bounds as informative. Camp B notes that log(UB)/p ~ 1.13-1.14 at k^2=34,36 -- close to Tsuchimura's 1.16, suggesting the scaling has NOT collapsed. Camp A notes that UBs can be 3-5x loose and the deceleration in exact data is the stronger signal.

**Secondary divergence:** Whether the 48-to-68 jump in z_eff (or 14-to-18 in p(k)) produces a proportionally large or diminishing-returns boost in reach. This is the core extrapolation question that only computation can resolve.

---

## 4. The z_eff Innovation (GPT-5.4 Pro)

GPT-5.4 Pro's parity-compatible coordination number is the single most novel analytical contribution.

**Definition:** z_eff(k^2) = count of non-zero vectors (dx,dy) with dx^2+dy^2 <= k^2 and dx == dy (mod 2). Off-axis Gaussian primes sit on the odd-norm sublattice, so only parity-compatible displacements can connect them.

**Verified values:** z_eff(20)=36, z_eff(26)=44, z_eff(32)=48, z_eff(34)=56, z_eff(36)=60, z_eff(40)=68. All confirmed by explicit enumeration in the Opus audit.

**Slope stability:** d(ln R)/d(z_eff) = 0.2535 (k^2: 20-26) and 0.2556 (k^2: 26-32). A 0.8% variation across a 21x range in R. This is striking.

**UB consistency:** Extrapolation predicts R_34 ~ 2.2e7 (UB: 2.43e7, ratio 89%) and R_36 ~ 6.0e7 (UB: 8.0e7, ratio 75%). Both plausible.

**Caution:** Only two calibration intervals (Delta_z_eff = 8 and 4). The 32-to-40 extrapolation spans Delta_z_eff = 20, which is 2.5-5x larger than any calibration interval. Earlier slopes (k^2 < 20) oscillate: 0.360, 0.228, 0.315. The stability may reflect asymptotic convergence or coincidence.

**Independent corroboration:** Gemini 3.1 Pro independently introduced the same sublattice coordination concept (N_sub(40)=68) before being truncated. Two models converging on the same variable unprompted is meaningful.

**Verdict:** z_eff is a genuine innovation with verified arithmetic and strong UB consistency. It is the best single predictor available. But the large extrapolation step warrants treating its output (4.5e8) as the high-quality upper anchor, not a precision estimate.

---

## 5. Moat Character -- What IS the Moat?

**Consensus mechanism: connectivity fragmentation, not prime depletion.**

At the moat distance, the local prime density supports expected degree ~ 4+ per prime. Primes are everywhere. The origin's component dies because its expanding frontier encounters a topological cutset -- a band where the specific primes reachable from the origin fail to chain across, even though unreachable primes fill the gaps.

This is a **branching-process extinction** event. The origin's lineage is a Galton-Watson process in an inhomogeneous medium where the offspring mean slowly declines. Even while supercritical (mean > 1), finite realizations go extinct with probability approaching 1 as the process weakens.

**The moat is not an annular gap.** No empty ring exists at computationally accessible distances. The Cramer-Granville analysis (Codex iteration 4) showed that complete prime-free annuli of width sqrt(40) are astronomically improbable below ~ 10^15.

**Physical picture:** The component grows like a fjord-riddled landmass, maintaining multiple narrow corridors that propagate outward. As corridors pass through regions where primes happen to be locally misaligned (gaps > sqrt(40) in specific directions), corridors pinch off. Eventually all surviving corridors terminate simultaneously -- not because primes vanish, but because no corridor finds the next stepping stone.

**Specific arithmetic at k^2=40:** The only new displacement vectors beyond k^2=36 are the eight "6-2" vectors: (+-6, +-2) and (+-2, +-6), norm 40. These are bridge-builders, potentially spanning bottlenecks that killed corridors at k^2=36. This is why R_moat(40) >> R_moat(36).

---

## 6. RTX 3090 Campaign Design

**Hardware:** RTX 3090, 24 GB VRAM. Solver: gaussian-moat-cuda (Rust+CUDA), UB method, auto-connect bug fixed (boundary_norm = D^2 per Tsuchimura).

### Phase 0: Validation (< 30 min)
Reproduce k^2=36 UB < 80,015,782. Confirms solver correctness post-fix.
```
gaussian-moat-cuda --k2 36 --ub-probe 85000000 --no-overlap
```

### Phase 1: Geometric Bracketing (1-3 hours)
The prediction spread is [1.5e7, 1.5e9]. Use geometric doubling to find the bracket.

Start HIGH (at ensemble median 3e8) and work outward in both directions. If the component terminates below the probe distance, we get an upper bound. If it survives, the moat is farther.

```bash
# Probe 1: R = 200M (norm ~ 4e16). Memory ~ 1.65 GB. Safe.
gaussian-moat-cuda --k2 40 --ub-probe 200000000 --no-overlap --sweep-mode sweep

# Probe 2: Based on result of Probe 1:
#   If terminated: probe 100M (bracket [100M, 200M])
#   If survived:   probe 400M (memory ~ 3.3 GB, safe)
gaussian-moat-cuda --k2 40 --ub-probe 400000000 --no-overlap --sweep-mode sweep

# Probe 3: Continue doubling/halving.
#   Next stops: 800M (memory ~ 6.2 GB), or 50M downward.
#   At 1B: memory ~ 7.6 GB. Still safe on 24 GB.
#   At 2B: memory ~ 14.8 GB. Tight but feasible.
#   At 5B: memory ~ 35 GB. EXCEEDS VRAM -- use --no-overlap mandatory.
```

**Time estimates per probe** (extrapolated from sieve benchmarks at 2.65M primes/sec):
| Probe R | Sieve est. | Connect est. | Total | Memory |
|---------|-----------|-------------|-------|--------|
| 100M | ~6s | ~4-10s | ~15s | ~0.9 GB |
| 200M | ~12s | ~9-20s | ~30s | ~1.65 GB |
| 400M | ~30s | ~18-40s | ~70s | ~3.3 GB |
| 800M | ~72s | ~35-80s | ~150s | ~6.2 GB |
| 1B | ~95s | ~40-90s | ~185s | ~7.6 GB |
| 2B | ~230s | ~76-174s | ~400s | ~14.8 GB |

### Phase 2: Log-Bisection (1-2 hours)
Once bracket U/L <= 4, switch to log-bisection: probe at sqrt(U*L).
5-7 bisections to reach U/L <= 1.2. Each probe ~ 30-180s depending on R.

### Phase 3: Linear Bisection (2-4 hours)
~20-30 additional probes to resolve the exact integer norm of the farthest reachable point.

### Total Campaign Budget
| Phase | Est. Probes | Est. Wall Time |
|-------|-------------|----------------|
| Phase 0 (validation) | 1 | 0.5 h |
| Phase 1 (bracketing) | 4-8 | 0.5-2.0 h |
| Phase 2 (log-bisect) | 5-7 | 1.0-2.0 h |
| Phase 3 (exact) | 20-30 | 2.0-4.0 h |
| **Total** | **30-46** | **4-8 h** |

### Critical Decision Points
- **If terminated at R < 50M:** The deceleration camp wins. Investigate phase transition near k^2=40. Most scientifically interesting outcome.
- **If alive past 1B:** The z_eff slope may be accelerating. Extend to 5B (memory-constrained; requires chunk-by-chunk sweep). Consider whether the component could be infinite.
- **If in [1.5e8, 5e8]:** Ensemble prediction validated. Measure the annular hazard profile at the moat to feed back into theory.

### Failure Modes to Watch
1. **OOM at high R:** Use `--no-overlap` unconditionally. At 2B+, monitor VRAM.
2. **Sieve bottleneck:** 78% of wall time. No mitigation beyond GPU optimization.
3. **Cross-chunk connectivity:** `--sweep-mode sweep` handles this. Verify with the k^2=36 validation run.

---

## 7. Final Verdict

**Central prediction: R_moat(40) ~ 3.0 x 10^8 (300 million).**

Honest uncertainty:
- 50% band: [1.5e8, 4.5e8]
- 90% band: [8e7, 1.5e9]

If forced to a single number for a bet: **300 million**, driven by the quality-weighted ensemble median where the z_eff extrapolation (4.5e8), the percolation calibration (1.9e8), and the Bayesian posterior (2.3e8) triangulate.

The moat is not an empty desert. It is the place where the origin's branching tree of connected primes -- after traversing hundreds of millions of stepping stones -- finally runs out of forward paths in every direction simultaneously, in a landscape still dense with primes it can no longer reach.

The RTX 3090 campaign should resolve this in under 8 hours of wall time.

---

*Synthesis of 8+ model predictions. All z_eff arithmetic independently verified. Campaign designed for RTX 3090 (24 GB VRAM) with gaussian-moat-cuda solver.*
