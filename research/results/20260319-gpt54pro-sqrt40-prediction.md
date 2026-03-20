---
date: 2026-03-19
engine: gpt-5.4-pro
status: complete
---

# GPT-5.4 Pro: sqrt(40) Gaussian Moat Prediction

## Prediction

R_moat(40) ≈ 4.5 × 10^8

- Heuristic central band: [2 × 10^8, 9 × 10^8]
- Conservative model-disagreement band: [8 × 10^7, 2 × 10^9]
- Corresponding norm scale: R² ~ 2 × 10^17
- 80% confidence the component is finite

## 1. Empirical Extrapolation (Key Innovation: Parity-Compatible Coordination)

The raw coordination number 128 is NOT the right bulk parameter. For off-axis Gaussian primes, differences satisfy dx ≡ dy (mod 2), so only parity-compatible displacement vectors matter asymptotically.

z_eff(40) = #{(dx,dy) ≠ (0,0): dx²+dy² ≤ 40, dx ≡ dy (mod 2)} = 68

For exact large-k data:
- z_eff(20) = 36
- z_eff(26) = 44
- z_eff(32) = 48

Fitting log R against effective coordination using only exact large-k points, the increments are remarkably stable:

- log(R_26/R_20) / (44-36) = 0.2535
- log(R_32/R_26) / (48-44) = 0.2556

Empirical slope a ≈ 0.2545 per added parity-compatible vector, hence:

R_40 ≈ R_32 × exp(a × (68-48))
     ≈ 2.823 × 10^6 × e^(0.2545 × 20)
     ≈ 4.6 × 10^8

**Consistency check against upper bounds:**
- R_34 ≈ 2.2 × 10^7 (UB: 2.43 × 10^7) ✓
- R_36 ≈ 6.0 × 10^7 (UB: 8.00 × 10^7) ✓

This is the single strongest reason for the mid-10^8 range.

## 2. Percolation / Mean-Degree Heuristic

Using density ρ(N) ~ 4/(π log N), occupancy among parity-compatible target sites is about 8/(π log N) = 4/(π log R), so expected local degree at radius R for k²=40:

μ(R) ≈ z_eff(40) × 4/(π log R) = 272/(π log R)

Placing critical mean degree in the low 4's (2D geometric-percolation regime behind Vardi's λ_c ≈ 0.35):

μ_c ∈ [4.3, 4.5] ⟹ R ∈ [2.3 × 10^8, 5.6 × 10^8]

Centering at μ_c ≈ 4.35 gives R ≈ 4.4 × 10^8 — essentially the same answer as empirical extrapolation.

By contrast, the smooth continuum estimate ignoring lattice shell effects gives only ~7.9 × 10^7.

## 3. Moat Formation Mechanism

**Not a prime-free annulus.**

At R ~ 4-5 × 10^8, a first-octant shell of width k=√40 contains ~7 × 10^7 Gaussian primes in expectation, with typical local degree still ~4.3. Primes are still locally abundant.

**Mechanism: connectivity fragmentation.** The reachable set looks like a near-critical random geometric graph with fjords, tendrils, and bottlenecks. Eventually no surviving k-step crossing of some annular band exists. The moat is a topological cutset in a graph that is still locally populated, not a circular empty desert.

The jump from 36→40 is arithmetically specific: the only new bulk displacements are the eight "6-2" vectors: ±(6,2), ±(6,-2), ±(2,6), ±(2,-6). k²=40 does not increase density — it gives one new way to bridge bottlenecks. That is exactly the sort of change that can move R_moat by a large factor without changing the qualitative mechanism.

## 4. Reliability of Extrapolation

In raw k², not very. In the right discrete variable, moderately.

Key insight: for exact large-k points 20, 26, 32, log R is strikingly linear in the effective parity-compatible coordination. The censored data at 34, 36 do not contradict this pattern. The step 36→40 adds exactly one new displacement shell type — this is a one-shell extrapolation, not an uncontrolled leap.

Caution: factor-of-2 or factor-of-3 uncertainty is real. Pre-2004 work had only an upper bound 5.59 × 10^6 at k²=26 while exact value was 1.02 × 10^6, so upper bounds can be loose by ~5×.

## 5. Recommended Search Strategy

Probe: 10^8, 2 × 10^8, 4 × 10^8, 8 × 10^8 in order.

First-octant width-√40 annulus workloads:
- R=10^8: ~1.7 × 10^7 primes
- R=2×10^8: ~3.3 × 10^7 primes
- R=4×10^8: ~6.4 × 10^7 primes
- R=8×10^8: ~1.2 × 10^8 primes

**Start at R = 4 × 10^8** — all three heuristics converge there, annulus workload is still manageable.

Use segmented ordinary-prime sieve in norm slabs, generate Gaussian primes from rational primes, check adjacency with hash table + precomputed 68 parity-compatible displacement vectors. For full moat structure, Prasad's finite-set reduction to Euclidean MST via Delaunay triangulation + Kruskal is the right engine.

## Novel Contributions

1. **Parity-compatible coordination z_eff** as the correct extrapolation variable (not raw p(k) or full lattice count 128)
2. **Remarkably stable slope** (0.2535 vs 0.2556) in z_eff space across the two exact large-k intervals
3. **Consistency check**: z_eff extrapolation predicts R_34 and R_36 values that sit comfortably under published UBs
4. **Identification of the specific arithmetic change**: eight "6-2" vectors as the only new bridges at k²=40
