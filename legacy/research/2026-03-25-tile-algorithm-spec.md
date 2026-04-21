---
date: 2026-03-25
engine: coordinator
status: complete
topic: First-principles algorithm spec for Gaussian prime tile processing at R~1B
method: Codex 5.4 xhigh deep analysis + codebase audit + literature scan
---

# Tile Algorithm Spec: Gaussian Prime Processing at R ~ 1B

The inner loop. Tens of millions of tiles. Every operation counted.

---

## 0. Critical Density Correction

**The prior estimate of ~15K primes per tile was wrong by ~8x.**

The Gaussian prime density in the first quadrant at norm n is 4/(pi * ln n), not 1/(2*pi*ln(R^2)). At R ~ 1B, n ~ 2e18, ln(n) ~ 42.14:

- Expanded tile side: S = W + 2c = 2000 + 14 = 2014
- Total lattice points: S^2 = 4,056,196
- Parity-valid points (a+b odd): S^2/2 = 2,028,098
- **Expected Gaussian primes: P ~ 4*S^2 / (pi * ln n) ~ 123,000**
- Expected edges (k^2=40, 34 parity-compatible backward offsets): E ~ 252,000

This changes everything about the cost model. The output is O(W^2) -- you cannot beat area-linear for this tile size at this radius.

---

## 1. Algorithm Recommendation: Row-Sieve to L ~ 110K + Deterministic MR

### Why this wins

The survivor fraction after sieving with splitting primes up to L is:

    P_surv(L) = prod_{p<=L, p=1(4)} (1 - 2/p) * prod_{p<=L, p=3(4)} (1 - 1/p^2) ~ 1.3194 / ln(L)

The total cost per tile is:

    T(L) = C_sieve(L) + 1440 * N_MR(L)

where N_MR(L) ~ 2,028,098 * 1.3194 / ln(L) and the sieve cost has a closed-form approximation. Differentiating and solving gives **L* ~ 110,000** (precise discrete evaluation: L ~ 105K-110K).

### Concrete operation counts per tile

| Sieve limit L | Sieve ops | Survivors (MR candidates) | MR ops (128-bit muls) | Total ops |
|---------------|-----------|---------------------------|----------------------|-----------|
| 10,000 (current) | 1.10e7 | 290,000 | 4.17e8 | **4.28e8** |
| **~110,000 (optimal)** | **3.92e7** | **231,000** | **3.32e8** | **3.72e8** |
| 1,000,000 | 2.46e8 | 194,000 | 2.79e8 | 5.25e8 |

**The optimum is L ~ 110K, saving ~13% over current L=10K.** Going to L=1M is over-sieving -- setup costs exceed MR savings.

The improvement from L=10K to L=110K is real but not dramatic because the current sieve already captures most of the easy composites. The 93% composite elimination rate at L=10K only improves to ~96% at L=110K.

### Why not larger L?

The sieve cost per additional prime p has two components:
- **Setup**: compute residue (a mod p)*r mod p -- costs O(1) per row per prime
- **Marking**: stride through ~S/p positions -- costs O(S/p) per row per prime

For large p, setup dominates marking. At L=110K there are ~5,136 splitting primes (vs ~609 at L=10K). Each adds a residue computation per row but eliminates very few additional composites. The crossover is sharp.

---

## 2. Can MR Be Avoided Entirely?

**No, not per-tile.** Full sieve to sqrt(max_norm) ~ 1.4e9 requires:

- ~35.3M splitting primes + ~35.3M inert primes
- Split setup across all rows: 2 * S * pi_{1,4}(sqrt(n)) ~ 1.42e11 ops
- Inert row checks: S * pi_{3,4}(sqrt(n)) ~ 7.12e10 ops
- **Total: ~2.13e11 lightweight ops per tile**

This is ~570x more work than the optimal hybrid (3.72e8). The bottleneck is not marking (which is only ~1.06e7 total marks) but the per-prime-per-row setup/check cost. Each of 70.7M primes needs at least one modular operation per row, and there are 2014 rows.

