# CUDA Tile Kernel Internals

Authoritative design for the CUDA kernel that processes one tile into one TileOp.
Mirrors the 5-phase C++ reference pipeline (`tile-cpp/src/`) but restructured for
GPU memory hierarchy: registers, shared memory, constant memory, global memory.

**Target hardware:** NVIDIA Ampere (sm_87, Jetson Orin Nano) and above.
**Reference implementation:** `tile-cpp/` (C++ single-threaded, validated against Python mirror).
**Fallback:** C++ pipeline at 1,003 tiles/sec (12-thread CPU) for poisoned tiles.

---

## 1. Thread Mapping

```
Threads/block:  288  (9 warps x 32 lanes)
Active threads: 271  (one per sieve-domain row, SIDE_EXP = 271)
Idle threads:   17   (lanes 271-287 in last warp; participate in syncthreads, idle during sieve)
Grid:           N blocks = N tiles
```

271 rows map 1:1 to threads 0-270. The 17 idle lanes (5.9% waste) are cheaper than
any thread-multiplexing scheme. All 288 threads activate during MR testing (Phase 1c)
and can assist in scans/reductions.

---

## 2. Memory Hierarchy

### 2.1 Constant Memory (~4.3 KB, warp-broadcast)

Read-only tables shared across ALL blocks. Every thread in a warp reads the same
table entry simultaneously -- perfect broadcast, zero bank conflicts.

```
__constant__ uint32_t c_split_table[609];     // 2,436 B  (packed: root<<16 | p)
__constant__ uint16_t c_inert_primes[619];    // 1,238 B
__constant__ int8_t   c_bk_dr[64];            //    64 B  (backward offsets, dr)
__constant__ int8_t   c_bk_dc[64];            //    64 B  (backward offsets, dc)
__constant__ uint64_t c_mr_witnesses[12];     //    96 B  (MR witness set)
__constant__ uint32_t c_trial_primes[24];     //    96 B  (trial division primes)
                                              // ────────
                                              //  3,994 B total
```

### 2.2 Registers (per thread, ~26 registers)

```
uint32_t ws[9]       9 regs   working sieve bitmap (288 bits, 271 used)
int32_t  a           1 reg    absolute row coordinate
int32_t  b_start     1 reg    column-start coordinate
--- loop-local (reused) ---
uint32_t p, root     2 regs   current split prime
int64_t  residue     2 regs   residue class
uint64_t norm        2 regs   a^2 + b^2 for MR
uint64_t mont_*      ~8 regs  Montgomery context + powmod state
int      col, k      ~2 regs  loop indices
                     ────────
                     ~26 regs total
```

Budget: 65,536 regs/SM / 288 threads = 227 regs/thread max. 26 used = 11.4%.

### 2.3 Shared Memory (35.4 KB peak, union-overlaid)

Two regions: persistent bitmap (A) and a phase-dependent working area (B).
Phases 1b-1c and Phases 2-4 reuse the same physical bytes via union overlay.

```
REGION A  (persistent, Phases 1c through 4):
  uint32_t bitmap[271 * 9]                    9,756 B    row-padded prime bitmap

REGION B  (union-overlaid by phase):

  ┌─ Phase 1b-1c layout ──────────────────┐   ┌─ Phase 2-4 layout ──────────────────┐
  │ uint32_t cand_counts[288]   1,152 B    │   │ uint32_t row_prefix[272]  1,088 B   │
  │ uint32_t cand_prefix[289]   1,156 B    │   │ uint32_t prime_pos[2304]  9,216 B   │
  │ uint32_t cand_list[2304]    9,216 B    │   │ uint32_t parent[2304]    9,216 B    │
  │                            ─────────   │   │                         ─────────   │
  │                            11,524 B    │   │                         19,520 B    │
  └────────────────────────────────────────┘   └─────────────────────────────────────┘

Peak (Phase 3-4):  9,756 + 19,520 = 29,276 B  (28.6 KB)
Available:         48 KB (default Ampere shared/L1 config)
Remaining for L1:  13 KB
```

