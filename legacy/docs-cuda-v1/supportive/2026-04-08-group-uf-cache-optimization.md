---
title: "Group Union-Find Cache Optimization"
date: 2026-04-08
engine: gemini
status: complete
---

# Cache-Optimal Group Union-Find for Gaussian Moat Compositor

## Hardware & Architecture Assumptions
- **Target:** Modern x86-64 (e.g., AMD EPYC Genoa / Intel Sapphire Rapids or equivalent consumer chips like Ryzen 9 7950X).
- **Cache Topology:** 64-byte cache lines. L1D: 32KB (4-5 cycles). L2: 1MB+ (~14 cycles). L3: 32MB+ (~40+ cycles).
- **RAM Bandwidth:** ~100 GB/s per channel (single socket).
- **Grid Size:** N = 73.4M tiles (1 band at R=830M).

---

## 1. Memory Layout: The Zero-Allocation Embedded UF

**Recommendation:** strictly adopt **Option B** (embedded in TileOp spare bytes) from the compositor spec, combined with **Tower-Major Array Ordering**.

By storing the UF parent pointers in bytes 96-127 of the `TileOp`, we achieve the holy grail of graph traversals: **zero-allocation structural colocation**.
- `TileOp` size remains exactly 128 bytes (2 cache lines).
- `groups` and `I/O h1` are in bytes 0-95. The UF array is in bytes 96-127.
- When we process a tile for L/R matching, pulling its `h1` array (bytes 64-95) automatically prefetches the UF parent array (bytes 96-127) into L1, because they reside in the same 64-byte cache line (cache line 1).
- Total memory footprint is locked at **9.38 GB**. A 24 GB GPU or standard RAM can trivially swallow this without thrashing the TLB.

**OS/Memory Tricks:** Allocate the 9.38 GB array using `mmap` with `MAP_HUGETLB` (2MB or 1GB hugepages) and `MADV_SEQUENTIAL`. This virtually eliminates TLB misses during the sequential tower-by-tower sweep.

---

## 2. UF Representation: Link-by-ID & Path Halving

We drop `rank` and `weight` entirely. Maintaining a separate rank array wastes cache footprint and DRAM bandwidth for microscopic tree depths.

Instead, we use **Link-by-ID** (Deterministic Linking) combined with **Path Halving**:
```rust
let root_a = find(id_a);
let root_b = find(id_b);
if root_a != root_b {
    // Lower ID becomes parent. This naturally directs trees towards Tower 0, Row 0.
    if root_a < root_b { parent[root_b] = root_a; }
    else               { parent[root_a] = root_b; }
}
```

**Why Link-by-ID is optimal here:**
`global_id` is defined as `(tower_j * 256) + (r * 8) + group`.
Because matching proceeds tower-by-tower (`j=0, 1, 2...`), cross-tower links *always* point backwards (from `j+1` to `j`). This completely prevents cycle formation and bounds tree depth organically without storing rank. Trees naturally flow toward the inner boundary.

**Branch-Free Path Halving:**
Standard path compression requires a second loop to update parents, causing pipeline stalls. Path halving compresses the tree *during* the traversal in a single pass:
```c
// Branch-free, single-pass path halving
uint32_t find(uint32_t i, uint32_t* uf_array) {
    while (true) {
        uint32_t p = uf_array[i];
        if (p == i) return i;
        uint32_t pp = uf_array[p];
        uf_array[i] = pp; // Halve path
        i = pp;
    }
}
```

---

## 3. Exploiting Locality: Hierarchical Pre-Flattening

Because I/O matching is strictly *intra-tower* and L/R matching is *inter-tower*, we can exploit the hierarchy to guarantee L1 hits.

1. **Intra-Tower Phase (I/O):** We load tower `j` (4 KB, fits completely in L1). We do all I/O matching between its 32 tiles.
2. **Pre-Flattening:** Before we expose tower `j` to L/R matching with tower `j+1`, we iterate over its 256 groups and flatten them completely: `uf_array[id] = find(id)`.
3. **Inter-Tower Phase (L/R):** When tower `j+1` matches against tower `j`, `find()` calls on tower `j`'s elements are **guaranteed O(1)** (they are already roots or point directly to roots in `j-1`).

By fusing I/O matching and Pre-Flattening into the pipeline, we ensure that the deep pointer chasing only happens on flat data that is hot in L1/L2.

---

## 4. Vectorization and Hot Inner Loops

