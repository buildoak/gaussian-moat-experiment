## Worker 4A: M4 Dense-Remap and Group-Flag Accumulation

Extend K4 after compression with geo staging, dense-remap, and group-flag accumulation.

### Files you own
- `src/kernel_uf_v2.cu` — add Phase B.5 geo staging, Phase C dense-remap, Phase D group-flag accumulation
- `include/cuda_campaign/uf_buffers.cuh` — extend with new output buffers
- `tests/test_dense_remap_adversarial.cpp` — adversarial remap test

### What to implement

1. **Phase B.5: Geo flag staging**
   - Read geo bits from `i128_sq_leq.cuh` helper (already landed in Wave 3)
   - Store per-prime `is_inner`/`is_outer` bits in `d_prime_geo_bits[]`

2. **Phase C: Serial dense-remap (thread-0 only)**
   - Scan primes in ascending index order
   - Assign wire labels: `wire_label_by_raw_root[root] = next_label++` on first encounter
   - CRITICAL: Do NOT parallelize — must preserve CPU first-appearance semantics exactly
   - Output: `d_wire_label_by_raw_root[]`, `d_max_label`, `d_overflow` (set if max_label > 255)

3. **Phase D: Parallel group-flag accumulation**
   - For each prime, look up its wire label via `wire_label_by_raw_root[parent[i]]`
   - Accumulate `inner_flag` and `outer_flag` per group
   - Output: `d_group_flags[]` (2 bits per group: inner|outer)

### CPU reference (bit-for-bit match required)
- `cpp-campaign-v2/src/tileop.cpp` — `build_local_dsu()` and remap logic
- `cpp-campaign-v2/src/tileop_internal.h` — data structures
- `cpp-campaign-v2/include/campaign/union_find.h` — DSU reference

### Existing pieces
- K4 Phase A (union) + Phase B (compression) landed in Wave 2 (commit b6116a2)
- TODO marker at line 140 in `kernel_uf_v2.cu` marks insertion point
- `i128_sq_leq.cuh` geo helper landed in Wave 3 (commit 6728e01)

### New buffer layout in `uf_buffers.cuh`
```cpp
// M4 outputs — add to UfBuffersOut
uint8_t* d_prime_geo_bits;        // [num_tiles * MAX_PRIMES_GPU], 2 bits used per byte
uint16_t* d_wire_label_by_raw_root; // [num_tiles * MAX_PRIMES_GPU], label for each root
uint16_t* d_max_label;            // [num_tiles], max wire label assigned
uint8_t* d_overflow;              // [num_tiles], 1 if overflow
uint8_t* d_group_flags;           // [num_tiles * 256], 2 bits per group (inner|outer)
```

### Verification gate
- 100 test tiles at R=1000
- `{parent[], wire_label_by_raw_root[], max_label, overflow, prime_geo_bits[], group_flags[]}` matches CPU bit-for-bit
- Adversarial test: inverted-root-order remap (roots appear in descending prime index) still matches CPU

### Do NOT touch
- K1/K2/K3 kernel files
- K5 skeleton files
- Host driver wiring (Wave 5 owns that)

### Deliverable
Commit with M4 dense-remap and group-flag accumulation. Build must succeed with CUDA enabled. Test harness must compile (even if it can't run locally without GPU).