**Verdict: MR at 1440 muls/candidate on 231K candidates is vastly cheaper than setup for 70.7M sieve primes.**

---

## 3. Why Inverse Enumeration (Cornacchia) Fails for Tiles

For a fixed row (fixed a), the norm range is [a^2 + b_lo^2, a^2 + b_hi^2], spanning ~4e9. Primes in that range: ~10^8. You must enumerate all of them to find the ~60 that decompose with b in [b_lo, b_hi].

The waste ratio: Q/P ~ pi*R*(W+B) / (4*B*W). For a single tile (B=W=2000), this is ~785,000. You enumerate ~785K primes per useful hit.

Cornacchia only becomes competitive when B = O(R), meaning you process a full annular strip, not a tile. This would require redesigning the entire tiling architecture.

---

## 4. Fused vs Separate: Keep Them Separate

### Cache analysis

- **Occupancy bitmap**: ~507 KB (1 bit per S^2 point). Fits in L2 with room to spare.
- **Sparse UF over primes**: parent[123K] ~ 492 KB. Fits in L2.
- **Dense UF over all points** (the wrong approach): parent[4.06M] ~ 16 MB. Thrashes L2, bleeds into L3.

### The real question is sparse vs dense UF, not fused vs separate

With sparse UF over compacted primes (~123K entries + spatial bins), the CC phase is cheap: each prime checks ~34 backward offsets, ~3% are occupied, yielding ~1 union-find op per prime on average. Total: ~123K union ops with path compression. This is negligible (<1ms).

Fusing primality and CC into one pass saves ~8K cache line reads (the bitmap write-then-read round trip). At ~10ns per miss, that is ~80 microseconds. Not worth the architectural complexity.

### GPU: absolutely keep them separate

Primality is compute-bound (modular exponentiation). CC is memory/atomic-bound (pointer chasing, CAS). Different resource profiles, different optimization strategies. Compaction between them enables CC to operate on 123K vertices instead of 4.06M -- a 33x reduction.

---

## 5. Platform-Agnostic Algorithm Spec

```
TILE_PROCESS(a_lo, b_lo, W, k_sq, collar):
  S = W + 2*collar                        // expanded side

  // Phase 1: Sieve + MR -> prime bitmap
  sieve_primes = precompute_sieve_table(L=110000)
  // sieve_primes contains ~5136 splitting primes (p=1 mod 4) with sqrt(-1) mod p
  // and ~5188 inert primes (p=3 mod 4)

  FOR row = 0 to S-1:
    a = a_lo - collar + row

    // Step 1a: Parity filter
    // Mark all (a,b) with a+b even as composite (both even -> 2|norm, both odd -> 2|norm)

    // Step 1b: Splitting prime sieve
    FOR each splitting prime (p, r) where r^2 = -1 mod p:
      residue = (a mod p) * r mod p
      neg_residue = p - residue
      Mark b = b_lo - collar + (residue - b_start mod p) mod p, stepping by p
      Mark b = b_lo - collar + (neg_residue - b_start mod p) mod p, stepping by p

    // Step 1c: Inert prime sieve
    FOR each inert prime p (p=3 mod 4):
      IF p divides a:
        Mark b = 0, p, 2p, ... (multiples of p in row)

    // Step 1d: MR on survivors
    FOR each unmarked position b in row:
      n = a^2 + b^2
      IF n > 1 AND is_prime_MR(n):  // deterministic MR, 4-12 witnesses by tier
        Record (a, b) as Gaussian prime

  // Phase 1.5: Compact primes
  prime_list = compact(bitmap)             // ~123K entries
  Build spatial bins (cell_size ~ sqrt(k_sq) or similar)

  // Phase 2: Connected components
  Initialize UF over prime_list (123K entries)
  FOR each prime (a, b) in prime_list:
    FOR each offset (da, db) with da^2 + db^2 <= k_sq, backward only:
      IF (a+da, b+db) is in prime_list:    // O(1) via bitmap + bin lookup
        UNION(prime_index, neighbor_index)

  // Phase 3: Extract face-port connectivity
  FOR each component:
    Check which tile faces it touches

  RETURN component_face_lists
```

