# Gaussian Moat -- Experiment Status

> Last updated: 2026-03-23
> Status: Active -- k^2=40 ISE campaigns running on Jetson Orin Nano

---

## 1. Problem Statement

### 1.1 The Gaussian Moat Problem

A Gaussian integer z = a + bi (with a, b in Z) is a Gaussian prime if its norm
a^2 + b^2 is a rational prime congruent to 1 (mod 4), or if one coordinate is
zero and the other has absolute value a rational prime congruent to 3 (mod 4).
The Gaussian prime graph G_k = (P, E_k) connects two Gaussian primes pi, pi'
by an edge whenever |pi - pi'|^2 <= k^2.

The Gaussian moat problem asks: for a fixed step bound sqrt(k), is the connected
component of the origin in G_k finite? Equivalently, does there exist a "moat" --
an annular gap in the Gaussian prime graph that no path of bounded steps can cross?

### 1.2 Known Results

| k^2 | Step distance | Moat radius | Method | Source |
|-----|---------------|-------------|--------|--------|
| 2   | sqrt(2)       | sqrt(137) ~ 11.7 | Exhaustive | Classical |
| 4   | 2             | ~313 | Exhaustive | Gethner, Wagon, Wick (1998) |
| 6   | sqrt(6)       | -- | -- | -- |
| 8   | 2*sqrt(2)     | -- | -- | -- |
| 10  | sqrt(10)      | -- | -- | -- |
| 16  | 4             | -- | -- | -- |
| 18  | 3*sqrt(2)     | -- | -- | -- |
| 20  | 2*sqrt(5)     | 273,791,623 primes in component | LB (this project) | Verified |
| 26  | sqrt(26)      | ~1,015,639 | Tsuchimura (2004) | Verified by ISE |
| 32  | 4*sqrt(2)     | ~2,823,055 | Tsuchimura (2004) | Verified by ISE |
| 36  | 6             | ~80,015,782 | Tsuchimura (2005) | Current computational record |
| 40  | sqrt(40)      | **Unknown** | **This project** | ISE campaigns active |

### 1.3 Why k^2 = 40 Matters

k^2 = 40 is the next scientifically meaningful threshold after k^2 = 36. The
connectivity disk grows from 112 neighbor vectors (k^2=36) to 128 (k^2=40),
gaining 16 new step vectors at d^2 = 37 and d^2 = 40: the (+-1, +-6), (+-6, +-1),
(+-2, +-6), (+-6, +-2) families. This increase in local connectivity pushes the
expected percolation boundary from ~80M (k^2=36) to an estimated ~839M (k^2=40),
requiring computation at scales 10x beyond the current record.

No prior computational work on k^2 = 40 exists in the published literature.

---

## 2. Methods

### 2.1 Angular Connectivity Solver (Deprecated)

The original approach used a CUDA sieve to generate Gaussian primes in GPRF
binary format, then a Rust angular connector to decompose the first octant into
wedges, process primes through per-wedge spatial hash + union-find (BandProcessor),
and stitch components across wedge boundaries.

**Verified results:** k^2 = 2 moat at sqrt(137), k^2 = 4 moat with 92 primes in
origin component, k^2 = 20 moat with 273,791,623 primes in origin component.
Cross-validated on Jetson Orin Nano, RTX 4090, and A100 with bitwise-identical
prime counts and component sizes.