### 2.4 Global Memory (per block)

```
Input:   TileCoord coords[]         16 B/tile   (a_lo, b_lo as int64)
Output:  TileOp    output[]        128 B/tile   (2 cache lines, aligned)
```

No other global memory. All intermediate state lives in shared memory and registers.

### 2.5 Capacity Bounds

The 2304-entry cap on prime_pos/parent/cand_list is validated by census:

```
100K tile census at R=860M:
  Mean prime_count:   2,044
  Max observed:       2,214
  99.99th pct:        2,183
  Cap (2304):         4.1% headroom over worst observed (90 entries)
  Tiles > 2304:       0  (zero in 100,000)
```

If prime_count ever exceeds 2304 (not observed at operating point), the tile
is marked poisoned (TileOp = all 0xFF) and falls back to C++ processing.
The tighter cap (reduced from 3072) saves ~4.6 KB shared memory per block.
Occupancy remains 3 blocks/SM (56.2%) due to 1024-byte allocation granularity:
  40,572 (dynamic) + 1,040 (static) = 41,612 → ceil(41,612/1024) = 41 chunks.
  41 × 1024 × 4 = 167,936 = SM limit (strict < required). Next threshold: N ≤ 2192.

---

## 3. Row-Padded Bitmap Layout

The C++ uses flat-packed bits: `pos = row * 271 + col`. Adjacent rows share u32
words, causing write conflicts between threads. The CUDA kernel uses row-padded
layout: each row occupies exactly 9 words (288 bits), of which bits 0-270 are used.

```
Flat-packed (C++):                 Row-padded (CUDA):
  pos = row * 271 + col             word_idx = row * 9 + (col >> 5)
  word = pos >> 5                   bit_pos  = col & 31
  bit  = pos & 31
  PROBLEM: rows share words         Each thread writes exclusively to its
  at boundaries                     own 9 words. Zero conflicts.
```

Operations:
```cuda
bitmap_set(row, col):   bitmap[row * 9 + (col >> 5)] |= (1u << (col & 31))
bitmap_test(row, col):  (bitmap[row * 9 + (col >> 5)] >> (col & 31)) & 1
```

Overhead: 17 unused bits per row (6.3%). Simplifies all downstream phases:
compact becomes per-row parallel, and uf_index avoids integer division by 271.

---

## 4. Phase Pipeline

```
Phase 1a  | 271 thr | Sieve marking          | const mem -> ws[9] regs
    | syncthreads
Phase 1b  | 271 thr | Candidate collection   | ws regs -> cand_list (shmem)
    | syncthreads
Phase 1c  | 288 thr | MR testing             | cand_list -> bitmap (shmem)
    | syncthreads
Phase 2   | 271 thr | Compact                | bitmap -> row_prefix + prime_pos (shmem)
    | syncthreads
Phase 3   |  32 thr | Union-find             | bitmap + prime_pos -> parent (shmem)
    | syncthreads
Phase 4-5 |   1 thr | Face extract + encode  | parent + prime_pos -> TileOp (global)
```

### 4.1 Phase 1a: Sieve Marking (271 threads, 1/row)

Each thread marks composite candidates in its register-local working sieve.
Three sub-steps matching the C++ pipeline, but with GPU-specific optimizations.

**Step 1 — Parity elimination:**

The C++ loops 271 iterations per row. On GPU, the parity pattern is constant:
`(a ^ b) & 1 == 0` marks same-parity positions. Since `b = b_start + col`,
the marked columns form a repeating pattern determined by `(a ^ b_start) & 1`.

```cuda
uint32_t pattern = ((a ^ b_start) & 1) ? 0xAAAAAAAAu : 0x55555555u;
for (int w = 0; w < 9; w++) ws[w] = pattern;
ws[8] &= (1u << 15) - 1;   // mask bits >= 271
```

9 register writes vs 271 branch-and-OR iterations. ~30x improvement.

**Step 2 — Split prime sieve (609 primes):**