---

## 6. Operation Count Budget per Tile

| Phase | Operation | Count | Unit cost | Total weight |
|-------|-----------|-------|-----------|-------------|
| Sieve setup | Residue computation (splitting) | S * 5136 = 1.03e7 | 1 mul + 1 mod | 1.03e7 |
| Sieve marking | Bitmap bit-set (splitting) | ~S^2 * ln(ln L) ~ 8.0e6 | 1 atomic OR | 8.0e6 |
| Sieve (inert) | Row check + marking | S * 5188 = 1.04e7 | 1 mod + sparse | 1.04e7 |
| Parity filter | Bitmap bit-set | S^2/2 = 2.03e6 | 1 atomic OR | 2.03e6 |
| **Sieve total** | | | | **~3.1e7** |
| MR | 128-bit Montgomery muls | 231K * 1440 = 3.33e8 | 1 mul128 | **3.33e8** |
| Compaction | Stream compact bitmap | S^2/32 words | scan + scatter | ~1.3e5 |
| Spatial binning | Histogram + scatter | 123K primes | hash + write | ~2.5e5 |
| CC: edge check | Bitmap probe per offset | 123K * 34 = 4.18e6 | 1 read | 4.18e6 |
| CC: union ops | UF union with compression | ~123K | amortized ~3 | 3.7e5 |
| **CC total** | | | | **~4.9e6** |
| **Grand total** | | | | **~3.7e8** |

**MR dominates at 90% of total work.** Sieve is ~8%. CC is ~1.3%.

---

## 7. What Tsuchimura Did Differently

From the original report (METR-2004-13):

Tsuchimura did not use tiles. He processed the entire annular region as a continuous computation, growing the origin-connected component outward. His key insight: you do not need the full graph -- you only need to determine whether the origin component reaches the outer boundary.

His sieve operated on angular sectors of the complex plane, streaming norms in order. This amortizes the prime enumeration cost across much wider regions than our 2000x2000 tiles. For his scale (R ~ 80M, k^2 <= 36), this was efficient because:

1. Norms in a sector form a nearly contiguous range, enabling efficient segmented sieve
2. The angular width was O(1), so Cornacchia-style enumeration becomes competitive
3. He did not need per-tile composition -- no face-port extraction

For our problem (R ~ 1B, tiled architecture), the sector approach does not directly apply because:
- Our tiles have tiny angular width (~W/R ~ 2e-6 radians)
- We need face-port connectivity for tile composition, which requires knowing ALL primes in the tile
- Our parallelism model is tile-independent, not sector-streaming

**Relevant techniques from Tsuchimura**: his norm-streaming approach is the right answer if we ever move to strip/annulus processing instead of tiles. For the current tile architecture, his work confirms that our row-sieve approach is the correct local algorithm.

---

## 8. Literature Approaches We Are Missing (Global, Not Per-Tile)

These only matter if the pipeline architecture changes to process wider regions:

1. **NFS-style lattice/bucket sieving for large primes**: Amortize large-prime sieve hits across many rows by pre-sorting hits into row buckets. Could reduce setup cost for L > 110K, but only if the bucket overhead is less than the MR savings.

2. **Atkin-Bernstein binary quadratic form sieve**: Generates primes p = a^2 + b^2 directly without testing arbitrary lattice points. Requires processing norms in a wide range. Asymptotically optimal but engineering-heavy and only wins at annulus scale.

3. **Bernstein's focused polynomial-value enumeration**: Enumerate values of a^2 + b^2 that are prime, rather than testing all lattice points. Again, wins at sector/annulus scale, not tile scale.

**Bottom line: the tile is the wrong granularity for these techniques. They all need O(R)-width regions to amortize setup.**

---

## 10. Tile Size Optimization & Fat Stripe Architecture

### Optimal Tile Size: W=2000 Confirmed

