---
date: 2026-03-22
engine: coordinator
status: complete
topic: Optimal algorithm for Gaussian prime connectivity tile operator
method: 4-worker parallel deep research (Codex 5.4 High, read-only + network)
---

# Tile Operator Algorithm Research

The tile operator takes a rectangle in Z[i], finds all Gaussian primes, builds the k^2-adjacency graph, and reports face connectivity (I->O, I->L, etc.). Current: Rust scanline kernel, 1.23s per 2000^2 tile on Mac, 4.81s on Jetson. Primality is ~90% of runtime.

## Executive Summary

**Rust CPU:** Keep two-phase (bitmap then sparse CC), but replace per-point trial division with a row-sieve for p = 1 mod 4 up to ~10^4. This cuts MR test volume by ~14x. For CC, use sparse cell-list union-find over compacted primes only.

**CUDA GPU:** Two-kernel pipeline. Kernel 1: lattice-aware row-sieve + deterministic MR on survivors, writing occupancy bitmap. Kernel 1.5: stream compaction + spatial cell binning. Kernel 2: sparse hook-to-min CC with separate compression passes over compacted primes (~140K vertices, ~260K edges). Do NOT fuse primality and CC into one kernel.

**Key technique we are missing:** Lattice-structure row-sieve. The norms a^2+b^2 have algebraic structure: for primes p = 1 mod 4, composites fall on two residue classes per row (a = +/-r*b mod p where r^2 = -1 mod p). Sieving by these arithmetic progressions is dramatically cheaper than per-point trial division and can eliminate ~85-93% of composites before any MR test.

---

## 1. Best Rust CPU Algorithm

### Recommendation: Row-Sieve + Sparse Cell-List Union-Find

**Phase 1 -- Primality via row-sieve:**
- Precompute sqrt(-1) mod p for all primes p = 1 mod 4 up to B = 10^4 (~609 primes)
- For each row b, mark composites at positions a = +/-r*b mod p for each sieve prime
- Also mark p = 3 mod 4 intersection composites (p|a and p|b, very sparse)
- Survivors (~7% of grid at B=10^4, vs 3.4% actual primes) get deterministic MR
- This replaces trial division of ~30 small primes per point with bulk marking

**Estimated speedup from row-sieve:** Currently primality is ~90% of 1.23s = ~1.1s for 4M points. Row-sieve reduces MR tests from 4M to ~286K (14x reduction). Even if sieve setup costs 0.1s, MR phase drops from ~1.1s to ~0.08s + 0.1s sieve = ~0.18s. Total tile time: ~0.3s (4x speedup).

**Phase 2 -- Sparse CC:**
- Compact prime list in scan order (~140K entries)
- Bin primes into spatial cells (e.g., 64x64 cells covering the 2000^2 grid)
- Union-find over primes only, checking 112 neighbor offsets against cell bins
- This avoids the current dense parent[4M] array; parent[140K] fits in L2 cache

**Why not single-pass scanline?** The current scanline approach works but maintains dense UF state over all 4M sites. Two-phase with sparse compaction reduces CC memory from ~20MB to ~1MB and improves cache behavior. The row-sieve also integrates naturally with phase separation.

---

## 2. Best CUDA GPU Algorithm

### Recommendation: Three-Stage Pipeline

**Stage 1: Primality bitmap (1 CUDA kernel)**
- One thread per lattice point (4M threads, trivially parallel)
- Option A (simpler): Per-point trial division of small primes in __constant__ memory, then 9-witness deterministic MR. Straightforward, each thread independent.
- Option B (faster): GPU row-sieve. One warp per row, mark composites from precomputed sieve tables in shared memory, then MR only on survivors. ~14x fewer MR calls.
- Output: 1-bit occupancy bitmap (~500KB)
- MR implementation: Montgomery multiplication via PTX mul.hi.u64 + mad.lo.cc.u64 for 64-bit modular exponentiation without __int128

**Stage 1.5: Stream compaction + spatial binning (1-2 kernels)**
- CUB/Thrust-style select: compact bitmap to dense prime list (~140K entries)
- Histogram + prefix sum + scatter into spatial cell bins
- This is cheap relative to primality (~1-2ms)

**Stage 2: Sparse connected components (1-3 kernels)**
- Graph: ~140K vertices, ~260K undirected edges (avg degree ~3.8)
- Algorithm: Hook-to-min with separate compression passes
  - Why hook-to-min over ECL-CC: Strongest correctness guarantees. Only monotone parent updates (atomicMin). Separate pointer-jumping passes avoid concurrent find/union races. For a 260K-edge graph, the extra passes cost microseconds.
  - ECL-CC is faster for million-edge+ graphs but adds correctness complexity we don't need at this scale.