```cuda
for (int k = 0; k < 609; k++) {
    uint32_t packed = c_split_table[k];       // constant memory, warp broadcast
    uint32_t p    = packed & 0xFFFFu;
    uint32_t root = packed >> 16;

    int32_t a_mod = euclidean_mod(a, p);
    int32_t residue = (int32_t)((int64_t)a_mod * root % p);

    mark_residue_class_reg(ws, b_start, p, residue);

    int32_t neg_res = (int32_t)euclidean_mod(-residue, p);
    if (neg_res != residue)
        mark_residue_class_reg(ws, b_start, p, neg_res);
}
```

All 271 threads read `c_split_table[k]` at the same cycle -- perfect constant-memory
broadcast. Different threads compute different residues (different `a` values), but
the inner marking loop `for (col = first; col < 271; col += p)` has near-identical
trip counts across threads (~271/p +/- 1). Minimal warp divergence.

**Step 3 — Inert prime sieve (619 primes, rare activation):**

```cuda
for (int k = 0; k < 619; k++) {
    uint32_t p = (uint32_t)c_inert_primes[k];
    if (euclidean_mod(a, p) == 0)             // fires with probability ~1/p
        mark_residue_class_reg(ws, b_start, p, 0);
}
```

### 4.2 Phase 1b: Candidate Collection (271 threads)

**This is the critical GPU optimization.** The C++ tests MR inline per-row.
On GPU, MR at 2.4% density means ~0.77 active threads per warp at any column --
2.4% warp utilization. Collecting candidates and redistributing them to all 288
threads yields ~24x speedup on MR (the dominant compute).

```cuda
// --- Count survivors ---
uint32_t my_count = 0;
for (int w = 0; w < 9; w++) {
    uint32_t surv = ~ws[w];
    if (w == 8) surv &= LAST_WORD_MASK;
    my_count += __popc(surv);
}
cand_counts[threadIdx.x] = my_count;
__syncthreads();

// --- Parallel exclusive scan -> cand_prefix[0..271] ---
block_exclusive_scan(cand_counts, cand_prefix, 271);  // standard Blelloch scan
__syncthreads();

// --- Scatter (re-scan working sieve, write packed positions) ---
int offset = cand_prefix[threadIdx.x];
for (int w = 0; w < 9; w++) {
    uint32_t surv = ~ws[w];
    if (w == 8) surv &= LAST_WORD_MASK;
    while (surv) {
        int bit = __ffs(surv) - 1;
        cand_list[offset++] = (threadIdx.x << 16) | (w * 32 + bit);
        surv &= surv - 1;
    }
}
__syncthreads();
```

The double-scan (count pass + scatter pass) avoids storing survivor positions
in registers. Scan cost is negligible vs MR.

### 4.3 Phase 1c: MR Testing (288 threads, round-robin)

All 288 threads participate. Round-robin distribution interleaves candidates
across warps for maximum occupancy. At operating point, all norms are ~R^2
and most survive MR (sieve catches composites), so MR cost is uniform across
candidates -- any distribution scheme achieves balance.

```cuda
int total = cand_prefix[270] + cand_counts[270];

for (int i = threadIdx.x; i < total; i += 288) {
    uint32_t packed = cand_list[i];
    int cand_row = packed >> 16;
    int cand_col = packed & 0xFFFF;

    int32_t ca = a_start + cand_row;
    int32_t cb = b_start + cand_col;

    // Axis case (rare at operating point: only near-origin tiles)
    if (ca == 0 || cb == 0) {
        if (is_axis_gaussian_prime_gpu(ca, cb))
            atomicOr(&bitmap[cand_row * 9 + (cand_col >> 5)], 1u << (cand_col & 31));
        continue;
    }

    uint64_t norm = (uint64_t)ca * ca + (uint64_t)cb * cb;
    if (is_prime_gpu(norm))
        atomicOr(&bitmap[cand_row * 9 + (cand_col >> 5)], 1u << (cand_col & 31));
}
__syncthreads();
```

**atomicOr on shared memory:** Required because different threads may write to the
same bitmap word (candidates from the same row distributed to different threads).
Ampere shared-memory atomics are single-cycle on no-conflict. Contention is low:
32 threads writing across 2,439 words.