Per-unit-area cost is nearly flat across tile sizes because MR dominates at 90% of total work. Both sieve marking and MR scale linearly with area. The remaining tensions:

| Factor | Favors small W | Favors large W |
|--------|---------------|----------------|
| UF cache (parent array in L2) | W≤500 (1.3MB) | — |
| Per-tile overhead (kernel launch) | — | W≥2000 (amortize 80ms) |
| GPU occupancy | — | W≥1000 (enough blocks) |
| Composition seams | — | Fewer tiles = less work |
| Memory per tile | W≤2000 (20MB) | — |

**W=2000 is the Pareto optimum.** On 4090 with 96MB L2, W=4000 is viable. On Jetson, W=2000 is the ceiling before memory pressure becomes real.

### Fat Stripe: Architecturally Clean, Modest Speedup

Instead of independent tiles, process a horizontal stripe of height H=W spanning full octant width:

**Phase 1 — Bulk row sieve:** For each row (fixed a), sieve the full width (all b-values across all tiles). Sieve residue setup is done ONCE per (row, prime) pair — amortized across all tiles at that row.

**Phase 2 — Partition + sparse UF:** Partition discovered primes into virtual tiles (with collar overlap). Run sparse UF on ~123K primes per tile, not 4M bitmap positions.

**Quantified savings:**

| Source | Independent tiles | Fat stripe | Savings |
|--------|------------------|------------|---------|
| Sieve setup per area | 5.13 ops/pt | ~0 (amortized) | 5.6% of total |
| Tile launch overhead | 80ms × N_tiles | ~0 (one launch) | 7.9Ks on Jetson |
| UF approach | Dense (4M pts) | Sparse (123K pts) | 32× fewer UF ops |
| Memory per tile | 20MB (bitmap+UF) | 2.5MB (sparse list+UF) | 8× reduction |
| MR cost | identical | identical | 0% (per-candidate, not per-tile) |
| **Total** | baseline | | **~6-8% faster** |

The gain is modest because MR (90% of cost) is per-candidate and unchanged by any tiling strategy.

**Fat stripe streaming model:** Process in column-chunks of C=1000 virtual tiles (~2M lattice units wide). Memory: C × 1MB (sparse UF) + 1.3MB (sieve table) ≈ 1GB. Row-by-row within each chunk.

### Why Fat Stripe Is Still Worth Building

Despite modest speedup, the fat stripe architecture is the correct foundation:
1. **Eliminates per-tile kernel launch overhead** — matters on Jetson (80ms × 550K = 12 hours of pure overhead)
2. **Sparse UF is structurally correct** — 123K primes not 4M bitmap positions
3. **Natural composition pipeline** — primes stream into partition buckets, sparse UF per partition, face ports from UF
4. **Memory-efficient** — 2.5MB per virtual tile vs 20MB, enables larger batch sizes on GPU
5. **The sieve is the sieve** — same algorithm either way, but cleaner separation of concerns

### Platform-Specific W*

| Platform | Independent tiles W* | Fat stripe partition W* | Rationale |
|----------|---------------------|------------------------|-----------|
| Jetson Orin | 2000 | 2000 | Memory + overhead balance |
| RTX 4090 | 4000 | 2000 | Larger L2 enables bigger tiles; fat stripe makes W less critical |
| A100 | 4000 | 2000-4000 | HBM bandwidth favors larger tiles |

---

## 11. Recommendations

**For immediate implementation (current tile architecture):**
- Raise sieve limit from L=10K to L=110K. Expected 13% speedup.
- Ensure sparse UF over compacted primes, not dense UF over all lattice points.
- Keep primality and CC in separate phases.

**For future architecture consideration:**
- **Adopt the fat stripe model** for the UB campaign. Process column-chunks of 1000 virtual tiles, row-by-row sieve across the chunk width, sparse UF per partition. This is the natural streaming architecture for composition.
- If tiles become the bottleneck, consider processing strips (multiple tile-rows as one sieve pass) to amortize sieve setup.
- At R >> 1B, the MR cost grows (more witnesses needed for larger norms) while sieve cost grows sublinearly, so L* shifts upward. Re-derive for each target radius.

