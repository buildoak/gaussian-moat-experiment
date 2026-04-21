---
date: 2026-03-22
engine: coordinator
status: ready
type: campaign-plan
campaign: k40-ise-exploration
target_k_squared: 40
---

# k²=40 ISE Exploration Campaign Plan

## 1. Parameter Calculation

### 1.1 Lattice Neighbors

For k²=40, the connectivity disk contains all lattice vectors (da, db) with da²+db² ≤ 40.

| d² | d     | Count | Example vectors            |
|----|-------|-------|----------------------------|
| 1  | 1.000 | 4     | (±1,0), (0,±1)            |
| 2  | 1.414 | 4     | (±1,±1)                   |
| 4  | 2.000 | 4     | (±2,0), (0,±2)            |
| 5  | 2.236 | 8     | (±1,±2), (±2,±1)          |
| 8  | 2.828 | 4     | (±2,±2)                   |
| 9  | 3.000 | 4     | (±3,0), (0,±3)            |
| 10 | 3.162 | 8     | (±1,±3), (±3,±1)          |
| 13 | 3.606 | 8     | (±2,±3), (±3,±2)          |
| 16 | 4.000 | 4     | (±4,0), (0,±4)            |
| 17 | 4.123 | 8     | (±1,±4), (±4,±1)          |
| 18 | 4.243 | 4     | (±3,±3)                   |
| 20 | 4.472 | 8     | (±2,±4), (±4,±2)          |
| 25 | 5.000 | 12    | (±5,0), (0,±5), (±3,±4), (±4,±3) |
| 26 | 5.099 | 8     | (±1,±5), (±5,±1)          |
| 29 | 5.385 | 8     | (±2,±5), (±5,±2)          |
| 32 | 5.657 | 4     | (±4,±4)                   |
| 34 | 5.831 | 8     | (±3,±5), (±5,±3)          |
| 36 | 6.000 | 4     | (±6,0), (0,±6)            |
| 37 | 6.083 | 8     | (±1,±6), (±6,±1)          |
| 40 | 6.325 | 8     | (±2,±6), (±6,±2)          |

**Total neighbors (z): 128** (64 backward offsets for scanline).

Compared to k²=36 (z=112), k²=40 gains 16 new vectors at d²=37 and d²=40. These are the (±1,±6), (±6,±1), (±2,±6), (±6,±2) vectors.

### 1.2 Collar

The code uses `ceil(sqrt(k²))`:
- k²=36: collar = ceil(6.0) = 6
- k²=40: collar = ceil(6.324) = 7

Note: the maximum single-coordinate excursion in the neighbor set is 6 (not 7). The theory paper's `floor(sqrt(k²)) = 6` is sufficient. The code is conservative — using collar=7 adds ~0.2% overhead per tile but guarantees correctness for any future k² value. **We use collar=7 as the code implements.**

### 1.3 Stride and Tile Geometry

| Parameter      | k²=36   | k²=40   |
|----------------|---------|---------|
| collar         | 6       | 7       |
| tile_size      | 2000    | 2000    |
| stride         | 2012    | 2014    |
| expanded tile  | 2013²   | 2015²   |
| points/tile    | 4,052,169 | 4,060,225 |
| stripes (M)    | 32      | 32      |
| max b-offset   | 64,385  | 64,448  |

### 1.4 Percolation Threshold Estimate

**Method: mean-degree calibration from k²=36.**

At k²=36, the percolation boundary (f(r)=0) was found at R ≈ 80.4M with empirical prime density ≈ 3.50%. The mean degree at the transition:

    d_c = p × z = 0.0350 × 112 = 3.92

If percolation occurs at the same critical mean degree for k²=40:

    p_c(k²=40) = d_c / z = 3.92 / 128 = 0.03063 = 3.06%

**Cross-check: linear z-scaling.**

    p_c(k²=40) ≈ p_c(k²=36) × (112/128) = 3.50% × 0.875 = 3.06%

Same answer — consistent.

### 1.5 Estimated Percolation Boundary Radius

Prime density vs radius (calibrated from k²=36 data: C = 1.274):

    p(R) = C / (2 × ln(R))

Setting p(R) = p_c = 3.06%:

    ln(R) = 1.274 / (2 × 0.0306) = 20.82
    R = e^20.82 ≈ 1.10 billion

| Target density | R estimate    | Notes                           |
|----------------|---------------|---------------------------------|
| 3.50% (k²=36) | ~80M          | k²=36 calibration point         |
| 3.30%          | ~225M         | If d_c lower than expected       |
| 3.06%          | **~1.1B**     | Central estimate for k²=40       |
| 2.80%          | ~7.3B         | If d_c higher than expected      |
| 2.50%          | ~173B         | Extreme upper bound (implausible)|