- Memory: parent[140K] (compact IDs) + occupancy bitmap for O(1) neighbor checks
- Neighbor generation: procedural (112 offsets), not materialized CSR. Check bitmap for occupancy, look up compact ID from dense map.
- Do NOT build CSR over all 4M sites.

**Why two stages, not fused?**
- Phase 1 is compute-bound (modular exponentiation). Phase 2 is memory/atomic-bound (pointer chasing, CAS). Different resource profiles = different optimization strategies.
- Phase separation enables compaction: CC operates on 140K vertices, not 4M. This is 28.5x less work and fits in L2 cache.
- Two kernel launches add ~10us overhead. The work reduction is orders of magnitude larger.

### GPU Memory Layout

| Array | Size | Purpose |
|-------|------|---------|
| occupancy bitmap | 500 KB (1 bit/site) | O(1) neighbor existence check |
| site_to_compact_id | 16 MB (u32 per site) | Map lattice position to compact vertex ID |
| compact_prime_list | 1.1 MB (2x u16 per prime) | (a,b) coordinates of each prime |
| parent | 560 KB (u32 per prime) | Union-find parent array |
| **Total** | **~18 MB** | Fits comfortably in GPU memory |

Alternative: skip the dense site_to_compact_id by using the occupancy bitmap + prefix-sum to compute compact IDs on the fly. Trades 16MB memory for a bit of compute in phase 2.

---

## 3. Key Technique We Were Missing: Lattice Row-Sieve

This is the single biggest optimization opportunity. The current approach tests every point independently. But for norms n = a^2 + b^2:

- For primes p = 1 mod 4: p divides n iff a = +/-r*b (mod p) where r^2 = -1 (mod p). This means composites divisible by p fall on exactly 2 arithmetic progressions per row.
- For primes p = 3 mod 4: p divides n only if p|a AND p|b. These are very sparse (hit only at multiples of p in both coordinates).

**Sieving with ~609 primes up to 10^4 removes ~93% of composites**, leaving only ~7% as MR candidates. Since MR is 90% of runtime, this transforms the bottleneck.

On GPU, row-sieve maps to: one warp per row, shared memory for sieve marks, mark 2 positions per sieve prime per row. Very regular, no divergence.

### Historical precedent

Tsuchimura (2005), who holds the computational record for Gaussian moats (k <= 6), used norm-ordered generation via ordinary primes p = 1 mod 4 plus sum-of-two-squares decomposition -- not brute-force primality testing. His bottleneck was connectivity, not primality. We should follow the same principle: make primality cheap via number-theoretic structure, then focus optimization on CC.

---

## 4. Architecture Decision: One Kernel or Two?

**Two kernels (recommended).** Clear separation:

| Property | Primality kernel | CC kernel |
|----------|-----------------|-----------|
| Parallelism | Embarrassingly parallel | Data-dependent (atomic UF) |
| Bottleneck | Compute (modular exponentiation) | Memory (pointer chasing) |
| Occupancy | High (uniform work) | Lower (divergent find depths) |
| Register pressure | High (Montgomery state) | Low (parent, rank) |

Fusing would force both to use the worst-case register allocation and would prevent compaction between phases.

---

## 5. Recommended Implementation Order

### Phase A: Rust improvements (low-risk, high-impact)

1. **Row-sieve for p = 1 mod 4** (expected 3-4x speedup on primality phase)
   - Precompute table of (p, r) where r = sqrt(-1) mod p for p = 1 mod 4, p < 10^4
   - Sieve each row: mark positions a = r*b mod p and a = (p-r)*b mod p as composite
   - Run MR only on survivors
   - Complexity: ~200 lines, self-contained

2. **Sparse CC with cell binning** (expected 2x speedup on CC phase)
   - Compact primes into sorted list
   - Bin into spatial cells (e.g., 32x32 or 64x64)
   - Union-find over compact IDs, neighbor lookup via cell bins
   - Complexity: ~150 lines, replaces current dense scanline UF

3. **Benchmark and tune sieve bound B**
   - Test B = 10^3, 10^4, 10^5
   - Find the crossover where sieve setup cost exceeds MR savings

### Phase B: CUDA implementation (high-impact, higher effort)

4. **GPU primality kernel with per-point MR** (baseline)
   - Port current MR to CUDA with Montgomery via PTX intrinsics
   - One thread per site, write bitmap
   - This alone should be 10-50x faster than Rust CPU