**What NOT to do:**
- Do not attempt Cornacchia/inverse enumeration for individual tiles. The waste ratio is ~785,000x.
- Do not attempt full sieve to sqrt(n). The setup cost is ~570x the optimal hybrid.
- Do not attempt norm-space segmented sieve. The bitmap for one row's norm range is ~250 GB.
- Do not fuse primality and CC. The cache savings (~80us) do not justify the complexity.

---

## 12. Fat Stripe — Full Mechanical Detail

### 12.1 Geometry

At R~1.05B, first-octant, the annular strip for the UB proof:
- Height: H = W = 2000 lattice units (one tile-height)
- Width: b in [0, a] where a ~ 1.05B. Full first-octant b-range.
- Virtual tiles: ceil(1.05B / 2000) = **525,000**
- Expanded tile: (W + 2c) x (W + 2c) = 2014 x 2014. Collar c = 7 = ceil(sqrt(40)).
- Column-chunk: C consecutive virtual tiles. C = 1000 -> chunk width = 2,000,000 lattice units.
- Chunks: ceil(525,000 / 1000) = **525 chunks**.

### 12.2 Column-Chunk Processing Loop

```
// Pre-compute sieve table (once, ~60KB)
sieve_table = precompute(L=110000)  // 5136 splitting, 5188 inert

FOR chunk_id = 0 to 524:
  b_chunk_lo = chunk_id * C * W
  b_chunk_hi = min((chunk_id + 1) * C * W + 2*collar, max_b)

  // ---- Phase 1: GPU sieve+MR (all tiles in chunk) ----
  // Launch grid: C * S blocks (one block per tile-row)
  // Each block: 256 threads, 252 bytes shared mem (tile-width sieve)
  // Block (tile_t, row_r): sieve row r of virtual tile t, MR survivors
  // Output: per-tile bitmap in device global memory (495KB per tile, reused)
  // OR: single chunk bitmap (504MB in device memory)
  gpu_launch_sieve_mr(sieve_table, chunk geometry)

  // ---- Phase 1.5: D2H transfer ----
  // Bitmap: 504MB per chunk (PCIe: 20ms, unified: 0ms)
  // OR: per-tile sequential: 495KB * 1000 = 495MB (same order)
  d2h_copy(chunk_bitmap)

  // ---- Phase 2: CPU sparse UF (parallel over tiles) ----
  // Rayon par_iter over C=1000 tiles
  FOR EACH virtual tile t IN PARALLEL:
    // Extract primes from bitmap slice (rank-based compaction)
    prime_list_t = compact_from_bitmap(chunk_bitmap, tile_t_bounds)
    // Build rank table for O(1) index lookup
    rank_table_t = build_rank(chunk_bitmap, tile_t_bounds)
    // Sparse UF over ~123K primes
    uf_t = sparse_union_find(prime_list_t, rank_table_t, offsets_34)
    // Extract face-port component labels
    face_ports_t = extract_faces(uf_t, prime_list_t, tile_t_bounds, collar)

  // ---- Phase 3: Horizontal composition within chunk ----
  FOR t = 1 to C-1:
    merge_face_ports(face_ports[t-1].right, face_ports[t].left)

  // Save boundary ports for inter-chunk merging
  save_boundary(chunk_id, face_ports[0].left, face_ports[C-1].right)

// ---- Phase 4: Inter-chunk composition ----
FOR chunk_id = 1 to 524:
  merge_face_ports(boundary[chunk_id-1].right, boundary[chunk_id].left)

// ---- Phase 5: Check inner-to-outer reachability ----
blocked = !any_component_touches(INNER and OUTER faces)
```

### 12.3 Memory Layout

**Per-row sieve buffer (GPU shared memory):** 252 bytes per block. Tile-width bitmap (2014 bits / 8 = 252 bytes). This is the current kernel's shared memory layout, unchanged.