**Central estimate: R ≈ 1.1 billion** (ratio vs k²=36: ~13.4×).

### 1.6 Comparison with k²=36

| Metric                    | k²=36    | k²=40 (est.) |
|---------------------------|----------|--------------|
| Neighbors (z)             | 112      | 128          |
| Collar                    | 6        | 7            |
| Percolation density (p_c) | 3.50%    | 3.06%        |
| Percolation boundary (R)  | 80.4M    | ~1.1B        |
| Mean degree at boundary   | 3.92     | 3.92         |
| Growth factor             | (base)   | ~13.4×       |

### 1.7 Validation Against Old UB Probes

The 2026-03-19 UB campaign (annular method, different from ISE) found:
- All probes SURVIVED at D = 50M through 2.4B
- Component ratio declined smoothly: 95.9% (50M) → 86.9% (2.4B)
- 3.2B: OOM killed (memory wall)
- 6.4B: u64 overflow

These UB probes measure annular connectivity (auto-connect + forward sweep), NOT local percolation. A UB probe can "survive" even past the true ISE percolation boundary because it fictitiously seeds connectivity and only checks organic bridging within a narrow band.

The UB survival at 2.4B is consistent with (but does not constrain) an ISE percolation boundary at ~1.1B. ISE and UB are measuring fundamentally different things.

**Key insight from the k²=36 calibration probe at D=1B:** k²=36 has a known moat at D ≈ 80M, but the UB probe at D=1B SURVIVED at 90% ratio. This proves annular connectivity is robust far beyond the moat — the moat is a local structural gap, not a density collapse. The same applies to k²=40: UB survival tells us nothing about where the ISE percolation boundary is.

---

## 2. Campaign Phases

### Phase 1: Coarse Scan (find the general region)

Sparse sampling from R=200M to R=2B, step 200M. Goal: locate where f(r) drops from healthy (>0.5) to transitional (<0.3).

```
R values: 200M, 400M, 600M, 800M, 1.0B, 1.2B, 1.4B, 1.6B, 1.8B, 2.0B
Shells per R: 1 (single tile row at each distance)
Tile: 2000², 32 stripes, collar=7
Total: 10 shells
```

**ISE command template:**
```bash
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 32 \
    --r-min $R \
    --r-max $((R + 2000)) \
    --threads 0
```

**Decision tree:**

| f(r) at 200M | f(r) at 1B | f(r) at 2B | Interpretation           | Next         |
|---------------|------------|------------|--------------------------|--------------|
| > 0.8         | 0.3–0.5    | < 0.1     | Transition near 1B       | Phase 2: 800M–1.4B |
| > 0.8         | > 0.5      | 0.3–0.5   | Transition near 1.5–2B   | Phase 2: 1.2B–2.2B |
| > 0.8         | > 0.5      | > 0.5     | Transition beyond 2B     | Extend: 2.5B, 3B, 4B |
| 0.3–0.5       | < 0.1      | < 0.1     | Transition near 200–400M | Phase 2: 100M–600M |

### Phase 2: Dense Sweep (pin the transition)

Once Phase 1 brackets the transition to a ~400M window, sweep with step = 2000 (= tile height) across a ±50M window centered on the estimated boundary.

```
R values: R_center ± 50M, step 2000
Shells: 50,000 (= 100M / 2000)
Tile: 2000², 32 stripes, collar=7
```

If the central estimate (~1.1B) is correct:

```bash
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 32 \
    --r-min 1050000000 \
    --r-max 1150000000 \
    --threads 0
```

Alternatively, run a medium-density sweep first (step = 100,000, 1,000 shells) to locate the transition to within ±5M, then dense-sweep the ±5M window (5,000 shells).

### Phase 3: Hard Negatives (confirm beyond boundary)

After finding the f(r)=0 boundary, run 200 shells starting 50M past the boundary with step = 500,000. Goal: confirm f(r) stays near zero or continues declining.

If percolation boundary found at R_b:

```bash
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 32 \
    --r-min $((R_b + 10000000)) \
    --r-max $((R_b + 110000000)) \
    --threads 0
```

Also run a control well below the boundary (R_b - 200M) to confirm f(r) > 0.5 there.

---

## 3. Compute Budget

### 3.1 Per-Tile Timing

**Calibration:** k²=36, R=80M, 2000² tile on Jetson Orin Nano: ~46.5s per shell (32 stripes), i.e. ~1.45s per tile.