**Montgomery multiplication (no __int128 on CUDA):**

```cuda
__device__ uint64_t mont_mul_gpu(uint64_t a, uint64_t b, uint64_t m, uint64_t m_inv) {
    uint64_t lo = a * b;
    uint64_t hi = __umul64hi(a, b);           // upper 64 bits of a*b

    uint64_t q     = lo * m_inv;
    uint64_t qm_lo = q * m;
    uint64_t qm_hi = __umul64hi(q, m);

    uint64_t carry = (lo + qm_lo < lo) ? 1ULL : 0ULL;
    uint64_t r     = hi + qm_hi + carry;

    return (r >= m) ? r - m : r;
}
```

4 u64 multiplies (~4 cycles each on Ampere) + 3 adds + 1 branch = ~20 cycles/mont_mul.
Per MR witness: ~90 mont_mul (powmod with ~60-bit exponent).
Per candidate (12 witnesses): 12 x 90 = 1,080 mont_mul = ~21,600 cycles.
Total MR phase: ~1,763 candidates x 21,600 / 288 threads = **132K cycles/thread**.

**is_prime_gpu sequence (mirrors C++ is_prime):**

```
1. Small-case checks (n < 2, n == 2|3, even) -- branch-free where possible
2. Trial division by 24 small primes (3..97) -- constant memory
3. If n < 97^2 = 9409: done (prime if survived trial division)
4. Factor out powers of 2 from n-1: d = (n-1) >> s
5. Montgomery context init: m_inv, r2, one, nm1
6. 12 MR witnesses: mont_powmod + squaring chain, early exit on composite
```

### 4.4 Phase 2: Compact (271 threads, row-parallel)

Row-padded bitmap enables fully parallel compaction -- each thread processes
its own row independently.

```cuda
if (threadIdx.x < 271) {
    // Count primes in my row
    uint32_t count = 0;
    for (int w = 0; w < 9; w++) {
        uint32_t word = bitmap[threadIdx.x * 9 + w];
        if (w == 8) word &= LAST_WORD_MASK;
        count += __popc(word);
    }

    // Parallel exclusive scan -> row_prefix[0..271]
    // (row_prefix[r] = total primes in rows 0..r-1)
    row_prefix[threadIdx.x] = count;
    __syncthreads();
    block_exclusive_scan(row_prefix, 271);
    __syncthreads();

    // Bit-extract my row's primes into prime_pos[]
    int offset = row_prefix[threadIdx.x];
    for (int w = 0; w < 9; w++) {
        uint32_t word = bitmap[threadIdx.x * 9 + w];
        if (w == 8) word &= LAST_WORD_MASK;
        while (word) {
            int bit = __ffs(word) - 1;
            prime_pos[offset++] = (threadIdx.x << 16) | (w * 32 + bit);
            word &= word - 1;
        }
    }
}
__syncthreads();
```

**prime_pos encoding:** `(row << 16) | col` -- avoids integer division by 271 in
downstream phases. Row = `pos >> 16`, col = `pos & 0xFFFF`.

**uf_index lookup (used in Phases 3-4):**

```cuda
__device__ int uf_index(int row, int col, uint32_t* bitmap, uint32_t* row_prefix) {
    int idx = row_prefix[row];
    // Count primes before (row, col) within the row
    for (int w = 0; w < (col >> 5); w++)
        idx += __popc(bitmap[row * 9 + w]);
    idx += __popc(bitmap[row * 9 + (col >> 5)] & ((1u << (col & 31)) - 1));
    return idx;
}
```

0-8 popcount ops + 1 masked popcount. All data in shared memory. ~5 cycles average.

### 4.5 Phase 3: Union-Find (32 threads, lock-free CAS)

Union-find has inherent sequential dependencies (backward offsets reach 6 rows).
Single-thread would cost ~1.1M cycles and dominate GPU time. Using one warp (32
threads) with CAS-based lock-free union-find on shared-memory u32 parent array.