**Why not fat-row shared memory:** A chunk-width row bitmap (2,000,014 bits = 244KB) exceeds Ampere's 48KB shared memory limit. Fat-row sieve in shared memory is impossible. Instead, we keep tile-width blocks and amortize across the launch grid.

**Device bitmap:** Two options based on GPU memory:
- Jetson (8GB unified): sequential tile launches, one 495KB bitmap reused. Total GPU memory: <1MB.
- Desktop/server GPU (24-80GB): single chunk bitmap = 504MB in device global memory. One kernel launch per chunk.

**CPU-side per tile (Phase 2):** 1.1MB working set per tile:
- Bitmap slice: 495KB (read-only, shared with other tiles in overlapping collar region)
- Rank table: 31KB (precomputed prefix-sum for O(1) prime-index lookup)
- UF parent array: 478KB (122K entries x 4 bytes)
- UF rank array: 119KB (122K entries x 1 byte)
- Face-port buffer: ~7KB (4 faces x ~420 primes x 4 bytes)

**Peak CPU memory:** Chunk bitmap (504MB) + C parallel tile workspaces. At 6 Jetson cores: 504MB + 6 x 1.1MB = 511MB. At 64 server cores: 504MB + 64 x 1.1MB = 574MB.

### 12.4 Sieve Buffer: Option B (Per-Row) Is Correct

**Option A (full chunk bitmap: 503MB)** — rejected. Forces allocation of H x chunk_width / 8 = 504MB contiguous buffer for the sieve alone, separate from the prime output bitmap. Doubles memory.

**Option B (per-row: 244KB per row)** — correct for CPU-side sieve, but irrelevant for GPU. The GPU kernel already uses per-block shared memory (252 bytes = one tile-row). The "fat stripe" does not change the per-block sieve — it changes the launch grid.

**Option C (multi-row: 15 rows x 244KB = 3.6MB)** — unnecessary. UF does not run concurrently with sieve. Primes from each row are accumulated into the output bitmap as they're found.

**Decision:** GPU uses per-block shared memory sieve (252 bytes, unchanged). Output goes to device global memory bitmap. CPU reads the bitmap after D2H. No new sieve buffer design needed.

### 12.5 Prime Partitioning Into Virtual Tiles

Primes are discovered on GPU and stored in a bitmap. CPU partitions them into virtual tiles after D2H:

**Method:** tile_id = (b - b_chunk_lo) / W. O(1) per prime. During bitmap scan (row-major), for each set bit at (row, col), compute b = b_chunk_lo - collar + col, then tile_id = (b - b_chunk_lo + collar) / W. Primes in the collar region (within 7 columns of a tile boundary) are assigned to BOTH adjacent tiles.

**No pre-allocation needed.** The bitmap scan is single-pass. Each tile's prime list is populated by scanning the tile's bitmap slice directly. Since UF processes tiles in parallel (one per core), each core scans its own bitmap slice independently.

### 12.6 Sparse UF: Bitmap + Rank Table

**Data structure:** The GPU output bitmap doubles as the spatial index. For each prime p at (a, b), neighbor existence at (a+da, b+db) is a single bitmap bit-test: O(1). The neighbor's prime-list index is obtained via rank query on the bitmap: popcount of all bits before position (a+da, b+db).

**Rank table construction:** Partition bitmap into 512-bit blocks. Store cumulative popcount per block (4 bytes each). For 2014x2014 = 4,056,196 bits: 7,923 blocks, 31KB. Rank query: one table lookup + popcount of partial word = ~5 cycles.

**UF loop per tile:**
1. Scan bitmap slice to build prime_list (122K entries) and rank table (31KB). Cost: ~9M cycles.
2. For each prime (122K iterations): for each of 34 backward offsets: bitmap_test (2 cycles). On hit (~3% density): rank_query (5 cycles) + UF union (10 cycles). Cost: 29M + 4M = 33M cycles.
3. Total: **42M cycles per tile.** At 2.2 GHz (Jetson): 19ms. At 5 GHz (desktop): 8ms.