At k²=40, two factors change:
- Collar increases from 6 to 7 → expanded tile grows from 2013² to 2015² → +0.2% more points
- At larger R, norms have more bits → powmod is slower

| R     | Norm (a²+b²)  | Bits | Est. time/tile | Est. time/shell |
|-------|---------------|------|----------------|-----------------|
| 80M   | ~6.4×10¹⁵     | 53   | 1.5s           | 48s             |
| 200M  | ~4.0×10¹⁶     | 56   | 1.7s           | 55s             |
| 500M  | ~2.5×10¹⁷     | 58   | 1.8s           | 57s             |
| 1B    | ~1.0×10¹⁸     | 60   | 1.8s           | 58s             |
| 1.5B  | ~2.25×10¹⁸    | 61   | 1.9s           | 59s             |
| 1.95B | ~3.80×10¹⁸    | 62   | 1.9s           | 60s             |

Time scaling is mild: norms grow from 53 to 62 bits across the full search range, increasing powmod cost by ~17%. The row sieve and union-find are independent of R.

### 3.2 Total Time by Phase

| Phase    | Shells  | Time/shell | Total time | Total hours |
|----------|---------|------------|------------|-------------|
| Phase 1  | 10      | 55–60s     | ~10 min    | 0.2 hr      |
| Phase 2a | 1,000   | ~58s       | ~16 hr     | 16 hr       |
| Phase 2b | 5,000   | ~58s       | ~80 hr     | 80 hr       |
| Phase 3  | 200     | ~60s       | ~3.3 hr    | 3.3 hr      |

**Phase 2 is the bottleneck.** Two strategies:

**Strategy A: Two-stage Phase 2 (recommended)**
1. Medium sweep: 1,000 shells (step 100K), locate transition to ±5M → ~16 hours
2. Dense sweep: 5,000 shells (step 2K) across ±5M window → ~80 hours
Total: ~96 hours = 4 days

**Strategy B: Skip medium sweep**
Dense sweep across full ±50M (50,000 shells) → ~800 hours = 33 days. Not feasible on Jetson.

**Strategy C: Adaptive scan**
Start sparse (step 1M), identify f(r)<0.3 shells, zoom into 10M windows around those, then dense-sweep. Total: ~20-30 hours.

### 3.3 Platform Decision

| Platform         | Phase 1 | Phase 2a | Phase 2b | Phase 3 | Total  |
|------------------|---------|----------|----------|---------|--------|
| Jetson (6-core)  | 10 min  | 16 hr    | 80 hr    | 3 hr    | ~4 days |

**Jetson CAN run the full campaign** using Strategy A:
- Phase 1: one evening (~10 min)
- Phase 2a: overnight (~16 hr)
- Phase 2b: 3–4 overnights (~80 hr)
- Phase 3: one evening (~3 hr)

Cloud (vast.ai RTX instance) is NOT needed — ISE is CPU-bound (scanline kernel), not GPU-bound. The Jetson's 6 ARM cores with rayon parallelism are sufficient.

### 3.4 Memory

Per-tile memory for 2000² with collar=7:

| Component      | Size     |
|----------------|----------|
| prime_bitmap   | 507 KB   |
| parent (u32)   | 16.2 MB  |
| rank (u8)      | 4.1 MB   |
| **Total**      | **20.8 MB** |

With rayon parallel tiles (6 cores × 1 tile each): ~125 MB peak. Well within Jetson's 8 GB RAM.

---

## 4. Risk Factors

### 4.1 MR-9 Validity Ceiling (CRITICAL)

The 9-witness deterministic Miller-Rabin test is valid for all n < 3.825 × 10¹⁸.

At radius R, the maximum norm tested is:

    n_max = (R + tile_h + collar)² + b_max²

where b_max ≈ 64,448 (32 stripes, negligible vs R). So effectively n_max ≈ R².

| R       | R²            | Status   |
|---------|---------------|----------|
| 1.0B    | 1.00 × 10¹⁸   | OK       |
| 1.5B    | 2.25 × 10¹⁸   | OK       |
| 1.9B    | 3.61 × 10¹⁸   | OK       |
| 1.95B   | 3.80 × 10¹⁸   | OK       |
| **1.956B** | **3.825 × 10¹⁸** | **LIMIT** |
| 2.0B    | 4.00 × 10¹⁸   | **EXCEEDS — MR-9 no longer deterministic** |

**Hard ceiling: R ≤ 1.955 billion.**

Beyond R ≈ 1.956B, the MR-9 test becomes probabilistic (may misclassify composites as prime). This would silently corrupt ISE results.