### I/O Matching Loop (Intra-Tower)
Positional alignment means `O_groups[s]` matches `I_groups[s]`. Because active ports pack at the front and average ~3-5 per face, a scalar loop with a zero-sentinel bailout beats SIMD setup overhead.

```rust
// Intra-tower matching (guaranteed L1 hit)
for s in 0..16 {
    let g_a = tile_ops[a].O_groups[s];
    if g_a == 0 { break; } // Highly predictable branch
    let g_b = tile_ops[b].I_groups[s];
    
    let id_a = a as u32 * 8 + (g_a as u32 - 1);
    let id_b = b as u32 * 8 + (g_b as u32 - 1);
    
    // Inline Link-by-ID (both IDs are within the same tower)
    let root_a = find_local(id_a, uf_array);
    let root_b = find_local(id_b, uf_array);
    if root_a < root_b { uf_array[root_b] = root_a; }
    else if root_b < root_a { uf_array[root_a] = root_b; }
}
```

### L/R Matching Loop (Inter-Tower Integer Trick)
Instead of converting `u8` to `i16` for signed delta math, we keep everything in `u16` to avoid vector overhead and branching.

Recall from the spec:
- Primary neighbor (`delta_h = -f`): `a.h1 + f == b.h1`
- Secondary neighbor (`delta_h = S - f`): `a.h1 + f == b.h1 + 256`

We compute `a_h1_mod = a.h1 as u16 + f as u16`.
If `a_h1_mod < 256`, it matches the primary neighbor's `b.h1`.
If `a_h1_mod >= 256`, it matches the secondary neighbor's `b.h1 + 256`.
This enables a unified, branch-free arithmetic check:

```c
// Hot loop for L/R matching (C syntax for raw memory access)
uint16_t f_u16 = (uint16_t)f;
for (int sa = 0; sa < 16; ++sa) {
    uint8_t g_a = a_face_R_groups[sa];
    if (!g_a) break;
    
    uint16_t a_mod = (uint16_t)a_face_R_h1[sa] + f_u16;
    
    // Choose which neighbor we belong to based on 256 threshold
    bool is_secondary = a_mod >= 256;
    uint8_t target_b_h1 = (uint8_t)(a_mod & 0xFF);
    
    uint8_t* b_groups = is_secondary ? b_sec_L_groups : b_pri_L_groups;
    uint8_t* b_h1     = is_secondary ? b_sec_L_h1     : b_pri_L_h1;
    uint32_t b_tile_idx = is_secondary ? b_sec_idx    : b_pri_idx;
    
    if (!b_groups) continue; // Boundary condition

    for (int sb = 0; sb < 16; ++sb) {
        uint8_t g_b = b_groups[sb];
        if (!g_b) break;
        
        if (b_h1[sb] == target_b_h1) {
            uint32_t id_a = a_idx * 8 + (g_a - 1);
            uint32_t id_b = b_tile_idx * 8 + (g_b - 1);
            
            uint32_t root_a = find(id_a, uf_array);
            uint32_t root_b = find(id_b, uf_array);
            if (root_a < root_b) uf_array[root_b] = root_a;
            else if (root_b < root_a) uf_array[root_a] = root_b;
            break; // Matched, move to next sa
        }
    }
}
```

---

## 5. Performance & Cycle Analysis

### Memory Bandwidth
The monolithic 9.38 GB array is streamed sequentially. At 100 GB/s (standard desktop DDR5), reading the array takes **~94 ms**. With one tower loaded at a time, cache evictions handle the write-back transparently. Memory bandwidth is *not* the bottleneck.

### Cycle Estimates (Per Tile)
- **I/O Phase:** ~16 loop iterations total per tile pair. Early bailout exits after 3-5 iterations. Because data is hot in L1 and predictable, ~10 cycles per tile.
- **Pre-flattening:** 8 groups per tile. `find()` is O(1). ~8 cycles per tile.
- **L/R Phase:** Outer loop runs 3-5 times, inner loop runs 3-5 times. Branch predictor perfectly handles `is_secondary` and the bailout. Total ~20-30 cycles per tile.
- **Total:** ~50 CPU cycles per tile.

### Wall-Clock Estimate
For N = 73.4M tiles:
- `73,400,000 * 50 cycles = 3.67 billion cycles`.
- On a 4.5 GHz core: **~0.81 seconds total execution time.**

This represents an **11x - 17x speedup** over the 9.3s - 14s estimates from the specifications. 
Parallelization (stripe partitioning) is completely unnecessary at this speed. A single threaded sweep using the integer-trick + path-halving will saturate the execution ports and finish the band in sub-second time.