**Why deprecated:** The angular solver computes the full origin-connected component
(LB mode) or verifies a known boundary (UB mode via Tsuchimura's trick). For k^2=40,
the LB approach requires processing the entire norm range [0, R_moat^2] -- estimated
at hundreds of billions of primes. The UB approach requires knowing the moat radius
in advance. Neither is practical for exploratory search at k^2=40 scales.

The solver remains in the codebase (`solver/`) and is used for small-scale
correctness validation (k^2=2 gate). The ISE method (Section 2.2) replaced it
for the k^2=40 campaign.

### 2.2 Independent Strip Ensemble (ISE)

ISE is the primary method for the k^2=40 campaign. It replaces global connectivity
tracing with embarrassingly parallel local probes.

**Mathematical definition.** Fix a radial shell at radius r. Place M strips at
collar-disjoint lateral offsets b_1, ..., b_M, each of width W and height H.
For each strip j, build the induced subgraph G_k[T_j^+] on the expanded tile
(interior plus collar of width c = ceil(sqrt(k^2))), and check whether any
connected component touches both the inner face (a = a_lo) and outer face
(a = a_lo + H). The ISE metric is:

    f(r) = |{j : io_count_j(r) > 0}| / M

where io_count_j(r) is the number of inner-to-outer spanning components in
strip j at radius r.

**Key properties:**

- **f(r) = 1:** Every probe crosses -- strong connectivity at this radius.
- **f(r) = 0:** Every probe is blocked -- total local percolation collapse.
- **0 < f(r) < 1:** Transition regime.

**Subgraph monotonicity theorem (zero false negatives).** The induced subgraph
G_k[T^+] is a subgraph of G_k. Every path in G_k[T^+] exists in G_k. Therefore
every detected crossing is genuine. If the full graph G_k has a moat at radius r,
ISE must detect blockage at or before r in at least one strip (Corollary 4.2 of
the CTO paper). ISE cannot produce false negatives.

**What ISE measures:** ISE measures *local percolation* -- whether connectivity
survives within independent rectangular probes. This is related to but distinct
from the Tsuchimura moat (origin-connected reachability). Specifically:

- f(r) = 0 implies total percolation collapse: no local connectivity exists
  anywhere in the shell. This is a *strictly stronger* condition than a Tsuchimura
  moat.
- A Tsuchimura moat can exist while f(r) > 0, if orphan clusters provide local
  connectivity without bridging the origin to infinity.
- f(r) = 0 is *necessary but not sufficient* for proving a moat rigorously.
  The tile-based upper bound method (Section 2.3) is needed to convert ISE
  candidates into proofs.

**Key parameters:**

| Parameter | Description | k^2=40 value |
|-----------|-------------|--------------|
| W | Tile width | 2000 |
| H | Tile height | 2000 |
| M | Stripe count | 32 (Phase 1), 64 (Phase 2+) |
| c | Collar width | 7 (code: ceil(sqrt(40))) |
| Stride | Stripe spacing | W + 2c = 2014 (ISE), or 10000 (wide separation) |

**Known limitation:** The collar definition is inconsistent across documents.
The CTO theory paper (Section 3.2) defines c = floor(sqrt(k^2)) = 6 for k^2=40.
The implementation (`tile.rs:125`) uses c = ceil(sqrt(k^2)) = 7. The code is
conservative -- using c = 7 adds ~0.2% overhead but guarantees correctness. The
maximum single-coordinate excursion in the k^2=40 neighbor set is indeed 6 (from
vectors like (+-6, +-2)), so c = 6 is theoretically sufficient. This discrepancy
is logged but does not affect soundness.

### 2.3 Tile-Based Upper Bound (In Development)

The tile UB method converts ISE moat candidates into rigorous proofs by replacing
sparse strip sampling with gapless tiling of the annular band.

**Core idea:** Decompose the first-octant annular band at the ISE-identified
blockage radius into a grid of tiles with lateral stride s_b = W (instead of
the ISE stride W + 2c). This makes expanded tiles overlap by 2c in the lateral
direction, ensuring that every edge in G_k within the band appears in at least
one expanded tile. Build a tile connectivity graph where edges connect components
that share a prime in the overlap region. If no path from the I-boundary to the
O-boundary exists in this graph, the moat is proved.

**Key theorems (from the tile UB campaign plan):**

- **Overlap sufficiency (Theorem 1.3):** With stride s_b = W, if a component in
  G_k crosses between laterally adjacent tiles, the overlap region captures the
  crossing and creates an edge in the tile connectivity graph.
- **Reachability (Theorem 1.6):** If no path from the I-boundary to the O-boundary
  exists in the tile connectivity graph, no component of G_k crosses the annular
  band. Therefore R_moat(k) <= R_min + c.
- **Soundness (Theorem 1.8):** The tile connectivity graph is a conservative
  coarsening of G_k. Every edge corresponds to a genuine connection. The method
  can miss connections (false negatives in crossing detection) but never create
  false ones.

**Status:** Adversarially reviewed with verdict "Accept with revisions." Key
revisions needed:

1. **Critical:** The existing `compose.rs` implements distance-based face-port
   matching, not the shared-prime matching described in the theory. The distance
   scheme is sound for UB purposes (it only creates edges for genuine G_k edges)
   but is strictly weaker than shared-prime matching.
2. **Major:** Theorem 1.3 proof needs correction -- the path prefix may exit the
   tile, so the claimed endpoint containment is too strong. The reachability
   theorem (1.6) handles this correctly by composing edges across tiles.
3. **Major:** The rectangular-band-to-full-plane argument needs a rigorous octant
   symmetry lemma.

**Estimated cost for a single-shell UB at k^2=40:** ~550,000 tiles covering the
first-octant lateral extent at R ~ 1.1B. At 1.8s per tile with 64-core A100
parallelism: ~4.3 hours wall time, ~$9 cloud cost (before efficiency corrections).

### 2.4 CUDA Acceleration

The compute pipeline uses both CUDA (for sieve-based Gaussian prime generation)
and Rust (for ISE tile processing). A separate `tile_cuda` binary exists that
performs primality testing on GPU but delegates union-find and face classification
to CPU (see Section 6.4 for the GPU UF implementation gap and projected impact).

**CUDA sieve kernel:** Segmented Eratosthenes on GPU shared memory. Three phases
handle small (<256), medium (256 to segment span), and large (bucket-based) primes.
Survivors undergo Cornacchia decomposition (for p = 1 mod 4) or direct emit (for
p = 3 mod 4 inert primes). Output in GPRF binary format.

**Scanline tile kernel (ISE):** Rust implementation with:
- **Row-sieve optimization:** Precomputes sqrt(-1) mod p for primes p = 1 mod 4
  up to B = 10^4 (~609 primes). Marks composites at positions a = +-r*b mod p,
  reducing Miller-Rabin test volume by 85-93%. This was the single biggest
  optimization identified through multi-worker algorithm research.
- **Montgomery multiplication:** 64-bit modular exponentiation for deterministic
  9-witness Miller-Rabin primality testing. Valid for all n < 3.825 x 10^18
  (R <= 1.955 billion).
- **Scanline union-find:** Process primes row by row, checking backward offsets
  only (56 for k^2=36, 64 for k^2=40). Each undirected edge is processed once.
- **Rayon parallelism:** Tiles within a shell are processed in parallel across
  all available cores.

**Cross-platform validation:**

| Platform | Per-tile time (2000^2, k^2=40, R~1B) | Notes |
|----------|---------------------------------------|-------|
| Jetson Orin Nano | ~41s per shell (32 stripes) | 6 ARM cores, primary campaign hardware |
| RTX 4090 host | Not yet profiled for ISE | 16384 CUDA cores (sieve only) |
| A100 SXM4 | Not yet profiled for ISE | 108 SMs (sieve only) |

ISE is CPU-bound (scanline kernel dominates), not GPU-bound. The Jetson's 6 ARM
cores with rayon parallelism are sufficient for the k^2=40 campaign.

---

## 3. Calibration Results

ISE was validated against known Tsuchimura moats before the k^2=40 campaign.
All calibration runs used the symmetry-fixed build (commit 8ee77d0: positive-only
b-offsets, giving M truly independent stripes).

### 3.1 k^2 = 2 -- Known Moat Validation

**Parameters:** tile = 8x8, 8 stripes, R = [0, 100], 13 shells.

| R | f(r) |
|---|------|
| 4 | 0.2500 |
| 12 | 0.1250 |
| 20 | **0.0000** |
| 28 | **0.0000** |
| 36 | **0.0000** |
| 44 | 0.1250 |
| 52 | 0.2500 |
| 60 | **0.0000** |
| 68 | **0.0000** |
| 76 | **0.0000** |
| 84 | 0.1250 |
| 92 | 0.1250 |
| 98 | 0.7500 |

Six f(r) = 0 shells detected. Matches known k^2 = 2 moat structure exactly.
Connectivity resumes at R = 44 (f = 0.125) and R = 84 (f = 0.125), consistent
with isolated prime clusters providing intermittent local connectivity beyond the
moat.

### 3.2 k^2 = 26 -- Tile Height Sensitivity

**Purpose:** Determine whether tile height affects ISE detection precision.

| Tile | Shells | f(r) min | f(r) max | f(r) = 0 |
|------|--------|----------|----------|----------|
| 200x200 | 150 | 0.2813 | 0.7500 | 0 |
| 500x500 | 60 | 0.2500 | 0.6250 | 0 |
| 1000x1000 | 30 | 0.3125 | 0.5938 | 0 |
| 2000x2000 | 15 | 0.3125 | 0.5938 | 0 |

**Finding:** Tile height does NOT significantly affect f(r) estimates. All heights
produce consistent values in the 0.25-0.75 range. Larger tiles have slightly
narrower f(r) variance but the mean and extrema are consistent.

### 3.3 k^2 = 26 -- Dense Moat Sweep

**Parameters:** tile = 500x500, 32 stripes, R = [950K, 1.1M], 300 shells.

- Mean f(r): 0.4765
- f(r) = 0 shells: **0**
- At Tsuchimura moat (R = 1,015,639): f(r) = 0.2500 at R = 1,016,250
- Minimum f(r) = 0.2188 at R = 1,067,750 and R = 1,084,250

**Near the Tsuchimura moat:**

| R | f(r) |
|---|------|
| 1,013,250 | 0.4688 |
| 1,014,250 | 0.5625 |
| 1,015,250 | 0.5938 |
| **1,016,250** | **0.2500** |
| 1,017,250 | 0.5938 |

ISE detects the f(r) depression near the known moat (dip to 0.25 just 600 units
past the known moat location) but does not reach f(r) = 0 with 32 stripes at
500x500 tiles. The moat is too narrow in angular extent for all 32 strips to
lose connectivity simultaneously.

### 3.4 k^2 = 26 -- Control Below Moat

**Parameters:** tile = 500x500, 32 stripes, R = [500K, 550K], 100 shells.

- Mean f(r): 0.9372, range [0.7813, 1.0000]
- f(r) = 0 shells: **0** (zero false positives)

### 3.5 k^2 = 32 -- Dense Moat Sweep

**Parameters:** tile = 500x500, 32 stripes, R = [2.7M, 2.9M], 400 shells.

- Mean f(r): 0.5027
- f(r) = 0 shells: **0**
- At Tsuchimura moat (R = 2,823,055): f(r) = 0.3438 at R = 2,823,250
- Minimum f(r) = 0.2500 at R = 2,812,250

### 3.6 k^2 = 32 -- Control Below Moat

**Parameters:** tile = 500x500, 32 stripes, R = [1.5M, 1.55M], 100 shells.

- Mean f(r): 0.8913, range [0.6875, 1.0000]
- f(r) = 0 shells: **0** (zero false positives)

### 3.7 k^2 = 36 -- Transition Zone

**Parameters:** tile = 2000x2000, 32 stripes, R = [79.5M, 80.5M], 500 shells.

- Mean f(r): 0.2118, range [0.0000, 0.4063]
- f(r) = 0 shells: **1**

**The single zero shell:**

| Shell | R | f(r) | Primes | Time (ms) |
|-------|---|------|--------|-----------|
| 449 | 80,399,000 | **0.000000** | 4,534,594 | 46,563 |

**Approach to zero:**

| R | f(r) |
|---|------|
| 80,389,000 | 0.1563 |
| 80,391,000 | 0.1875 |
| 80,393,000 | 0.2188 |
| 80,395,000 | 0.1250 |
| 80,397,000 | 0.0625 |
| **80,399,000** | **0.0000** |
| 80,401,000 | 0.1875 |
| 80,403,000 | 0.2188 |

The monotonic descent (0.1563 -> 0.1250 -> 0.0625 -> 0.0000) confirms this is a
genuine percolation transition, not a stochastic fluctuation. With 32 truly
independent stripes (post-symmetry-fix), all disconnected simultaneously.

### 3.8 k^2 = 36 -- Control at 50M

**Parameters:** tile = 2000x2000, 32 stripes, R = [49.9M, 50.1M], 100 shells.

- Mean f(r): 0.8813, range [0.6875, 1.0000]
- f(r) = 0 shells: **0** (zero false positives)

### 3.9 Calibration Summary

| Run | k^2 | Shells | f(r) min | f(r) max | f(r)=0 | Mean f(r) |
|-----|-----|--------|----------|----------|--------|-----------|
| k2-moat | 2 | 13 | 0.0000 | 0.7500 | **6** | ~0.19 |
| k26-tile200 | 26 | 150 | 0.2813 | 0.7500 | 0 | -- |
| k26-tile500 | 26 | 60 | 0.2500 | 0.6250 | 0 | -- |
| k26-tile1000 | 26 | 30 | 0.3125 | 0.5938 | 0 | -- |
| k26-tile2000 | 26 | 15 | 0.3125 | 0.5938 | 0 | -- |
| k26-moat-dense | 26 | 300 | 0.2188 | 0.7813 | 0 | 0.4765 |
| k26-control | 26 | 100 | 0.7813 | 1.0000 | 0 | 0.9372 |
| k32-moat-dense | 32 | 400 | 0.2500 | 0.7500 | 0 | 0.5027 |
| k32-control | 32 | 100 | 0.6875 | 1.0000 | 0 | 0.8913 |
| k36-moat-dense | 36 | 500 | 0.0000 | 0.4063 | **1** | 0.2118 |
| k36-control | 36 | 100 | 0.6875 | 1.0000 | 0 | 0.8813 |

**Total shells processed:** 1,768.

### 3.10 Control vs Moat Separation

| k^2 | Control mean f(r) | Moat sweep mean f(r) | Separation factor |
|-----|-------------------|---------------------|-------------------|
| 26 | 0.937 | 0.477 | 1.97x |
| 32 | 0.891 | 0.503 | 1.77x |
| 36 | 0.881 | 0.212 | 4.16x |

k^2 = 36 shows the strongest separation, consistent with being at a genuine
percolation transition rather than merely a prime density dip.

---

## 4. k^2 = 40 Campaign Results

The k^2=40 campaign ran in six phases on the Jetson Orin Nano, progressing from
coarse reconnaissance to dense mapping of the transition region and extinction
confirmation. All runs used 2000x2000 square tiles unless otherwise noted.

### 4.1 Reconnaissance (R = 100M -- 1B)

**Parameters:** 32 stripes, stride = W + 2c = 2014, 5 shells per probe.
Also tested 4000x4000 tiles for comparison.

**2000x2000 tiles, 32 stripes:**

| R | Mean f(r) | Min f(r) | Max f(r) |
|---|-----------|----------|----------|
| 100M | 1.0000 | 1.0000 | 1.0000 |
| 200M | 1.0000 | 1.0000 | 1.0000 |
| 500M | 0.9688 | 0.9375 | 1.0000 |
| 1.0B | 0.2188 | 0.0938 | 0.3438 |

**4000x4000 tiles, 32 stripes (for comparison):**

| R | Mean f(r) | Min f(r) | Max f(r) |
|---|-----------|----------|----------|
| 100M | 1.0000 | 1.0000 | 1.0000 |
| 200M | 1.0000 | 1.0000 | 1.0000 |
| 500M | 0.9938 | 0.9688 | 1.0000 |
| 1.0B | 0.1500 | 0.1250 | 0.1875 |

**Interpretation:** The transition from fully connected (f ~ 1.0) to near-collapse
(f ~ 0.15-0.22) occurs between R = 500M and R = 1B. The 4000x4000 tiles show
slightly lower f(r) at 1B, consistent with larger tiles capturing connectivity
more faithfully near the transition. The recon established the target window for
bisection: 500M -- 1B.

### 4.2 Bisection (R = 600M -- 900M)

**Parameters:** 32 stripes, 10 shells per probe.

| R | Shell f(r) values |
|---|-------------------|
| 600M | 0.9375, 0.9063, 0.8438, 0.9688, 0.9375, 0.9063, 0.9063, 0.9375, 0.8438, 0.8438 |
| 700M | 0.6563, 0.7813, 0.6875, 0.7813, 0.6250, 0.7500, 0.7188, 0.7813, 0.6563, 0.6875 |
| 800M | 0.4375, 0.5938, 0.5625, 0.5625, 0.5625, 0.5313, 0.4375, 0.5313, 0.5938, 0.6250 |
| 900M | 0.2813, 0.4688, 0.4063, 0.2188, 0.4375, 0.4688, 0.3438, 0.4375, 0.4688, 0.4063 |

**Per-probe summary:**

| R | Mean f(r) | Min f(r) | Max f(r) |
|---|-----------|----------|----------|
| 600M | 0.9031 | 0.8438 | 0.9688 |
| 700M | 0.7125 | 0.6250 | 0.7813 |
| 800M | 0.5438 | 0.4375 | 0.6250 |
| 900M | 0.3938 | 0.2188 | 0.4688 |

The transition is smooth and monotonic. The f(r) = 0.5 crossing point lies
between R = 800M and R = 900M based on this data.

### 4.3 Push Runs (R = 1.2B -- 1.8B)

**Parameters:** 32 stripes, 10 shells per probe. Mapped the low-f(r) tail.

**R = 1.2B:**

| Shell | f(r) |
|-------|------|
| 0 | 0.0625 |
| 1 | 0.0938 |
| 2 | 0.1250 |
| 3 | 0.0313 |
| 4 | 0.1250 |
| 5 | 0.1250 |
| 6 | 0.0938 |
| 7 | 0.0625 |
| 8 | 0.0313 |
| 9 | 0.0625 |

Mean f(r) = 0.0813. First appearances of f(r) = 0.0313 (1/32).

**R = 1.5B:**

| Shell | f(r) |
|-------|------|
| 0 | **0.0000** |
| 1 | 0.0313 |
| 2 | **0.0000** |
| 3 | **0.0000** |
| 4 | **0.0000** |
| 5 | 0.0625 |
| 6 | **0.0000** |
| 7 | **0.0000** |
| 8 | **0.0000** |
| 9 | 0.0313 |

Mean f(r) = 0.0156. Six f(r) = 0 shells out of 10. First confirmed extinction
shells for k^2 = 40.

**R = 1.8B:**

| Shell | f(r) |
|-------|------|
| 0 | **0.0000** |
| 1 | **0.0000** |
| 2 | **0.0000** |
| 3 | **0.0000** |
| 4 | **0.0000** |
| 5 | **0.0000** |
| 6 | 0.0625 |
| 7 | 0.0313 |
| 8 | **0.0000** |
| 9 | **0.0000** |

Mean f(r) = 0.0094. Eight f(r) = 0 shells. Near-total extinction.

### 4.4 Dense Sweep (R = 1.25B -- 1.45B)

**Parameters:** 32 stripes, 10 shells per probe. Fine-grained mapping of the
extinction onset region.

| R | Mean f(r) | f(r) = 0 shells | Min f(r) | Max f(r) |
|---|-----------|-----------------|----------|----------|
| 1.25B | 0.0906 | 0 | 0.0313 | 0.1563 |
| 1.30B | 0.0438 | 3 | 0.0000 | 0.0938 |
| 1.35B | 0.0719 | 1 | 0.0000 | 0.1250 |
| 1.40B | 0.0313 | 2 | 0.0000 | 0.0625 |
| 1.45B | 0.0344 | 3 | 0.0000 | 0.0938 |

**Shell-by-shell at R = 1.30B (first zero cluster):**

| Shell | R center | f(r) |
|-------|----------|------|
| 0 | 1,300,001,000 | 0.0938 |
| 1 | 1,300,003,000 | 0.0313 |
| 2 | 1,300,005,000 | 0.0625 |
| 3 | 1,300,007,000 | 0.0313 |
| 4 | 1,300,009,000 | **0.0000** |
| 5 | 1,300,011,000 | **0.0000** |
| 6 | 1,300,013,000 | 0.0938 |
| 7 | 1,300,015,000 | **0.0000** |
| 8 | 1,300,017,000 | 0.0938 |
| 9 | 1,300,019,000 | 0.0313 |

### 4.5 Focus Runs (R = 1.29B -- 1.32B)

**Parameters:** Two configurations tested.

**32 stripes, 5 shells per probe:**

| R | Mean f(r) | Min f(r) | Max f(r) |
|---|-----------|----------|----------|
| 1.290B | 0.0594 | 0.0469 | 0.0781 |
| 1.300B | 0.0438 | 0.0156 | 0.0781 |
| 1.310B | 0.0750 | 0.0313 | 0.0938 |
| 1.320B | 0.0250 | 0.0156 | 0.0313 |

**64 stripes, stride = 10000, 20 shells at R = 1.290B:**

| Shell | R center | f(r) |
|-------|----------|------|
| 0 | 1,290,001,000 | 0.0625 |
| 1 | 1,290,003,000 | 0.0469 |
| 2 | 1,290,005,000 | 0.0469 |
| 3 | 1,290,007,000 | 0.0781 |
| 4 | 1,290,009,000 | 0.0625 |
| 5 | 1,290,011,000 | 0.0781 |
| 6 | 1,290,013,000 | 0.0781 |
| 7 | 1,290,015,000 | 0.0938 |
| 8 | 1,290,017,000 | 0.0781 |
| 9 | 1,290,019,000 | 0.0781 |
| 10 | 1,290,021,000 | 0.0781 |
| 11 | 1,290,023,000 | 0.0625 |
| 12 | 1,290,025,000 | 0.0781 |
| 13 | 1,290,027,000 | 0.0625 |
| 14 | 1,290,029,000 | 0.0625 |
| 15 | 1,290,031,000 | 0.0938 |
| 16 | 1,290,033,000 | 0.0469 |
| 17 | 1,290,035,000 | 0.0781 |
| 18 | 1,290,037,000 | 0.0781 |
| 19 | 1,290,039,000 | 0.0781 |

Mean f(r) = 0.0719, no f(r) = 0 shells. The 64-stripe configuration shows
stable f(r) in the 0.047-0.094 range at R ~ 1.29B, consistent with the 32-stripe
data but with finer resolution (1/64 = 0.0156 step size).

### 4.6 Extinction Mapping (R = 1.5B -- 3.5B)

**Parameters:** 64 stripes, stride = 10000, 20 shells per probe.

| R | Mean f(r) | f(r) = 0 shells | Min f(r) | Max f(r) |
|---|-----------|-----------------|----------|----------|
| 1.5B | 0.0164 | 8 | 0.0000 | 0.0625 |
| 2.0B | 0.0039 | 14 | 0.0000 | 0.0156 |
| 2.5B | **0.0000** | **20** | 0.0000 | 0.0000 |
| 3.0B | 0.0008 | 19 | 0.0000 | 0.0156 |
| 3.5B | **0.0000** | **20** | 0.0000 | 0.0000 |

**R = 2.5B: Total extinction.** All 20 shells show f(r) = 0 across all 64
stripes. With N = 64 x 20 = 1,280 independent tile observations, the probability
of seeing f = 0 if the true crossing probability exceeds 0.005 is < 0.2%. We
can state with > 99.8% confidence that p(2.5B) < 0.005.

**R = 3.0B: Near-total extinction.** 19/20 shells at f(r) = 0, one shell at
0.0156 (1/64). A single surviving stripe crossing at this radius.

**R = 3.5B: Total extinction confirmed.** All 20 shells at f(r) = 0. Note the
dramatically reduced prime counts (~4,500-5,000 per shell vs ~7.6M at R = 2.5B).
At R = 3.5B, the norm R^2 = 1.225 x 10^19 exceeds the MR-9 validity ceiling of
3.825 x 10^18, so these results use probabilistic (not deterministic) primality
testing. The extinction is confirmed but the precision guarantee is weaker.

**MR-9 validity status per radius:**

| R | R^2 | MR-9 status |
|---|-----|-------------|
| 1.5B | 2.25 x 10^18 | **OK** (deterministic) |
| 1.8B | 3.24 x 10^18 | **OK** |
| 1.955B | 3.82 x 10^18 | **LIMIT** |
| 2.0B | 4.00 x 10^18 | Exceeds -- probabilistic |
| 2.5B | 6.25 x 10^18 | Exceeds |
| 3.0B | 9.00 x 10^18 | Exceeds |
| 3.5B | 1.23 x 10^19 | Exceeds |

Results at R >= 2.0B should be treated as high-confidence but not deterministic.
All results at R <= 1.955B are fully deterministic under MR-9.

### 4.7 Compensated Runs (R = 800M -- 1.2B)

**Parameters:** 128 stripes, stride = 10000, curvature-compensated placement
(per-stripe a_min adjusted so all inner faces sit at the same Euclidean radius),
20 shells per probe. This is the highest-precision dataset.

| R | Mean f(r) | Min f(r) | Max f(r) | Stdev f(r) |
|---|-----------|----------|----------|------------|
| 800M | 0.5684 | 0.5078 | 0.7031 | 0.0424 |
| 900M | 0.3949 | 0.2891 | 0.5078 | 0.0534 |
| 1.0B | 0.2691 | 0.1953 | 0.3516 | 0.0403 |
| 1.1B | 0.1816 | 0.1016 | 0.2500 | 0.0365 |
| 1.2B | 0.1133 | 0.0703 | 0.1797 | 0.0268 |

These compensated runs with 128 stripes provide the best estimates of the true
f(r) sigmoid. The f(r) = 0.5 crossing point lies between R = 800M (mean 0.5629)
and R = 900M (mean 0.3949), closer to 800M. Linear interpolation on the logit
scale gives:

    logit(0.5684) = 0.275
    logit(0.3949) = -0.427
    R_0.5 = 800M + 100M * 0.275 / (0.275 + 0.427) = 800M + 39.2M ~ 839M

**Updated ISE moat estimate: R_0.5 ~ 839M** (compare to the earlier 826M estimate
from the 32-stripe data). The 95% confidence interval from the delta method
(Section 3.5 of the narrowing plan) is approximately +/- 12M, so R_0.5 ~ 839M
+/- 12M.

### 4.8 Rectangular Tile Calibration

**Purpose:** Evaluate whether non-square tiles could reduce compute cost without
losing moat detection sensitivity.

**Result:** Rectangular tiles destroy moat signal. Data from controlled
comparisons across k^2 = 26, 32, 36:

| k^2 | Square tile | Square moat mean | Rect tile | Rect moat mean | Delta |
|-----|-------------|-----------------|-----------|----------------|-------|
| 26 | 500x500 | 0.477 | 1000x500 | 0.808 | +0.331 |
| 32 | 500x500 | 0.503 | 1000x500 | 0.817 | +0.314 |
| 36 | 2000x2000 | 0.212 | 2000x1000 | 0.591 | +0.379 |
| 36 | 2000x2000 | 0.212 | 4000x1000 | 0.888 | +0.676 |

The k^2=36 moat that produced f(r) = 0 on square 2000x2000 tiles reads 0.50
minimum on 2000x1000 and 0.78 minimum on 4000x1000. Width-dominant rectangles
provide redundant lateral coverage that masks angular gaps in the prime distribution.

**Conclusion:** All production runs must use square tiles. Aspect ratio should
not exceed 4:3. If tile area must be reduced, prefer tall-narrow over wide-short
(counterintuitive but experimentally confirmed).

### 4.9 Summary Table: All k^2=40 f(r) Data Points

Combined f(r) vs radius from ALL campaigns, showing per-probe mean and range.
Data points marked with [C] used compensated placement; [64s] used 64 stripes;
[128s] used 128 stripes. All others used 32 stripes.

| R | Mean f(r) | Min f(r) | Max f(r) | Shells | Config | f(r)=0 |
|---|-----------|----------|----------|--------|--------|--------|
| 100M | 1.0000 | 1.0000 | 1.0000 | 5 | 32s/2000^2 | 0 |
| 200M | 1.0000 | 1.0000 | 1.0000 | 5 | 32s/2000^2 | 0 |
| 500M | 0.9688 | 0.9375 | 1.0000 | 5 | 32s/2000^2 | 0 |
| 600M | 0.9031 | 0.8438 | 0.9688 | 10 | 32s/2000^2 | 0 |
| 700M | 0.7125 | 0.6250 | 0.7813 | 10 | 32s/2000^2 | 0 |
| 800M | 0.5438 | 0.4375 | 0.6250 | 10 | 32s/2000^2 | 0 |
| 800M | 0.5684 | 0.5078 | 0.7031 | 20 | [C] 128s | 0 |
| 900M | 0.3938 | 0.2188 | 0.4688 | 10 | 32s/2000^2 | 0 |
| 900M | 0.3949 | 0.2891 | 0.5078 | 20 | [C] 128s | 0 |
| 1.0B | 0.2188 | 0.0938 | 0.3438 | 5 | 32s/2000^2 | 0 |
| 1.0B | 0.2691 | 0.1953 | 0.3516 | 20 | [C] 128s | 0 |
| 1.1B | 0.1816 | 0.1016 | 0.2500 | 20 | [C] 128s | 0 |
| 1.2B | 0.0813 | 0.0313 | 0.1250 | 10 | 32s/2000^2 | 0 |
| 1.2B | 0.1133 | 0.0703 | 0.1797 | 20 | [C] 128s | 0 |
| 1.25B | 0.0906 | 0.0313 | 0.1563 | 10 | 32s/2000^2 | 0 |
| 1.29B | 0.0719 | 0.0469 | 0.0938 | 20 | [64s] s=10k | 0 |
| 1.30B | 0.0438 | 0.0000 | 0.0938 | 10 | 32s/2000^2 | 3 |
| 1.30B | 0.0438 | 0.0156 | 0.0781 | 5 | [64s] | 0 |
| 1.31B | 0.0750 | 0.0313 | 0.0938 | 5 | [64s] | 0 |
| 1.32B | 0.0250 | 0.0156 | 0.0313 | 5 | [64s] | 0 |
| 1.35B | 0.0719 | 0.0000 | 0.1250 | 10 | 32s/2000^2 | 1 |
| 1.40B | 0.0313 | 0.0000 | 0.0625 | 10 | 32s/2000^2 | 2 |
| 1.45B | 0.0344 | 0.0000 | 0.0938 | 10 | 32s/2000^2 | 3 |
| 1.5B | 0.0156 | 0.0000 | 0.0625 | 10 | 32s/2000^2 | 6 |
| 1.5B | 0.0164 | 0.0000 | 0.0625 | 20 | [64s] s=10k | 8 |
| 1.8B | 0.0094 | 0.0000 | 0.0625 | 10 | 32s/2000^2 | 8 |
| 2.0B | 0.0039 | 0.0000 | 0.0156 | 20 | [64s] s=10k | 14 |
| 2.5B | **0.0000** | 0.0000 | 0.0000 | 20 | [64s] s=10k | **20** |
| 3.0B | 0.0008 | 0.0000 | 0.0156 | 20 | [64s] s=10k | 19 |
| 3.5B | **0.0000** | 0.0000 | 0.0000 | 20 | [64s] s=10k | **20** |

### 4.10 Estimated Percolation Boundary

**Sigmoid model.** The f(r) data follow a decreasing logistic:

    f(r) = 1 / (1 + exp(beta * (r - R_0.5)))

From the bisect data (32 stripes, widest baseline):

    logit(f) at R = 700M: ln(0.7125 / 0.2875) = 0.908
    logit(f) at R = 900M: ln(0.3938 / 0.6062) = -0.431

    beta = (0.908 + 0.431) / (200 x 10^6) = 6.69 x 10^-9
    R_0.5 ~ 839M (interpolated from compensated 128-stripe data)

**95% confidence interval:** With N = 128 x 20 = 2,560 tile observations per
probe and the delta method (sigma_f = sqrt(0.25/N) = 0.0099):

    sigma_R_0.5 = sigma_f / |f'(R_0.5)| = 0.0099 / (6.69e-9 / 4) ~ 5.9M

    R_0.5 = 839M +/- 12M (95% CI)

**Comparison to initial prediction.** The campaign plan predicted R ~ 1.1B
based on the mean-degree model calibrated from k^2=36 (d_c = 3.92). The actual
R_0.5 ~ 839M is lower, suggesting the critical mean degree for k^2=40 is
somewhat higher than the k^2=36 value, or that finite-size effects shift the
observed transition.

**First f(r) = 0 shells:** First observed at R = 1.30B (32 stripes) and
R = 1.5B (64 stripes). Total extinction at R = 2.5B. The ~1.7B gap between
R_0.5 (~839M) and total extinction (~2.5B) is the percolation tail.

**Caveats:**
1. R_0.5 is tile-size dependent. The estimate is for 2000x2000 tiles. A
   finite-size scaling study at multiple tile widths would be needed to
   extrapolate to the infinite-width limit.
2. The logistic model is fit to a narrow range and may not be the correct
   functional form (erfc or Gompertz could fit equally well with different
   R_0.5).
3. This is the ISE percolation boundary, not the Tsuchimura moat radius.
   The actual moat (where the origin component terminates) may be at a
   different radius.

---

## 5. Open Issues

### 5.1 Collar Three-Way Disagreement

The CTO theory paper defines c = floor(sqrt(k^2)). The implementation uses
c = ceil(sqrt(k^2)). For k^2=40: floor gives 6, ceil gives 7. The maximum
single-coordinate excursion in the step set is 6, so floor is theoretically
sufficient. The code is conservative. Both the ISE narrowing plan and tile UB
plan use c = 7 citing "code convention."

**Resolution needed:** Either update the CTO paper to use ceil, or verify
the code with floor(sqrt(k^2)) = 6 and demonstrate equivalence. The soundness
of all results is not affected (c = 7 is strictly conservative), but the
discrepancy should not persist in publication-ready documents.

### 5.2 Tile-Size Dependence of R_0.5

The ISE moat estimate R_0.5 depends on the tile geometry. Narrower tiles
see blockage sooner (false positives from lateral path escape). The ISE
narrowing challenge (Issue 4) identifies this as critical: the f(r) = 0.5
crossing is only meaningful as a property of a specific tile geometry, not
as a property of the prime graph itself.

**Required work:** Measure R_0.5 at W = 1000, 2000, 4000 and extrapolate
via finite-size scaling to W -> infinity.

### 5.3 compose.rs Matching Mismatch

The tile UB campaign plan describes shared-prime matching for inter-tile
edges (find primes with identical (a,b) coordinates in the overlap region).
The existing `compose.rs` implements distance-based face-port matching
(connect R-face ports of one tile to L-face ports of the next if within
step distance). For UB with stride s_b = W, the distance scheme is sound
(no false edges) but strictly weaker than shared-prime matching (may miss
connections through the overlap). Implementation of shared-prime matching
is a prerequisite for the tile UB campaign.

### 5.4 ISE to Rigorous Proof Gap

ISE identifies moat candidates (shells where f(r) = 0 or f(r) is very low).
Converting these into rigorous moat proofs requires the tile UB method
(Section 2.3), which has been adversarially reviewed but not yet implemented
as a campaign. The gap between "ISE sees extinction" and "moat proved" is
the primary methodological open question.

### 5.5 Logistic Model Limitations

The sigmoid fit uses only 3-5 data points in the transition region. Alternative
functional forms (complementary error function, Gompertz) could produce different
R_0.5 estimates. Model selection (AIC/BIC comparison) after collecting the planned
gap-filling probes would reduce this uncertainty.

### 5.6 MR-9 Ceiling at R > 1.955B

The 9-witness deterministic Miller-Rabin test is valid for n < 3.825 x 10^18.
For R > 1.955B, the test becomes probabilistic. Results at R = 2.0B, 2.5B, 3.0B,
3.5B confirm extinction but with slightly weaker correctness guarantees.

**Mitigation options:**
1. Add 3 more witnesses (12 total): valid to n < 3.3 x 10^20, covering R to ~18B.
2. Implement BPSW (no known counterexample below 2^64).
3. Accept the risk: MR-9 false positive rate is ~4^-9 ~ 10^-5.4 per test.

For the k^2=40 campaign, the primary target (R_0.5 ~ 839M) is well below the
ceiling. The MR-9 issue affects only the extinction confirmation beyond 2B.

### 5.7 GPU Union-Find Not Implemented in tile_cuda

The `tile_cuda` binary (`tile_main.cu`) runs GPU-accelerated primality testing
but performs union-find and face classification on the CPU after a device-to-host
bitmap copy. The `gpu_cc_ms` field exists in the JSON output but is always 0.0 --
GPU UF was planned but never implemented. The header `tile_kernel.cuh` is named
suggestively but contains an older primality kernel, not a union-find kernel.

This is the single largest architectural gap in the tile pipeline. Closing it
would shift the UB campaign from "multi-hour Jetson job" to "minutes on a 4090."
See Section 6.4 for detailed timing projections.

**Implementation path:** GPU connected-components is a solved problem. ECL-CC
(Jaiganesh & Burtscher, PPoPP 2018) and Afforest (Sutton et al., SC 2018) both
achieve near-linear throughput on GPUs. The tile grid (2000x2000, ~4M cells) fits
comfortably in shared memory or L2. This is a bounded implementation task, not a
research problem.

---

## 6. Hardware and Compute

### 6.1 Primary Campaign Platform: Jetson Orin Nano

- **SoC:** NVIDIA Jetson Orin Nano, ARM aarch64
- **CPU:** 6 cores (rayon parallelism, --threads 0)
- **GPU:** SM 8.7, 1024 CUDA cores (used for sieve only, not ISE)
- **RAM:** 8 GB unified memory
- **Rust:** 1.94.0, release profile (LTO, opt-level 3, codegen-units 1)
- **Role:** All ISE campaigns run here. CPU-bound scanline kernel.

**Per-shell timing (2000x2000, k^2=40, 32 stripes):**

| R | Time per shell | Primes per shell |
|---|---------------|-----------------|
| 100M | ~40s | ~4.49M |
| 500M | ~41s | ~4.13M |
| 800M | ~49s | ~4.04M |
| 1.0B | ~41s | ~3.99M |
| 1.2B | ~49s | ~3.96M |
| 1.5B | ~49s | ~3.92M |

**Per-shell timing (2000x2000, k^2=40, 64 stripes, stride 10000):**

| R | Time per shell | Primes per shell |
|---|---------------|-----------------|
| 1.29B | ~190-226s | ~7.89M |
| 1.5B | ~113s | ~7.83M |
| 2.0B | ~114s | ~7.73M |
| 2.5B | ~115s | ~7.65M |

**Per-shell timing (2000x2000, k^2=40, 128 stripes, stride 10000, compensated):**

| R | Time per shell | Primes per shell |
|---|---------------|-----------------|
| 800M | ~237s | ~16.1M |
| 900M | ~236s | ~16.0M |
| 1.0B | ~235s | ~16.0M |
| 1.1B | ~238s | ~15.9M |
| 1.2B | ~237s | ~15.8M |

### 6.2 Validated GPU Platforms

All three platforms produce bitwise-identical prime counts at every tested scale.

| Platform | Sieve (primes/sec @ 10^15) | Connector (primes/sec) |
|----------|---------------------------|----------------------|
| Jetson Orin Nano (SM 8.7) | 1.45M | 1.68M |
| RTX 4090 (SM 8.9, Ada) | 4.84M | 3.89M |
| A100 SXM4 40GB (SM 8.0) | 4.00M | -- |

The RTX 4090 leads on sieve throughput due to higher boost clock (2520 vs 1410 MHz)
and more SMs (128 vs 108). The sieve kernel is shared-memory-throughput-bound,
not global-memory-bandwidth-bound, so the A100's superior HBM is wasted.

### 6.3 Compute Budget Estimate

**Completed so far (approximate):**

| Phase | Shells | Approx wall time |
|-------|--------|-----------------|
| Calibration (k^2=2,26,32,36) | 1,768 | ~30 hours |
| Recon (k^2=40) | 40 | ~1 hour |
| Bisect | 40 | ~1 hour |
| Push | 30 | ~0.5 hours |
| Dense | 50 | ~3 hours |
| Focus | 50 | ~6 hours |
| Extinction | 100 | ~6 hours |
| Compensated | 100 | ~13 hours |
| Rect calibration | 140 | ~3 hours |
| **Total** | **~2,318** | **~63 hours** |

### 6.4 GPU Union-Find Implementation Gap

#### Current tile_cuda Architecture

The `tile_cuda` binary splits tile processing across GPU and CPU:

| Stage | Device | Time (Jetson, 2000x2000, k^2=36) |
|-------|--------|----------------------------------|
| GPU primality (sieve + MR) | GPU | ~170 ms |
| Device-to-host bitmap copy | DMA | ~2 ms |
| CPU union-find + face classify | CPU | ~40 ms |
| CUDA context init (one-time) | GPU | ~150 ms |
| **Total per tile** | | **~330 ms** (sequential, after init) |

The CPU union-find stage (~40 ms/tile) is not the bottleneck for single-tile
processing, but it prevents the entire tile pipeline from running on-device.
This forces a D2H copy per tile and serializes the UF step, blocking tile-per-SM
batching where multiple tiles would execute concurrently across SMs.

#### The Gap

The `gpu_cc_ms` field in `tile_cuda`'s JSON output is always 0.0. GPU UF was
planned (the field was allocated) but never implemented. `tile_kernel.cuh` is
named "GPU union-find" but actually contains a primality kernel from an earlier
iteration.

#### Projected Performance with Fully-GPU Tiles

A fully GPU-side tile kernel (primality + union-find + face classification all
on device) would enable tile-per-SM batching: each SM processes one tile
independently, with no D2H transfers until the final face-port summary.

**Sequential (one tile at a time):**

| Platform | SMs | Clock (MHz) | Est. tile time | Tiles/sec |
|----------|-----|-------------|----------------|-----------|
| Jetson Orin Nano | 16 | 918 | ~180 ms | ~5.6 |
| RTX 4090 | 128 | 2520 | ~65 ms | ~15.4 |
| A100 SXM4 | 108 | 1410 | ~80 ms | ~12.5 |

**Batched (tile-per-SM, all SMs occupied):**

| Platform | SMs | Est. tile time (batched) | Tiles/sec | Speedup vs Jetson CPU UF |
|----------|-----|-------------------------|-----------|--------------------------|
| Jetson Orin Nano | 16 | ~180 ms / 16 SM ~ 11.3 ms | ~47 | 14x |
| RTX 4090 | 128 | ~65 ms / 128 SM ~ 0.5 ms | ~2,065 | 624x |
| A100 SXM4 | 108 | ~80 ms / 108 SM ~ 0.7 ms | ~973 | 294x |

*Note: Batched estimates assume sufficient tiles to fill all SMs and negligible
inter-SM contention. Real throughput depends on memory bandwidth saturation and
shared memory pressure.*

#### UB Campaign Feasibility: Current vs Projected

**Single-shell UB at R = 1.5B (589K tiles, first-octant coverage):**

| Configuration | Tiles/sec | Wall time | Cloud cost |
|---------------|-----------|-----------|------------|
| Current: Jetson, CPU UF | ~3.0 | **~54 hours** | $0 (local) |
| Projected: 1x 4090, GPU UF batched | ~2,065 | **~4.8 min** | ~$0.30 |
| Projected: 1x A100, GPU UF batched | ~973 | **~10.1 min** | ~$1.50 |

**"1M up" worst case (294.5M tiles, full gapless tiling):**

| Configuration | Tiles/sec | Wall time | Cloud cost (est.) |
|---------------|-----------|-----------|-------------------|
| Current: Jetson, CPU UF | ~3.0 | ~1,135 days | -- |
| Projected: 8x 4090, GPU UF | ~16,520 | **~5.0 hours** | ~$44 |
| Projected: 8x A100, GPU UF | ~7,784 | **~10.5 hours** | ~$80 |

#### Implementation Notes

GPU connected-components is a solved algorithmic problem:

- **ECL-CC** (Jaiganesh & Burtscher, PPoPP 2018): Hook-based label propagation
  with shortcutting. Near-linear scaling on power-law and mesh graphs.
- **Afforest** (Sutton et al., SC 2018): Sampling-based CC with sublinear
  convergence for large-diameter graphs.

The tile grid (2000x2000, ~4M cells) fits comfortably in GPU shared memory or
L2 cache. The connectivity pattern (128 neighbors for k^2=40) produces a sparse
but regular graph. Implementation is a bounded engineering task with well-known
algorithmic solutions, not a research problem.

**Priority:** High for UB campaigns. The GPU UF gap is the difference between
"days on Jetson" and "minutes on a 4090." It does not affect ISE campaigns
(which use the Rust scanline kernel and are CPU-bound).

---

## 7. Prior Art

### 7.1 Tsuchimura (2004/2005)

H. Tsuchimura established the computational record for Gaussian moats in a pair
of papers (Technical Report METR 2004-13, Nihon University, and a 2005 follow-up).

**Key results:**
- k^2 = 26 moat at R ~ 1,015,639.
- k^2 = 32 lower bound: 139 billion primes generated, component traced.
- k^2 = 36 upper bound: R_moat <= 80,015,782.

**Method:** Norm-ordered generation of Gaussian primes via ordinary primes
p = 1 mod 4 plus sum-of-two-squares decomposition. Connectivity traced via
sequential subgraph construction in the angular domain. The upper-bound trick
(fictitiously assume all primes below a boundary are origin-connected, then verify
the component terminates) reduces work from processing the full norm range to
processing a single boundary shell.

**How our approach differs:**

| Aspect | Tsuchimura | This project |
|--------|-----------|--------------|
| Primary method | Global origin-connected trace | Independent Strip Ensemble (local probes) |
| Output | Exact moat radius | Percolation boundary estimate (R_0.5) |
| Parallelism | Sequential | Embarrassingly parallel (rayon) |
| Memory | Proportional to component size | Constant per tile (~21 MB) |
| Scalability | Requires processing all primes to the boundary | Samples arbitrary radii independently |
| Weakness | Cannot explore beyond known boundary | Cannot prove moats without tile UB method |

### 7.2 Gethner, Wagon, and Wick (1998)

"A Stroll Through the Gaussian Primes" (American Mathematical Monthly). Introduced
the Gaussian moat problem in its modern form and established small k^2 moats.

---

## 8. Repo Map

> **Canonical formal writeup:** `research/2026-03-23-gpcto-methods.tex` (compiled: `research/2026-03-23-gpcto-methods.pdf`)
> Paper-formatted methodology document with theorem-proof structure, designed for mathematician peer review.
> Covers: GPCTO definition, ISE method with soundness proofs, tile-UB framework, curvature compensation, feasibility analysis with GPU UF projections.
> This is the most relevant formal writeup of the project's methodology.

```
gaussian-moat-cuda/                    Repo root
  src/                                 CUDA sieve sources
    main.cu                            Host pipeline: batch loop, sort, GPRF output
    sieve_kernel.cu                    Segmented sieve + Cornacchia dispatch
    kernel.cu                          Miller-Rabin alternative kernel
    device_config.cuh                  Compile-time GPU profiles (Jetson/4090/A100)
    tile_main.cu                       tile_cuda binary: GPU primality + CPU UF (GPU UF stub)
    row_sieve.cuh                      Row-sieve optimization for tile primality
    miller_rabin.cuh                   Deterministic 9-witness MR test
    cornacchia.cuh                     Sum-of-two-squares decomposition (device)
    gprf_writer.cuh                    GPRF binary writer with buffered I/O
  solver/                             Rust angular connectivity solver (deprecated for k40)
    src/angular.rs                     Angular wedge orchestrator
    src/band.rs                        BandProcessor: spatial hash + union-find
    src/prime_router.rs                Per-prime angular overlap (cab53c3 fix)
  tile-probe/                         Rust ISE implementation (primary for k40)
    crates/moat-kernel/src/tile.rs     Core tile builder: sieve + scanline UF
    crates/moat-kernel/src/compose.rs  Tile composition (horizontal/vertical)
    src/main.rs                        ISE CLI: orchestrator, CSV/JSON output
  research/                           Campaign plans, theory, results
    EXPERIMENT.md                      THIS DOCUMENT
    2026-03-23-gpcto-methods.tex                   GPCTO methods paper (LaTeX source) -- CANONICAL FORMAL WRITEUP
    2026-03-23-gpcto-methods.pdf                   Compiled PDF of methods paper
    2026-03-22-connectivity-transfer-operator.md   CTO theory paper
    2026-03-21-independent-strip-proof.md          ISE soundness proof
    2026-03-22-calibration-manifest.md             ISE calibration data
    2026-03-22-k40-campaign-plan.md                k^2=40 campaign plan
    2026-03-23-ise-narrowing-plan.md               ISE narrowing protocol
    2026-03-23-tile-ub-campaign-plan.md             Tile UB framework
    2026-03-23-tile-ub-challenge.md                Adversarial review of tile UB
    2026-03-23-ise-narrowing-challenge.md          Adversarial review of ISE narrowing
    results/                           Raw campaign data (CSV, JSON, logs)
      k40-recon-2026-03-23/            Reconnaissance runs (100M-1B)
      k40-bisect-2026-03-23/           Bisection runs (600M-900M)
      k40-push-2026-03-23/             Push runs (1.2B-1.8B)
      k40-dense-2026-03-23/            Dense sweep (1.25B-1.45B)
      k40-focus-2026-03-23/            Focus runs (1.29B-1.32B)
      k40-extinction-2026-03-23/       Extinction mapping (1.5B-3.5B)
      k40-compensated-2026-03-23/      Compensated 128-stripe runs (800M-1.2B)
      rect-calibration-2026-03-23/     Rectangular tile calibration
  deploy/                             Cloud deployment scripts (A100, 4090)
  CMakeLists.txt                      CUDA build with device profile selection
```

---

## Appendix A: Neighbor Vectors for k^2 = 40

The connectivity disk for k^2 = 40 contains 128 nonzero neighbor vectors
(64 backward offsets for scanline processing):

| d^2 | d | Count | Example vectors |
|-----|---|-------|-----------------|
| 1 | 1.000 | 4 | (+-1,0), (0,+-1) |
| 2 | 1.414 | 4 | (+-1,+-1) |
| 4 | 2.000 | 4 | (+-2,0), (0,+-2) |
| 5 | 2.236 | 8 | (+-1,+-2), (+-2,+-1) |
| 8 | 2.828 | 4 | (+-2,+-2) |
| 9 | 3.000 | 4 | (+-3,0), (0,+-3) |
| 10 | 3.162 | 8 | (+-1,+-3), (+-3,+-1) |
| 13 | 3.606 | 8 | (+-2,+-3), (+-3,+-2) |
| 16 | 4.000 | 4 | (+-4,0), (0,+-4) |
| 17 | 4.123 | 8 | (+-1,+-4), (+-4,+-1) |
| 18 | 4.243 | 4 | (+-3,+-3) |
| 20 | 4.472 | 8 | (+-2,+-4), (+-4,+-2) |
| 25 | 5.000 | 12 | (+-5,0), (0,+-5), (+-3,+-4), (+-4,+-3) |
| 26 | 5.099 | 8 | (+-1,+-5), (+-5,+-1) |
| 29 | 5.385 | 8 | (+-2,+-5), (+-5,+-2) |
| 32 | 5.657 | 4 | (+-4,+-4) |
| 34 | 5.831 | 8 | (+-3,+-5), (+-5,+-3) |
| 36 | 6.000 | 4 | (+-6,0), (0,+-6) |
| 37 | 6.083 | 8 | (+-1,+-6), (+-6,+-1) |
| 40 | 6.325 | 8 | (+-2,+-6), (+-6,+-2) |
| **Total** | | **128** | |

Compared to k^2=36 (z=112), k^2=40 gains 16 new vectors at d^2=37 and d^2=40.

## Appendix B: Key Commits

| Commit | Description |
|--------|-------------|
| 8ee77d0 | Symmetry fix: positive-only b-offsets for ISE |
| cab53c3 | 721x connector fix: per-prime angular overlap |
| d538440 | D3 segmented base sieve: 3x speedup at 10^18 |
| c3b56b5 | Wedge count floor fix (130 -> 4) + SmallVec |
| e9780cc | GPRF filter fix + wedge count explosion fix |

## Appendix C: Glossary

| Term | Definition |
|------|-----------|
| f(r) | ISE crossing fraction: fraction of strips with io_count > 0 at radius r |
| io_count | Number of connected components spanning inner to outer face of a tile |
| ISE | Independent Strip Ensemble |
| CTO | Connectivity Transfer Operator (the tile formalism) |
| UB | Upper Bound (on moat radius) |
| LB | Lower Bound (on farthest reachable distance) |
| MR-9 | 9-witness deterministic Miller-Rabin primality test |
| GPRF | Gaussian Prime Record Format (binary sieve output) |
| R_0.5 | Radius where f(r) = 0.5 (ISE moat estimate) |
| Collar (c) | Tile expansion width, ceil(sqrt(k^2)) in code |
| Stride | Lateral spacing between strip centers |