5. **GPU stream compaction + sparse CC**
   - CUB DeviceSelect for compaction
   - Hook-to-min CC on compact graph
   - Verify against Rust CC output

6. **GPU row-sieve** (optimization on top of baseline)
   - Move sieve to GPU: one warp per row, shared memory sieve marks
   - MR only on survivors
   - Expected additional 5-10x on top of baseline GPU kernel

### Phase C: Pipeline integration

7. **Tile boundary operator extraction** from sparse CC output
8. **Multi-tile composition** using boundary operators

---

## 6. Literature Findings

### Key computational precedents

- **Tsuchimura (2005)** holds the published record: k <= 6 (sqrt(36)), reachable distance < 80,015,782. His method: norm-ordered prime generation from p = 1 mod 4 + sum-of-squares decomposition, then sequential subgraph construction with arborescence CC and hash by imaginary part. Connectivity was his bottleneck, not primality.
- **Gethner, Wagon, Wick (1998)** established the BFS framework for k <= sqrt(26). Level-by-level BFS, octant symmetry, two BFS levels in memory.
- **No post-2005 publication improves Tsuchimura's bound.** Our ISE approach is genuinely novel.

### GPU CC algorithms (for our problem scale)

- **ECL-CC** (Jaiganesh & Burtscher 2018): Best general GPU CC. 3-7 GEdges/s on Pascal. But our graph is too small (~260K edges) to benefit from its edge-degree specialization.
- **Hook-to-min + compression**: Safest correctness. Only monotone parent updates. Best fit for our ~260K edge graph where CC is not the bottleneck.
- **Afforest**: Wins when one giant component dominates. Not clearly applicable since we don't know component structure a priori.

### Percolation theory connections

- **Hoshen-Kopelman (1976)**: Foundational lattice CCL via scanline UF. Our current approach is essentially this.
- **Newman-Ziff (2000/2001)**: Incremental UF over monotone process. Relevant if we want to study connectivity vs step size k.
- **Malarz & Galam (2005), Xun et al. (2021)**: Extended-range percolation thresholds. Confirms our ~3.4% density is well below nearest-neighbor percolation threshold but with 112-neighbor connectivity, we're in a different regime.

### No existing GPU Gaussian integer implementation found

No peer-reviewed GPU implementation for Gaussian integer arithmetic or Gaussian-prime connectivity. This is genuinely new territory.

---

## 7. Risk Assessment

| Risk | Mitigation |
|------|-----------|
| Row-sieve precomputation of sqrt(-1) mod p is wrong | Verify: for each sieve prime p, confirm r^2 + 1 = 0 mod p. Unit test against brute-force. |
| Sparse CC misses connections at cell boundaries | Cell size must be >= 2*k (neighbor radius). With k=6, cells of 16x16 or larger are safe. |
| GPU Montgomery arithmetic has off-by-one | Test against CPU MR for all primes in a small tile. Exact match required. |
| Row-sieve setup cost exceeds savings for small tiles | Benchmark at multiple tile sizes. For 2000^2, sieve should win decisively. |

---

## Sources

### Gaussian Moats
- Gethner, Wagon, Wick (1998). "A Stroll Through the Gaussian Primes." American Mathematical Monthly.
- Tsuchimura (2005). "Computational Results for Gaussian Moat Problem." IEICE Trans.
- Gethner, Stark (1997). "Periodic Gaussian Moats." Experimental Mathematics.
- Vardi (1998). "Prime Percolation." Experimental Mathematics.
- Jordan, Rabung (1970). "A Conjecture of Paul Erdos Concerning Gaussian Primes." Math. Comp.

### GPU Connected Components
- Jaiganesh, Burtscher (2018). "A High-Performance Connected Components Implementation for GPUs." HPDC.
- Soman, Narang, Kothapalli (2010). "Fast GPU Algorithms for Graph Connectivity."
- Hawick, Leist, Playne (2010). "Parallel graph component labelling with GPUs and CUDA."
- Kalentev et al. (2011). "Connected component labeling on a 2D grid using CUDA." JPDC.

### Primality and Sieving
- Sorenson, Webster (2016). Deterministic MR witness bounds. Math. Comp.
- CUDASieve (curtisseizert/CUDASieve). GPU segmented sieve.
- Atkin, Bernstein (2004). "Prime sieves using binary quadratic forms." Math. Comp.

### Percolation
- Hoshen, Kopelman (1976). Cluster labeling. Phys. Rev. B.
- Newman, Ziff (2000/2001). Efficient percolation algorithm. Phys. Rev. Lett. / Phys. Rev. E.
- Malarz, Galam (2005). Extended-range percolation. Phys. Rev. E.