**vs Dense UF (current code):** parent[4,056,196] = 16MB, iterates all 4M positions checking 34 offsets each. Cost: ~1,250ms/tile on Jetson. **Sparse UF is 32x faster and uses 70x less memory.**

---

## 13. CUDA Pipeline for Fat Stripe

### 13.1 Kernel Architecture: Keep Tile-Sized Blocks (Option 2)

The current kernel launches one block per row of one tile (S = 2014 blocks per tile, 256 threads/block, 252 bytes shared memory). For the fat stripe, the grid scales to C x S blocks:

```
Grid:  C * S = 1000 * 2014 = 2,014,000 blocks
Block: 256 threads, 252 bytes shared memory
```

Each block is **identical** to the current `tile_sieved_primality_bitmap_kernel`. The only change is the grid dimensions and the output bitmap address calculation (offset by tile_id within chunk).

**Why not fat-row blocks:** A chunk-width row needs 244KB shared memory. Ampere shared memory limit is 48KB. The row does not fit. Splitting a fat row across multiple blocks would require global-memory sieve synchronization between blocks — slower and more complex than independent tile blocks.

**Why not one-block-per-row-across-chunk:** Same shared memory problem. The sieve bitmap must be in shared memory for atomicOr performance.

### 13.2 GPU Memory Layout

**Device bitmap:** Two strategies:

| Strategy | GPU memory | Kernel launches | D2H transfer |
|----------|-----------|-----------------|--------------|
| Sequential tiles | 495KB (reused) | 525K launches | 495KB x 525K |
| Chunk bitmap | 504MB | 525 launches | 504MB x 525 |

**Sequential tiles:** Current architecture scaled. Reuse one 495KB bitmap. Launch 2014 blocks per tile, 525K times. Total launches: 525K. Kernel launch overhead: ~10us each = 5.25s total. Works on Jetson.

**Chunk bitmap:** Allocate 504MB device memory. Launch 2,014,000 blocks per chunk, 525 times. Each block writes to its tile's slice of the chunk bitmap. Requires 504MB GPU memory. Does not fit on Jetson (8GB shared with system, ~4GB available). Fits on RTX 3090+ (24GB+).

**Recommendation:** Sequential tiles for Jetson. Chunk bitmap for desktop/server GPUs. The kernel code is identical — only the bitmap pointer offset and grid dimensions differ.

### 13.3 GPU to CPU Data Flow

**Bitmap D2H is more compact than coordinate list.** Prime density is ~3%. Bitmap: 504MB per chunk. Compact coordinate list: 122M primes x 6 bytes = 733MB. Bitmap wins.

**Transfer timing:**

| Platform | Method | Transfer size | Bandwidth | Time |
|----------|--------|--------------|-----------|------|
| Jetson | Unified memory | 0 (no copy) | N/A | 0ms |
| RTX 3090 | PCIe 4.0 x16 | 504MB | 25 GB/s | 20ms |
| RTX 4090 | PCIe 4.0 x16 | 504MB | 25 GB/s | 20ms |
| A100 (PCIe) | PCIe 4.0 x16 | 504MB | 25 GB/s | 20ms |
| A100 (SXM) | NVLink | 504MB | 300 GB/s | 2ms |

For sequential-tile mode: 495KB per tile at 25 GB/s = 0.02ms. Negligible.

### 13.4 Double-Buffering Pipeline

GPU chunk N+1 overlaps with CPU UF of chunk N:

```
Time -->
GPU:  [=== chunk 0 ===][=== chunk 1 ===][=== chunk 2 ===] ...
D2H:              [D2H 0]          [D2H 1]          [D2H 2]
CPU:                    [=== UF 0 ===][=== UF 1 ===][=== UF 2 ===]
```

**Chunk pipeline time = max(GPU + D2H, CPU UF) + composition.**

On Jetson (GPU-bound): GPU=17s >> CPU UF=3.2s. Double-buffering hides CPU entirely. Chunk time = 17s.