```cuda
if (threadIdx.x < 32) {
    int lane = threadIdx.x;
    int prime_count = row_prefix[271];  // total from scan

    // Initialize parent[i] = i
    for (int i = lane; i < prime_count; i += 32)
        parent[i] = i;
    __syncwarp();

    // Neighbor scan + union
    for (int i = lane; i < prime_count; i += 32) {
        int row = prime_pos[i] >> 16;
        int col = prime_pos[i] & 0xFFFF;

        for (int k = 0; k < 64; k++) {
            int nr = row + c_bk_dr[k];
            int nc = col + c_bk_dc[k];
            if (nr < 0 || nr >= 271 || nc < 0 || nc >= 271) continue;
            if (!bitmap_test(bitmap, nr, nc)) continue;

            int j = uf_index(nr, nc, bitmap, row_prefix);
            atomic_union(parent, (uint32_t)i, (uint32_t)j);
        }
    }
    __syncwarp();

    // Flatten all paths
    for (int i = lane; i < prime_count; i += 32)
        parent[i] = atomic_find(parent, (uint32_t)i);
}
__syncthreads();
```

**Lock-free union (Jayanti & Tarjan):**

```cuda
__device__ uint32_t atomic_find(uint32_t* parent, uint32_t x) {
    uint32_t p = parent[x];
    while (p != x) {
        uint32_t gp = parent[p];              // path splitting
        atomicCAS(&parent[x], p, gp);         // compress
        x = p;
        p = gp;
    }
    return x;
}

__device__ void atomic_union(uint32_t* parent, uint32_t x, uint32_t y) {
    while (true) {
        uint32_t rx = atomic_find(parent, x);
        uint32_t ry = atomic_find(parent, y);
        if (rx == ry) return;
        if (rx > ry) { uint32_t t = rx; rx = ry; ry = t; }
        if (atomicCAS(&parent[ry], ry, rx) == ry) return;
        // CAS failed: another thread changed parent[ry], retry
    }
}
```

parent[] is uint32_t (not uint16_t) for native atomicCAS support on CUDA.
Costs 4 bytes vs 2 bytes per entry -- 12,288 B vs 6,144 B. Acceptable for
atomic correctness.

**Estimated cost:** 32 threads x ~55 primes/thread x 64 offsets x ~10 cycles
= ~50K cycles. Down from ~1.1M single-thread.

### 4.6 Phase 4-5: Face Extract + Encode (1 thread)

Tiny data volume: ~60 ports pre-prune, ~30 post-prune, 128-byte TileOp output.
Single-thread is appropriate -- total work is ~2K operations.

```cuda
if (threadIdx.x == 0) {
    int prime_count = row_prefix[271];

    FaceDataGPU face;
    extract_faces_gpu(bitmap, row_prefix, prime_pos, prime_count, parent, &face);
    prune_dead_ends_gpu(&face);
    encode_tileop_gpu(&face, &output[blockIdx.x]);
}
```

Port/FaceData structs are in registers or small local arrays (< 512 B).
TileOp is written directly to global memory (128 B = 2 cache lines, coalesced
if output[] is 128-byte aligned).

---

## 5. Correctness Contract

The CUDA kernel must produce byte-identical TileOps to the C++ reference for
every valid tile coordinate. Validation strategy:

1. **Unit**: each phase tested against C++ phase output for a fixed set of tiles.
2. **Cross-validation**: full pipeline on 1,000+ tiles, TileOp bytes compared.
3. **Census**: 100K tile census, overflow/poison statistics match C++ census.
4. **Poison path**: tiles exceeding 2304 primes produce all-0xFF TileOp (same
   semantics as C++ overflow -- compositor treats as conservative bridge).

---

## 6. Cycle Budget Estimate (Ampere, ~1.3 GHz)

