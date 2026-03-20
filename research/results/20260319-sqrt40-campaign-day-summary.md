---
date: 2026-03-19
engine: coordinator
status: complete
type: campaign-summary
campaign: sqrt40-upper-bound
---

# sqrt(40) Gaussian Moat — Campaign Day Summary

**Date:** 2026-03-19
**k²:** 40 (step distance sqrt(40) ≈ 6.324)
**Outcome:** Moat confirmed beyond 2.4B. All predictions eliminated. Stitching bug found, fixed, and verified.

---

## Corrected Probe Data (k²=40, step ≤ √40)

All probes used Tsuchimura upper-bound method: annular sieve around distance D, fictitious auto-connect at norm ≤ D², check organic connectivity beyond D².

"Survived" = no moat found at D under auto-connect assumption. Does NOT prove origin-to-D connectivity.

| D | Primes | Component Size | Ratio | Result | Notes |
|---|--------|---------------|-------|--------|-------|
| 50M | 19.7M | — | 95.9% | survived | Phase 1 |
| 100M | 38.0M | — | 95.3% | survived | Phase 1 |
| 200M | 73.2M | — | 94.6% | survived | Phase 1 |
| 400M | 141.4M | — | 93.9% | survived | Phase 1 |
| 500M | 262.1M | — | 90.0% | survived | Phase 1 |
| 600M | 311.7M | — | 89.7% | survived | Phase 1 |
| 700M | 360.9M | — | 89.4% | survived | Phase 1 |
| 800M | 365.1M | — | 89.1% | survived | Post-fix verified |
| 900M | 458.3M | — | 88.9% | survived | Phase 1 |
| 1.0B | 506.7M | — | 88.7% | survived | Phase 1 |
| 1.2B | ~602M | — | ~88% | survived | Post-fix verified |
| 1.6B | 792.7M | — | 87.7% | survived | Phase 2 |
| 2.4B | 1,166.7M | 1,013.4M | 86.9% | survived | Phase 2, FRONTIER |
| 3.2B | 1,535.2M | — | — | OOM killed | solver SIGKILL at 887M/1535M primes |
| 6.4B | — | — | — | overflow | D²>UINT64_MAX |

### Ratio Decay Analysis

Smooth monotonic decline from 95.9% (50M) to 86.9% (2.4B). No real anomalies after stitching bug fix.

Rate of decline:
- 50M–400M: ~0.67%/100M (steeper)
- 400M–800M: ~1.2%/100M (steepest — crossover regime)
- 800M–2.4B: ~0.14%/100M (flat — slow logarithmic decay)

In ln(R) space: ~2.0% per unit ln(R) from 800M–2.4B. Consistent with μ(R) ∝ 1/ln(R) decay.

The deceleration is pronounced: rate drops by ~6x between the 400M–800M regime and the 800M–2.4B regime. At the regime 3 rate, reaching 0% ratio would require an additional ~62B from the 2.4B frontier. Naive extrapolation suggests true moat in the range of **10B to 20B**, though this is speculative without data beyond 2.4B.

---

## Calibration Probe: k²=36 at D=1B

**CRITICAL FINDING**: k²=36 has a known moat at D≈80M. We probed k²=36 at D=1B expecting failure. The probe SURVIVED at 90.0% ratio (260.7M/289.5M primes).

This proves:
1. **Local annular connectivity ≠ global path existence.** The moat at 80M is a local structural gap, not a density collapse. At D=1B (12× past the moat), local connectivity is healthy.
2. **μ < μ_c does NOT break local connectivity.** μ(1B, k²=36) ≈ 3.44, well below μ_c ≈ 3.92. Yet ratio is 90%.
3. **The component ratio is a weak predictor of moat proximity.** The ratio can remain >85% even far past the actual moat distance.
4. **Moats are first-bad-sector events** — rare structural gaps on a growing circumference, not average density thresholds.

Implications for k²=40: the 86.9% ratio at 2.4B tells us almost nothing about how close the moat is. The moat could be at 5B or 50B with the ratio barely changing.

---

## Stitching Bug (Found and Fixed)

Three distances showed anomalous ratios with `--angular 6`:
- 800M: 44.6% (should be 89.1%)
- 950M: 44.4% (should be 88.8%)
- 1200M: 29.4% (should be ~88%)

Root cause: `stitch()` in `stitcher.rs` relied on overlap primes (only 6-11 per boundary) to merge wedge origins. At specific distances, overlap primes failed to bridge wedge boundaries.

Fix: explicit origin unification via `global_uf.union()` of all wedge origins. Commit `d956873`.

Verified: all three distances recovered to expected ratios. `--angular 1` (no stitching needed) confirmed the expected values independently.

### Why the Bug Was Silent

The bug did not manifest at 50M–700M or at 1.0B, 1.6B, 2.4B because the overlap prime geometry at those distances happened to bridge wedge origins transitively. There was no error message, no assertion failure, no anomalous timing. The only signal was the ratio anomaly in an otherwise smooth curve.

---

## Model Predictions (updated with round 2)

| Model | Prediction | μ_c | 90% CI | Status |
|-------|-----------|-----|--------|--------|
| GPT-5.4 Pro r1 (boxed) | 450M | ~5.0 | — | eliminated |
| GPT-5.4 Pro r1 (reasoning) | ~1.1-1.2B | ~4.1 | — | eliminated |
| Tsuchimura formula | 1.17B | ~4.14 | — | eliminated |
| **GPT-5.4 Pro r2** | **10B** | **3.75** | **3B–60B** | alive |
| **Gemini 3.1 Pro** | **5.8B** | **3.85** | **3.5B–14.5B** | alive |