**Impact on campaign:** The central estimate for the k²=40 percolation boundary (~1.1B) is safely below the MR-9 ceiling. If the boundary turns out to be at R > 1.9B, we need to either:
1. Add more MR witnesses (12 witnesses extend validity to ~3.3 × 10²⁰, covering R up to ~18B)
2. Use a different primality test (BPSW, which is deterministic for all n < 2⁶⁴)
3. Accept the risk (MR-9 false positive rate with 9 random-equivalent witnesses is ~4⁻⁹ ≈ 10⁻⁵·⁴ per test — extremely unlikely to affect any individual tile, but not zero across millions of tiles)

**Recommendation:** Monitor R during Phase 1. If f(r) > 0.5 at R=1.9B, pause and upgrade primality before proceeding.

### 4.2 u64 Norm Overflow

u64 max = 1.844 × 10¹⁹ → max R ≈ 4.295B (where R² fits in u64).

The code has `assert!(norm <= u64::MAX as i128)` in `is_gaussian_prime()`, so overflow would cause a panic, not silent corruption. This is safe — the program will crash rather than produce wrong results.

For our campaign (R ≤ 1.95B), u64 is sufficient.

### 4.3 Per-Tile Time Scaling

Powmod cost grows logarithmically with norm (one more squaring step per doubling of norm). From R=80M to R=2B, norms grow ~600×, adding ~10 squaring steps. This is a ~20% slowdown, already accounted for in Section 3.1.

The row sieve is independent of R (small primes ≤ 10,000). Union-find is independent of R. Only Miller-Rabin slows down.

### 4.4 Uncertainty in the Percolation Estimate

The central estimate (R ≈ 1.1B) assumes:
1. Mean degree at percolation is constant across k² values (d_c ≈ 3.92)
2. The calibrated prime density function p(R) = 1.274 / (2·ln(R)) holds at 1B
3. The ISE geometry (2000² tiles, 32 stripes) produces the same finite-size effects at 1B as at 80M

All three assumptions have uncertainty:
- d_c could be 3.5 or 4.5 (we only have one calibration point with f(r)=0)
- The density function has small corrections at different scales
- Finite-size effects depend on the correlation length ξ, which may scale differently at different densities

**Practical impact:** The boundary could be anywhere from ~200M (if d_c ≈ 4.5) to ~10B (if d_c ≈ 3.5). Phase 1's coarse scan (200M–2B) covers the most likely range. If nothing is found by 2B, the estimate needs revision.

### 4.5 The Tsuchimura Moat vs ISE Percolation

ISE measures local percolation — where ALL local connectivity collapses. The Tsuchimura moat is where the ORIGIN-connected component terminates — a weaker condition. At k²=40:

- The Tsuchimura moat could be at much smaller R than the ISE percolation boundary
- Old UB probes showed smooth connectivity up to 2.4B with the annular method
- ISE cannot detect the Tsuchimura moat directly (it has no concept of "origin")

If the campaign finds an f(r)=0 shell, that is the ISE percolation boundary. The Tsuchimura moat may be closer to the origin but invisible to ISE with 2000² tiles.

---

## 5. The Narrow-Strip Question

### 5.1 Background

At k²=36, narrow strips (W=240, H=2000) were found to be more sensitive to connectivity dips. The calibration campaign (2026-03-22) clarified why: narrow strips make per-strip crossing geometrically harder, so f(r)=0 appears at density levels that are still well above the true percolation threshold. This is "sensitivity through geometric constraint" — a feature, not a bug, when hunting for the Tsuchimura moat.

### 5.2 Should We Use Narrow Strips for k²=40?

**For the percolation boundary (primary goal): No.**

Square 2000² tiles are the correct instrument. The ISE percolation boundary is what we want for k²=40 — the radius where ALL local connectivity collapses. Narrow strips would detect a weaker signal (individual strip blockage) at smaller R, but this would conflate the Tsuchimura moat with the percolation boundary.

**For finding the Tsuchimura moat (secondary goal): Maybe, as a follow-up.**

After the percolation campaign maps the f(r) gradient:
1. If percolation boundary is at R_p ≈ 1.1B
2. Run narrow strips (W=240, H=2000) at R = 200M–1B to look for early f(r) dips
3. Any f(r)=0 with narrow strips at R << R_p is a Tsuchimura moat candidate

This is explicitly a Phase 4 activity — only after the percolation boundary is established.

### 5.3 Recommendation

**Campaign uses square 2000² tiles throughout.** Narrow strips deferred to Phase 4.

---

## 6. What Success Looks Like

### 6.1 Finding the f(r) Gradient

Map f(r) from healthy (>0.8) to collapsed (<0.1) across radius. Expected shape (based on k²=36):