On desktop (mixed): GPU=1s, CPU UF=0.5s. GPU-bound. Chunk time = 1s.

Double-buffering requires two bitmap buffers on GPU (sequential-tile mode: 2 x 495KB = trivial; chunk-bitmap mode: 2 x 504MB = 1GB).

---

## 14. Concrete Time Estimates

### 14.1 Per-Tile Timing (L=110K, sparse UF)

| Phase | Jetson Orin | RTX 3090 | RTX 4090 | A100 |
|-------|-------------|----------|----------|------|
| GPU sieve+MR | 17 ms | 1.0 ms | 0.3 ms | 0.6 ms |
| D2H bitmap (495KB) | 0 ms | 0.02 ms | 0.02 ms | 0.02 ms |
| CPU sparse UF (1 core) | 19 ms | 8 ms | 8 ms | 14 ms |
| CPU composition | 0.1 ms | 0.05 ms | 0.05 ms | 0.05 ms |
| **Serial total** | **36 ms** | **9 ms** | **8 ms** | **15 ms** |

CPU cores for parallel UF: Jetson 6, desktop 16, server 64.

| Phase | Jetson (6c) | RTX 3090 (16c) | RTX 4090 (16c) | A100 (64c) |
|-------|-------------|----------------|----------------|------------|
| CPU sparse UF (parallel) | 3.2 ms | 0.5 ms | 0.5 ms | 0.2 ms |
| **Pipelined total** | **17 ms** | **1.1 ms** | **0.6 ms** | **0.6 ms** |
| **Bottleneck** | GPU | GPU | CPU UF | GPU |

### 14.2 Full Campaign (525K tiles = 525 chunks)

| Metric | Jetson | RTX 3090 | RTX 4090 | A100 | 8xA100 |
|--------|--------|----------|----------|------|--------|
| Serial | 5.3 h | 1.3 h | 1.2 h | 2.2 h | 0.3 h |
| Pipelined + parallel UF | **2.5 h** | **0.2 h** | **0.1 h** | **0.1 h** | **< 1 min** |

### 14.3 Optimization Impact (Jetson, per-tile, cumulative)

| Configuration | ms/tile | Campaign | vs baseline |
|--------------|---------|----------|-------------|
| Baseline (L=10K, dense UF, serial) | 1,275 | 186 h | — |
| +L=110K sieve | 1,271 | 185 h | -0.3% |
| +Sparse UF (bitmap+rank) | 36 | 5.3 h | **-97%** |
| +Double-buffer (GPU‖CPU) | 19 | 2.8 h | -98.5% |
| +Rayon parallel UF (6 cores) | 17 | **2.5 h** | **-98.7%** |

**The single most impactful optimization is sparse UF: 32x speedup on the CPU phase, which is currently the bottleneck at 1,254ms/tile (98% of wall time).** Sieve limit increase helps only 0.3% because MR (the GPU-bound 90% of compute) is unchanged. After sparse UF, the GPU becomes the bottleneck and further CPU optimizations have diminishing returns.

### 14.4 Model Assumptions

- GPU INT64 multiply throughput: 16 per SM per cycle (Ampere/Ada, emulated via 4x MAD32). A100: 32/SM/cycle (native INT64).
- GPU utilization: 30% of peak (warp divergence in MR early-exit, memory stalls during bitmap writes).
- MR cost: 12 Montgomery witnesses, ~90 mont_mul each, 4 INT64-muls per mont_mul = ~4,320 INT64-muls per MR call. Plus Montgomery init overhead = ~5,760 total INT64-muls per candidate.
- Sieve survivor rate at L=110K: 11.4% of parity-valid points. Trial division in `is_prime()` filters ~50% of survivors before Montgomery path.
- Sparse UF: 122K primes x 34 backward offsets x 7 cycles/probe + 3% hit rate x 15 cycles/hit = 42M cycles/tile.
- CPU clocks: Jetson A78AE 2.2 GHz, desktop 5.0 GHz, server Xeon 3.0 GHz.