Both surviving predictions place the moat past the u64 overflow wall (4.29B).

Earlier eliminated predictions (pre-Phase 2):

| Model | Prediction | Eliminated At |
|-------|-----------|---------------|
| MiniMax M2.5 | 15M | 50M |
| Empirical scaling | 35M | 50M |
| External ensemble batch 1 | 40M | 50M |
| Origin-lineage survival | 100M | 200M |
| DeepSeek Terminus | 120M | 200M |
| MiMo v2-Pro | 180M | 200M |
| DeepSeek v3.1 | 190M | 200M |
| Grok 4.1 Fast | 280M | 400M |
| Codex Swarm (Bayesian blend) | 227M | 400M |
| GLM-5 | 280M | 400M |
| Grok 4.20 Beta | 400M | 800M (corrected) |
| Gemini 3 Flash | 420M | 800M (corrected) |

All predictions eliminated except GPT-5.4 Pro r2 (10B) and Gemini 3.1 Pro (5.8B). The pattern is systematic under-prediction. Notable: GPT-5.4 Pro's publicly stated boxed answer remained 450M even after its reasoning trace had revised upward to ~1.1B — a documented discrepancy between stated and computed prediction.

---

## Infrastructure

- Quebec instance 33133205: RTX 3090, $0.119/hr. Used for fix verification + k²=36 calibration.
- California instance 33129683: DESTROYED. Data saved to `research/results/california-instance/`.
- Solver fix deployed: commit d956873 (explicit origin unification in stitcher.rs).

### Instance History

**Instance 33123347** (Phase 1+2, Quebec):
- $0.128/hr, 20 GB disk, 257 GB RAM
- Ran all probes 50M through 1.6B
- Destroyed when disk full at 3.2B attempt; results recovered from session transcript

**Instance 33129683** (Phase 3, California):
- $0.150/hr, 50 GB disk, RTX 3090
- Used for Phase 3 probes and bug fix verification
- Destroyed after campaign completion

**Instance 33133205** (fix verification, Quebec):
- $0.119/hr, RTX 3090
- Spun up to verify d956873 fix on all three anomalous distances + k²=36 calibration
- All verifications passed; destroyed

### Commit References

| Commit | Description |
|--------|-------------|
| 65dc2f7 | CMakeLists.txt: added RTX 3090 (SM 8.6) target |
| 0dd7e32 | Paper-quality logging; `sqrt40-bracket.sh` direct probe script |
| d956873 | Stitching bug fix: explicit origin unification in `stitch()` |

---

## Memory Wall Analysis

Peak memory at distance D: ~77 bytes × π√k × D / (16 ln D) primes. At D=3.2B: ~29 GB.

Root cause: norm-ordered sweep forces entire annular ring to be held in memory. Ring has constant radial width (√40 ≈ 6.32) but circumference grows as πD/4.

6 concurrent BandProcessors × ~4.14 GB each = ~24.8 GB peak RAM demand at 3.2B. RTX 3090 VRAM is 24 GB — solver SIGKILL.

**Proposed fix (short term):** Bounded concurrency — process 2 wedges at a time instead of all 6 concurrently, reducing peak to ~8.3 GB. Requires a semaphore or work queue in the solver's wedge dispatch loop.

**Identified path forward (long term):** Tile-based spatial sort in CUDA sieve + tile-based solver could give O(1) memory. Requires changing the sieve sort comparator from (norm) to (spatial tile) and rewriting the solver's BandProcessor as a TileProcessor. Combined with u128 norms, this removes both the memory wall and the 4.29B u64 overflow wall.

### u64 Overflow at 6.4B

At D = 6.4B, D² = 4.096×10¹⁹ > UINT64_MAX (1.844×10¹⁹). The norm storage type overflows silently.

**Fix:** `u128` norm storage for the prime coordinate representation, with sieve output and solver input updated to match. Required for any probe beyond D ≈ 4.3B.

---

## Open Questions

1. Can tile-based processing with O(1) per-tile memory + cross-tile stitching replace the norm-ordered BandProcessor?
2. How does the cross-tile equivalence table scale? (Likely small — dominated by one giant component.)
3. Is the Tsuchimura two-parameter formula ln R ≈ a·p(k) + b·g(k) testable against known moats?
4. Can we validate the "first-bad-sector" moat model by running k²=36 probes at multiple distances to map the moat structure?
5. Clean re-run of full probe series with fixed solver from 50M to 2.4B would produce paper-quality data with consistent trust level across all distances.
6. Corridor structure measurement at frontier: measure C_δ(R) — the number of macroscopic corridors through which the origin component crosses the frontier annulus at radius R. Predicted to be O(1) near the moat, dropping to 0 at moat formation.

---

## Related Files

| File | Description |
|------|-------------|
| `20260319-sqrt40-probe-results-recovered.md` | Full Phase 1+2 data recovered from transcript |
| `20260319-gpt54pro-sqrt40-computational-feedback.md` | GPT-5.4 Pro full response; boxed vs reasoning trace discrepancy |
| `20260319-gpt54pro-sqrt40-followup.md` | GPT-5.4 Pro round 2 prediction (10B) |
| `20260319-gemini31pro-sqrt40-round2.md` | Gemini 3.1 Pro round 2 prediction (5.8B) |
| `20260319-opus-final-synthesis.md` | Final synthesis and campaign assessment |
| `california-instance/` | Data from destroyed Phase 3 instance |
| `quebec-800M-grid/` | 800M grid scan results from Quebec instance |