```
R (est.)     f(r)
400M         0.85–0.95  (healthy)
600M         0.70–0.85  (beginning of decline)
800M         0.40–0.60  (transition zone)
1.0B         0.15–0.30  (near collapse)
1.1B         0.00–0.10  (percolation boundary)
1.3B         0.00–0.05  (below threshold)
```

### 6.2 Pinning the Percolation Boundary

Find the smallest R where f(r) = 0 with 32 independent stripes. This is the k²=40 ISE percolation boundary.

**Verification:** f(r) should show a monotonic decline approaching the boundary (as at k²=36: 0.1563 → 0.1250 → 0.0625 → 0.0000). A sudden isolated f(r)=0 without a declining approach would be suspicious.

### 6.3 If No f(r)=0 Found

If the entire Phase 1 scan (200M–2B) shows f(r) > 0.1 everywhere:

1. **The boundary is beyond 2B.** Extend to 3B, 4B (approaching MR-9 limit).
2. **The mean-degree model under-predicts.** Recalibrate using the measured f(r) gradient.
3. **Tile size may be too small.** At very large R, correlation length ξ may exceed 2000 lattice units. Test with 4000² tiles (4× compute cost per shell).

If f(r) stays above 0.5 even at R=1.9B (MR-9 ceiling), the campaign must pause for primality upgrade before extending further.

### 6.4 Deliverables

1. **f(r) gradient table:** R vs f(r) from 200M to boundary
2. **Percolation boundary radius:** R where f(r) first reaches 0
3. **Mean degree at boundary:** p(R_boundary) × 128, compared to k²=36 value (3.92)
4. **Dense sweep data:** full shell-by-shell profile near the boundary
5. **Control separation:** mean f(r) at control radius vs boundary radius

---

## 7. Execution Sequence

### Day 1: Phase 1 (Coarse Scan)

Run 10 shells at R = 200M, 400M, ..., 2.0B. Takes ~10 minutes. Analyze results immediately.

### Day 1–2: Phase 2a (Medium Sweep)

Based on Phase 1 results, select a ~200M window around the transition. Run 1,000 shells (step 100K). Takes ~16 hours (overnight).

### Day 2–5: Phase 2b (Dense Sweep)

Zoom into the ±5M window around the sharpest f(r) decline. Run 5,000 shells (step 2K). Takes ~80 hours (3–4 overnights).

### Day 5–6: Phase 3 (Hard Negatives + Control)

200 shells past the boundary + 100 shells at control distance. Takes ~5 hours.

**Total campaign: ~5–6 days on Jetson** (mostly unattended overnight runs).

---

## 8. Pre-Flight Checklist

- [ ] Verify ISE binary handles k²=40: `./ise --k-squared 40 --tile-size 8 --stripes 8 --r-min 0 --r-max 100`
- [ ] Confirm backward_offsets count = 64 for k²=40
- [ ] Run k²=2 smoke test with scanline kernel (f(r)=0 shells expected)
- [ ] Verify collar=7 in code output
- [ ] Check Jetson has ~200 MB free RAM
- [ ] Set up tmux session for long-running phases
- [ ] Prepare output directory: `research/results/k40-ise/`

---

## Appendix A: ISE Command Reference

```bash
# Phase 1: single shell at R=1000000000
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 32 \
    --r-min 1000000000 \
    --r-max 1000002000

# Phase 2: dense sweep across 100M window
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 32 \
    --r-min 1050000000 \
    --r-max 1150000000

# Phase 3: hard negatives beyond boundary
./tile-probe/target/release/ise \
    --k-squared 40 \
    --tile-size 2000 \
    --stripes 32 \
    --r-min $((BOUNDARY + 10000000)) \
    --r-max $((BOUNDARY + 110000000))
```

## Appendix B: Primality Upgrade Path (if R > 1.9B needed)

If the percolation boundary is beyond R = 1.9B, MR-9 becomes unreliable. Three options:

1. **Add witnesses (cheapest):** Extend MR_WITNESSES_9 to 12 witnesses: [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37]. Valid for n < 3.317 × 10²⁰, covering R up to ~18.2B. Cost: ~33% more powmod calls per primality test.

2. **BPSW test (standard):** Baillie-PSW (strong probable prime test + Lucas test). No known counterexample below 2⁶⁴. Widely believed deterministic for all u64 values. Cost: ~2× current MR-9.

3. **Hybrid:** Keep MR-9 for R < 1.9B, switch to BPSW only for tiles at R > 1.9B. Requires a runtime branch in `is_gaussian_prime()`.

Option 1 is simplest and sufficient for any realistic k²=40 campaign.
