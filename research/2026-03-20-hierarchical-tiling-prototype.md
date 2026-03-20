---
date: 2026-03-20
type: working-prototype
status: draft
engine: opus-4.6
topic: hierarchical tiling solver with GPU acceleration
---

# Hierarchical Tiling Gaussian Moat Solver — Working Prototype

## 1. GPU Kernel for Level 0 Tiles

**Assessment: Yes, with sparse parallel union-find using hash tables.**

At ~100K primes per tile with 128 step offsets per prime, total edge-check work per tile is ~12.8M lookups. Right algorithm is sparse parallel union-find using hash tables in shared/global memory.

**Concrete design:**
```
Per tile (one CUDA block cluster, 4-8 blocks cooperating):
1. Load 100K primes into hash table in global memory (key: (a,b) → slot index)
2. Initialize UF array: parent[i] = i for all 100K primes
3. For each prime i (distributed across warps):
     For each of 128 offsets d with |d|² ≤ k²:
       j = hash_lookup(a_i + d.a, b_i + d.b)
       if j exists: atomic_union(i, j)
4. Path compress, extract boundary operator
```

Hash table: 256K-slot open-addressing, load factor 0.4, avg probe length ~1.3. On A100: ~0.5-2ms per tile. 216 tiles in flight simultaneously. 100K tiles total → <0.5 seconds for all Level 0. Expected ~2000x speedup over CPU Rust solver. Engineering cost: ~800-1200 LOC CUDA, 1-2 weeks.

## 2. Boundary Port Regularization

Growth is logarithmic, not exponential. At Level N, each face has ~25 × 2^N ports.

Memory budget across all levels: ~76.5MB total. No compression needed through Level 15.

Regularization algorithm (for extreme cases, Level 10+):
After merge at level L:
1. Identify connected groups of boundary ports on each face (same UF component AND within angular distance < epsilon)
2. Replace each group with a single representative port
3. Remap the UF parent array
Criterion: epsilon = sqrt(k) × 2^(L-10), only activate above Level 10.
Exactness: ZERO loss — merging ports already in same component AND angularly adjacent preserves all connectivity.

## 3. Edge Cases on Tile Edges

Use polar-aligned tiles (constant-radius inner/outer faces, constant-angle left/right faces). The (a,b) lattice is axis-aligned everywhere — no orientation issues.

Overlap collar: extends √k beyond each face. At R=1B, angular collar = arcsin(6.3/1e9) ~ 6.3e-9 radians. Trivially small.

Angular wrap-around: with D4 symmetry exploitation, work in first octant (0 to π/4). Wrap at boundaries using symmetry reflections — same as existing stitcher.rs.

No new edge cases beyond what the existing angular decomposition handles.

## 4. GPU Matrix Operations for Composition

Boolean transfer matrix T per tile: P_in × P_out, where T[i][j] = 1 iff inner port i reaches outer port j. Composition = boolean matmul (OR-of-AND semiring).

Implementation on INT8 tensor cores: pack 8 booleans per byte, use WMMA instructions, threshold at >0. A100 INT8: 624 TOPS. Level-10 composition (25,600 × 25,600 matrix): <0.1ms.

Break-even: Level 0-3 (25-200 ports) → CPU faster. Level 4-8 → GPU wins if batched. Level 9+ → GPU dominant, 10-100x over CPU.

~200 LOC CUDA using cuBLAS-style approach. Makes entire pipeline GPU-resident.

## 5. Validation Below Known Moats

**Phase V1** (k²=2, 4, 9): Smoke test at trivially small radius. ~1 tile.
**Phase V2** (k²=20, 26): Multi-tile test. k²=20 moat at R=133,679, ~20 tiles. Gate: origin component size matches existing solver bit-exact.
**Phase V3** (k²=20-36): Observable tracking. Track crossing count C(R), conductance G(R), Lyapunov γ(R), largest angular gap θ_gap(R).
**Phase V4** (k²=36): Full calibration run. Moat at R < 80,015,782. ~1,250 tiles. Expected <10 minutes on A100. Establishes γ(R) calibration baseline for k²=40.

## 6. Additional Issues Identified

**A. Prime binning:** Existing sieve produces primes in norm order. Need per-radial-band sieve (launch sieve once per radial band, route primes to tiles). ~625K bands for R=80M, ~31 seconds total.

**B. Dual representation:** Union-find partition at low levels (fast composition via seam union), boolean transfer matrix at high levels (fast composition via matmul). Crossover at Level 8.

**C. Adaptive angular tiling:** Fixed angular width → load imbalance (inner tiles sparse, outer tiles dense). Fix: choose angular tile width so each tile has ~100K primes. Round to power-of-2 subdivisions of π/4 for alignment.

**D. Origin bootstrap:** Origin (1,1) is not on the inner face of the annulus. Run existing Rust solver for R < 10,000 to establish origin component footprint. One-time computation, seconds.

## 7. Full Algorithm

```
HIERARCHICAL GAUSSIAN MOAT DETECTOR

PHASE 0: ORIGIN BOOTSTRAP
  Run existing Rust solver for R in [0, 10000].
  Extract: primes on circle |z|=10000 connected to origin.

PHASE 1: TILE GENERATION
  Radial bands: width W=128. Per band at radius R:
    Angular tile width: delta_theta = M / (rho(R) * W * R)
    Round to nearest pi/(4 * 2^j) for alignment.

PHASE 2: LEVEL 0 (GPU, parallel)
  Per tile: CUDA sieve → hash table → sparse union-find → boundary operator.
  A100: 216 tiles in flight, ~1ms/tile.

PHASE 3: HIERARCHICAL COMPOSITION
  Level 1-7: CPU union-find merge.
  Level 8+: GPU boolean matmul on tensor cores.

PHASE 4: ANGULAR WRAP
  Merge first/last angular columns with D4 symmetry.

PHASE 5: MOAT CHECK + OBSERVABLES
  Apply origin boundary condition. Propagate through composed transfer matrix.
  Track C(R), G(R), γ(R), θ_gap(R).
```

## 8. Performance Estimates

For k²=40 at R_max=1B:
- Total tiles: ~2.5M
- Phase 2 on A100: ~12 seconds
- Phase 3: ~5 seconds
- Prime generation: ~5 minutes
- **Total: ~6 minutes for full annular sweep to R=1B**

Current solver: hours for k²=36 at R=80M. This is **100-1000x improvement**.

## Document History

- 2026-03-20: Refined from brainstorm session
- Original seed ideas: Nikita (connectivity operator, single kernel per tile, translate to (0,0), flow/electricity, matrix operations, recursive tiling)
- GPT 5.4 Pro: PTO design, shell schema, cost estimates, density normalization correction
- Codex 5.3 xhigh: algorithmic analysis (cell-list, DSU+CSR, microtile CCL, int16 packing)
- Gemini 3.1 Pro: number-theoretic input (Hecke, Rudnick-Waxman, angular correlations)
- Grok 4.20 Heavy: lateral thinking (percolation analogues, probabilistic reformulation)
- Opus 4.6: pressure-testing, refinement, GPU kernel design, validation plan
- R. Jenkins (coordinator): synthesis, challenge, sequencing