| Phase | Threads | Cycles/tile | % of total | Bottleneck |
|-------|---------|-------------|------------|------------|
| 1a Sieve | 271 | ~100K | 34% | Split-prime modular arithmetic |
| 1b Collect | 271 | ~5K | 2% | Prefix scan + scatter |
| 1c MR | 288 | ~132K | 45% | Montgomery powmod (u64 multiply) |
| 2 Compact | 271 | ~3K | 1% | Popcount + prefix scan |
| 3 UF | 32 | ~50K | 17% | Bitmap tests + CAS |
| 4-5 Face+Enc | 1 | ~2K | 1% | Sequential port processing |
| **Total** | | **~292K** | | |

At 1.3 GHz: ~292K cycles = **0.22 ms/tile**.
8 SMs (Orin Nano): 8 tiles in flight = **~36K tiles/sec** (theoretical peak).
With occupancy/memory effects, expect **10-20K tiles/sec** realistic.

C++ baseline: 1,003 tiles/sec (12 threads). Target speedup: **10-20x**.

### 6.1 Measured Jetson Baselines

**Hardware:** Orin Nano, sm_87, 8 SMs, CUDA 12.6, driver 540.4.0

**Current baseline (2026-04-10, commit on feature/gpu-uf-v2):**

| Metric | Value |
|--------|-------|
| Throughput (10K tiles) | **1,131 tiles/sec** |
| Throughput (1K tiles) | 1,102 tiles/sec |
| ms/tile (steady state) | 0.884 |
| Blocks/SM | 4 |
| Occupancy | 75% (1,152 / 1,536 threads) |
| Registers/thread | 46 |
| Dynamic shared mem | 36,640 bytes |
| Static shared mem | 1,040 bytes |
| Stack/thread | 56 bytes |
| MAX_PRIMES_GPU | 2560 |
| MAX_CANDIDATES_GPU | 6144 |
| FACES_PER_PASS | 2 |

**Key changes from initial kernel:**
- 2-face-per-pass extraction (halved face_cells, recovered 4-block occupancy)
- Separated MAX_CANDIDATES_GPU (sieve survivors) from MAX_PRIMES_GPU (confirmed primes)
- Candidate census: 490K+ tiles, observed max 5,882 at operating radii (R >= 800M)

**Historical progression:**

| State | Tiles/sec | Blocks/SM | Notes |
|-------|-----------|-----------|-------|
| Initial cooperative phase 4 | 467 | 4 | 33,312B stack spill dominated |
| + Stack spill fix | 341 | 3 | Occupancy regression from fat face_cells |
| + 2-face-per-pass, N=2560 | 954 | 3 | Benchmark fix (8192 candidates) |
| + Tightened candidates (6144) | **1,131** | **4** | Current baseline |

**Remaining optimization targets (not yet implemented):**
- Barrett reduction for sieve modulo (est. 5-8x sieve speedup)
- Single sieve pass (eliminate duplicate sieve_row calls)

---

## 7. Implementation Notes

### 7.1 Shared Memory Configuration

```cuda
cudaFuncSetAttribute(process_tiles_kernel,
    cudaFuncAttributeMaxDynamicSharedMemorySize, 36640);  // 35.8 KB
```

Use dynamic shared memory (`extern __shared__`) for the union layout.
36,640 bytes dynamic + 1,040 bytes static = 37,680 bytes/block.
4 blocks x 37,680 = 150,720 bytes < 167,936 bytes/SM. Fits with 17 KB headroom.

### 7.2 Occupancy

At 288 threads/block, 46 registers/thread, 37,680 bytes shared/block:
- Orin Nano (sm_87): 167,936 bytes shared/SM -> 4 blocks/SM.
  288 x 4 = 1,152 threads < 1,536 max. 75% occupancy.
- Registers: 46 x 288 x 4 = 53,568 < 65,536/SM. Fits.

### 7.3 Poison Path

```cuda
int prime_count = row_prefix[271];
if (prime_count > MAX_PRIMES_GPU) {  // 2560
    // Poison: write overflow sentinel, skip Phases 3-5
    if (threadIdx.x == 0) {
        memset(&output[blockIdx.x], 0xFF, 128);
    }
    return;
}
```

Poisoned tiles are retried on CPU (C++ pipeline, 1,003 tiles/sec).
At operating point (R >= 800M): zero tiles hit this path per 490K census.
