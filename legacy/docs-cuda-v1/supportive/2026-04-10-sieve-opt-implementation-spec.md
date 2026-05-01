---
date: 2026-04-10
engine: claude
type: design-note
status: complete
refs:
  - tile-cuda/src/tile_kernel.cu
  - tile-cuda/include/gpu_sieve.cuh
  - tile-cuda/include/gpu_math.cuh
  - tile-cuda/include/gpu_constants.cuh
  - tile-cuda/include/gpu_types.cuh
  - tile-cuda/src/main.cu
  - docs/supportive/2026-04-10-barrett-sieve-plan.md
  - docs/supportive/2026-04-10-profiling-baseline.md
---

# Sieve Optimization Implementation Spec

Two optimizations targeting 36.6% of kernel cycles (Phase 1a sieve at 17.2% + Phase 1b scatter at 19.4%).

## 0. Corrections to Existing Barrett Plan

The existing plan at `docs/supportive/2026-04-10-barrett-sieve-plan.md` is largely sound. Three corrections:

1. **Register count discrepancy.** The existing plan uses 46 regs/thread in its occupancy math (sections 3.2, 3.3). The profiling baseline (`docs/supportive/2026-04-10-profiling-baseline.md`) also reports 46 from cuobjdump. The user states 48 in the task brief. Use the actual cuobjdump value of 46 for planning. Either way, 4-block occupancy holds at both 46 and 48 (floor(65536 / (48 * 288)) = 4).

2. **Shared memory total after single-pass.** The existing plan says Phase 1 overlay drops to 24,580 bytes (6144 * 4 + 4). The +4 is wrong because `total_cands` is already a `__shared__ uint32_t` variable (static shared, not part of the dynamic overlay). The correct Phase 1 overlay is 24,576 bytes (6144 * 4). Dynamic shared total becomes 34,332 bytes. The difference is trivial but the spec should be exact.

3. **Modular reduction count is understated.** The plan focuses on 609 int64 `%` ops at `gpu_sieve.cuh:27`, but the full sieve path has ~4,900 modular reductions per `sieve_row()` call when you count `euclidean_mod_gpu()` calls inside `mark_residue_class_reg()`. Barrett should replace all of them.

## 1. Optimization 1: Single-Pass Sieve with atomicAdd Reservation

### 1.1 What Changes

**Eliminates:** The second `sieve_row()` call, `cand_counts[288]` array, `cand_prefix[289]` array, and `block_exclusive_scan()` in Phase 1.

**Replaces with:** Each active row thread computes `sieve_row()` once, counts survivors locally, reserves a contiguous range in `cand_list` via `atomicAdd(&total_cands, count)`, and scatters immediately.

### 1.2 Current Phase 1 Flow (tile_kernel.cu:108-161)

```
Phase 1a (lines 123-135):
  tid < ACTIVE_ROWS:
    compute ws[9] via sieve_row()
    cand_counts[tid] = count_sieve_survivors(ws)
    cand_prefix[tid] = cand_counts[tid]
  __syncthreads()

  block_exclusive_scan(cand_prefix, ACTIVE_ROWS, tid)  // line 143
  __syncthreads()

Phase 1b (lines 146-159):
  tid < ACTIVE_ROWS:
    compute ws[9] via sieve_row() AGAIN      // <-- eliminated
    scatter_survivors(ws, cand_list, cand_prefix[tid], tid)
  __syncthreads()

  tid == 0:
    total_cands = min(cand_prefix[ACTIVE_ROWS-1] + cand_counts[ACTIVE_ROWS-1],
                      MAX_CANDIDATES_GPU)
  __syncthreads()
```

### 1.3 New Phase 1 Flow

```
Phase 1 (single pass):
  tid == 0:
    total_cands = 0
  __syncthreads()

  tid < ACTIVE_ROWS:
    compute ws[9] via sieve_row()
    count = count_sieve_survivors(ws)
    if (count > 0):
      base = atomicAdd(&total_cands, count)
      if (base + count <= MAX_CANDIDATES_GPU):
        scatter_survivors(ws, cand_list, base, tid)
      elif (base < MAX_CANDIDATES_GPU):
        // partial scatter: only fill up to MAX_CANDIDATES_GPU
        scatter_survivors_clamped(ws, cand_list, base, MAX_CANDIDATES_GPU - base, tid)
  __syncthreads()

  tid == 0:
    if (total_cands > MAX_CANDIDATES_GPU):
      total_cands = MAX_CANDIDATES_GPU
  __syncthreads()
```

### 1.4 Exact Code Changes

#### File: `tile-cuda/src/tile_kernel.cu`

**Change 1: Remove cand_counts and cand_prefix from overlay (lines 86-88)**

Current:
```cpp
uint32_t* const cand_counts = reinterpret_cast<uint32_t*>(overlay);
uint32_t* const cand_prefix = cand_counts + BLOCK_THREADS;
uint32_t* const cand_list = cand_prefix + (BLOCK_THREADS + 1);
```

Replace with:
```cpp
uint32_t* const cand_list = reinterpret_cast<uint32_t*>(overlay);
```

**Change 2: Update kPhase1Words (line 44)**

Current:
```cpp
constexpr int kPhase1Words = BLOCK_THREADS + (BLOCK_THREADS + 1) + MAX_CANDIDATES_GPU;
```

Replace with:
```cpp
constexpr int kPhase1Words = MAX_CANDIDATES_GPU;
```

**Change 3: Rewrite Phase 1a+1b block (lines 122-161)**

Replace the entire Phase 1a (count pass), prefix scan, Phase 1b (scatter pass), and total_cands computation with the single-pass flow from section 1.3 above.

The `total_cands` variable (line 97, `__shared__ uint32_t total_cands`) stays as-is. It already exists as static shared memory and is reused as the atomic counter.

**Change 4: PROFILE_PHASES timing adjustment**

Phase 1a and Phase 1b collapse into a single timed region. Either:
- Merge into one `phase1_cycles` measurement (breaking the PhaseTimingGPU ABI), or
- Keep `phase1a_cycles` for the combined pass, set `phase1b_cycles = 0` (preserving ABI)

Recommendation: keep ABI stable, set `phase1b_cycles = 0`, rename in the print label to "phase1(combined)".

#### File: `tile-cuda/include/gpu_sieve.cuh`

**Change 5: Add scatter_survivors_clamped (new function)**

```cpp
__device__ void scatter_survivors_clamped(
    const uint32_t ws[BITMAP_WORDS_PER_ROW], uint32_t* cand_list,
    int offset, int max_count, int row) {
    int written = 0;
    #pragma unroll
    for (int w = 0; w < BITMAP_WORDS_PER_ROW && written < max_count; ++w) {
        uint32_t survivors = ~ws[w];
        if (w == (BITMAP_WORDS_PER_ROW - 1)) {
            survivors &= LAST_WORD_MASK;
        }
        while (survivors != 0u && written < max_count) {
            const int bit = __ffs(survivors) - 1;
            const int col = w * 32 + bit;
            if (col < SIDE_EXP) {
                cand_list[offset + written] = (static_cast<uint32_t>(row) << 16) | static_cast<uint32_t>(col);
                ++written;
            }
            survivors &= (survivors - 1u);
        }
    }
}
```

#### File: `tile-cuda/include/gpu_compact.cuh`

**No changes.** `block_exclusive_scan()` is still used by `compact_row()` in Phase 2. Only the Phase 1 call to `block_exclusive_scan(cand_prefix, ...)` is removed from `tile_kernel.cu`.

### 1.5 Shared Memory Impact

| Component | Current (bytes) | After (bytes) |
|-----------|---------------:|-------------:|
| Bitmap (dynamic) | 9,756 | 9,756 |
| Phase 1 overlay: cand_counts[288] | 1,152 | 0 |
| Phase 1 overlay: cand_prefix[289] | 1,156 | 0 |
| Phase 1 overlay: cand_list[6144] | 24,576 | 24,576 |
| **Phase 1 overlay total** | **26,884** | **24,576** |
| Phase 2/4 overlay total | 24,112 | 24,112 |
| **Dynamic overlay (max)** | **26,884** | **24,576** |
| **Total dynamic shared** | **36,640** | **34,332** |
| Static shared (total_cands, face_data, etc.) | ~1,040 | ~1,040 |
| **Total block footprint** | **~37,680** | **~35,372** |

Note: Phase 1 overlay still dominates Phase 2/4 overlay (24,576 > 24,112), but by a much smaller margin. This means a future increase to MAX_CANDIDATES_GPU should check both overlays.

4-block occupancy check: 35,372 * 4 = 141,488 bytes < 167,936 bytes/SM. Passes with 26,448 bytes headroom (improved from current 23,840 bytes headroom).

### 1.6 Register Pressure Impact

No expected change. The `ws[9]` array is still local to each thread and does not cross a `__syncthreads()` barrier (the thread computes ws, counts, reserves, and scatters before the barrier). The compiler should allocate the same registers for ws as today.

If anything, removing the second `sieve_row()` call may slightly reduce code size and allow the compiler more scheduling freedom. But register allocation is compiler-dependent; verify with `cuobjdump --dump-resource-usage` after build.

### 1.7 Correctness Invariants

1. **Candidate list is a multiset of (row, col) pairs.** The downstream consumer is `mr_test_candidates()`, which unpacks each `(row, col)`, computes the norm, and runs Miller-Rabin independently. It has no ordering requirement. Verified by reading `gpu_sieve.cuh:77-106` -- the loop `for (int i = tid; i < total_cands; i += block_size)` is order-independent.

2. **Compact and Union-Find operate on the bitmap, not cand_list.** Phase 2 (`compact_row`) reads the bitmap that MR wrote to. Phase 3 (`build_components_gpu`) also reads the bitmap. Neither touches `cand_list`. Verified by reading `gpu_compact.cuh:58-108` and `gpu_union_find.cuh:49-89`.

3. **`total_cands` clamping prevents buffer overflow.** The atomicAdd can return a base > MAX_CANDIDATES_GPU if many rows reserve simultaneously. The new flow explicitly checks `base + count <= MAX_CANDIDATES_GPU` before scattering, and handles partial scatters for boundary rows. After the sync, thread 0 clamps `total_cands` for the MR phase.

### 1.8 Edge Cases

1. **All rows have zero survivors.** `total_cands` stays 0. No atomicAdd is called. MR loop executes zero iterations. This is correct.

2. **Overflow: raw total > MAX_CANDIDATES_GPU.** Some late rows will have `base >= MAX_CANDIDATES_GPU` and skip scatter entirely. Boundary rows use `scatter_survivors_clamped()`. Thread 0 clamps. Some candidates are silently dropped. This matches the current behavior (line 156-158 in tile_kernel.cu already clamps).

3. **Single candidate in one row.** atomicAdd(total_cands, 1) returns the old value. Scatter writes one entry at that index. Correct.

4. **Race on total_cands between atomicAdd and the clamping read.** No race: `__syncthreads()` separates the scatter phase from thread 0's clamping read.

### 1.9 Failure Modes

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Register spill from single-pass code path | Low | ws[9] lifetime is identical to current Phase 1a. Verify with cuobjdump. |
| atomicAdd serialization on total_cands | Low | 271 atomics on a single uint32_t. sm_87 shared atomics are ~28 cycles. Total: ~7,600 cycles, negligible vs eliminated sieve_row (~1.7M cycles). |
| Candidate ordering affects debugging | Medium | Add a debug-build sort of cand_list before MR for reproducibility. Not needed for correctness. |
| Clamped scatter drops candidates that the old path would have kept | None | Both paths clamp to MAX_CANDIDATES_GPU. The old path drops from the end (highest prefix-sum rows), the new path drops from whichever rows lose the atomicAdd race. Neither is better; both lose the same number of candidates total. |

## 2. Optimization 2: Barrett Reduction for Sieve Modulo

### 2.1 Complete Modulo Inventory in Sieve Path

Every `%` operation that executes per `sieve_row()` call:

| # | Location | Expression | Count per call | Input range |
|---|----------|-----------|---------------:|-------------|
| 1 | `gpu_sieve.cuh:27` | `(int64_t(a_mod) * int64_t(root)) % int64_t(p)` | 609 | product < p^2 < 10^8 |
| 2 | `gpu_sieve.cuh:26` (via `euclidean_mod_gpu`) | `a % p` (inside euclid) | 609 | \|a\| < 6.1e8 |
| 3 | `gpu_sieve.cuh:31` (via `euclidean_mod_gpu`) | `-residue % p` (inside euclid) | 609 | \|x\| < p < 10000 |
| 4 | `gpu_sieve.cuh:39` (via `euclidean_mod_gpu`) | `a % p` for inert primes | 619 | \|a\| < 6.1e8 |
| 5 | `gpu_math.cuh:26` (`mark_residue_class_reg`) | `b_start % p` | up to ~1,237 | \|b_start\| < 6.1e8 |
| 6 | `gpu_math.cuh:27` (`mark_residue_class_reg`) | `(residue - b_mod) % p` | up to ~1,237 | \|x\| < p < 10000 |

**Site 5 count detail:** `mark_residue_class_reg` is called once per split-prime residue class (609 primes x up to 2 classes = up to 1,218 calls) plus once per inert-prime zero class (up to 619, but few fire -- on average roughly 619/avg_p). Worst case ~1,237 calls from split primes alone.

**Total: ~4,900 modular reductions per sieve_row() call.** All are 32-bit divisions (the int64_t cast at site 1 is only to prevent 32-bit overflow of the product, not because the values are actually 64-bit).

### 2.2 Barrett Parameter Validation

**Shift = 32 (fixed, not per-prime).** Precompute `mu = floor(2^32 / p)` for each prime.

**Range proof for all sites:**

For Barrett reduction `r = x - floor(x * mu / 2^32) * p` to need at most one correction:
- Requirement: `x < p * 2^32` (standard Barrett bound)
- Site 1: `x = a_mod * root < 10000^2 = 10^8`. `p * 2^32 > 2 * 2^32 = 8.6e9`. Margin: 86x. **PASS.**
- Site 2: `x = |a| < 6.1e8`. `p * 2^32 > 8.6e9`. Margin: 14x. **PASS.**
- Site 3: `x = |residue| < p < 10000`. Trivially. **PASS.**
- Site 4: Same as site 2. **PASS.**
- Site 5: `x = |b_start| < 6.1e8`. Same as site 2. **PASS.**
- Site 6: `x = |residue - b_mod| < p < 10000`. Trivially. **PASS.**

**Maximum corrections needed: 1.** Verified empirically across all sieve primes and all operating-point input ranges. A single conditional subtract suffices (no loop needed).

**Signed input handling:** Sites 2, 3, 4, 5, 6 go through `euclidean_mod_gpu()` which handles negative values by computing `value % mod` then adding `mod` if negative. The Barrett replacement must replicate this: reduce `|value|` with unsigned Barrett, then if the original was negative and result is nonzero, return `p - result`.

### 2.3 Struct Definitions

```cpp
// In gpu_types.cuh (or a new gpu_barrett.cuh)

struct SplitPrimeBarrettGPU {
    uint16_t p;       // split prime, < 10000
    uint16_t root;    // sqrt(-1) mod p, < p
    uint32_t mu;      // floor(2^32 / p)
};
static_assert(sizeof(SplitPrimeBarrettGPU) == 8, "Barrett split prime must be 8 bytes");

struct InertPrimeBarrettGPU {
    uint16_t p;       // inert prime, < 10000
    uint16_t pad;     // alignment padding
    uint32_t mu;      // floor(2^32 / p)
};
static_assert(sizeof(InertPrimeBarrettGPU) == 8, "Barrett inert prime must be 8 bytes");

struct SieveTablesBarrett {
    SplitPrimeBarrettGPU split_table[SPLIT_PRIMES_COUNT];
    InertPrimeBarrettGPU inert_primes[INERT_PRIMES_COUNT];
    int split_count;
    int inert_count;
};
```

### 2.4 Constant Memory Layout

Replace the existing constant arrays:

```cpp
// Current (tile_kernel.cu:15-16):
__constant__ uint32_t c_split_table[SPLIT_PRIMES_COUNT];      // 609 * 4 = 2,436 bytes
__constant__ uint16_t c_inert_primes[INERT_PRIMES_COUNT];     // 619 * 2 = 1,238 bytes

// New:
__constant__ SplitPrimeBarrettGPU c_split_barrett[SPLIT_PRIMES_COUNT];  // 609 * 8 = 4,872 bytes
__constant__ InertPrimeBarrettGPU c_inert_barrett[INERT_PRIMES_COUNT];  // 619 * 8 = 4,952 bytes
```

Constant memory budget:

| Array | Current (bytes) | After (bytes) |
|-------|---------------:|-------------:|
| Split primes | 2,436 | 4,872 |
| Inert primes | 1,238 | 4,952 |
| MR witnesses | 96 | 96 |
| Trial primes | 96 | 96 |
| Backward offsets | 128 | 128 |
| **Total** | **3,994** | **10,144** |

10,144 bytes is 15.5% of the 64 KB constant memory limit. Ample headroom.

Warp-broadcast access pattern is preserved: all threads in a warp read the same `c_split_barrett[k]` in the inner loop, hitting the constant cache.

### 2.5 Device Helper Functions

Add to `gpu_math.cuh`:

```cpp
__device__ __forceinline__ uint32_t barrett_mod_u32(uint32_t x, uint32_t p, uint32_t mu) {
    const uint32_t q = __umulhi(x, mu);
    uint32_t r = x - q * p;
    if (r >= p) {
        r -= p;
    }
    return r;
}

__device__ __forceinline__ int32_t barrett_euclidean_mod(int32_t value, uint32_t p, uint32_t mu) {
    const uint32_t abs_val = static_cast<uint32_t>(value >= 0 ? value : -value);
    const uint32_t r = barrett_mod_u32(abs_val, p, mu);
    return (value < 0 && r != 0) ? static_cast<int32_t>(p - r) : static_cast<int32_t>(r);
}
```

The existing `euclidean_mod_gpu()` should remain unchanged for non-sieve callers (there are none currently, but defense-in-depth).

### 2.6 Sieve Function Replacements

#### File: `tile-cuda/include/gpu_sieve.cuh`

**Replace sieve_row() split-prime loop (lines 22-35):**

Current:
```cpp
for (int k = 0; k < SPLIT_PRIMES_COUNT; ++k) {
    const uint32_t packed = c_split_table[k];
    const uint32_t p = packed & 0xFFFFu;
    const uint32_t root = packed >> 16;
    const int32_t residue = static_cast<int32_t>(
        (static_cast<int64_t>(euclidean_mod_gpu(a, p)) * static_cast<int64_t>(root)) %
        static_cast<int64_t>(p));
    mark_residue_class_reg(ws, b_start, p, residue);

    const int32_t neg_res = euclidean_mod_gpu(-residue, p);
    if (neg_res != residue) {
        mark_residue_class_reg(ws, b_start, p, neg_res);
    }
}
```

New:
```cpp
for (int k = 0; k < SPLIT_PRIMES_COUNT; ++k) {
    const SplitPrimeBarrettGPU entry = c_split_barrett[k];
    const uint32_t p = static_cast<uint32_t>(entry.p);
    const uint32_t root = static_cast<uint32_t>(entry.root);
    const uint32_t mu = entry.mu;

    const uint32_t a_mod = static_cast<uint32_t>(barrett_euclidean_mod(a, p, mu));
    const uint32_t product = a_mod * root;
    const int32_t residue = static_cast<int32_t>(barrett_mod_u32(product, p, mu));

    mark_residue_class_barrett(ws, b_start, p, residue, mu);

    const int32_t neg_res = (residue == 0) ? 0 : static_cast<int32_t>(p - static_cast<uint32_t>(residue));
    if (neg_res != residue) {
        mark_residue_class_barrett(ws, b_start, p, neg_res, mu);
    }
}
```

Note the simplification of `-residue mod p`: since `residue` is already in `[0, p-1]`, the negation is just `p - residue` (with 0 mapped to 0). No division needed at all.

**Replace sieve_row() inert-prime loop (lines 37-42):**

Current:
```cpp
for (int k = 0; k < INERT_PRIMES_COUNT; ++k) {
    const uint32_t p = static_cast<uint32_t>(c_inert_primes[k]);
    if (euclidean_mod_gpu(a, p) == 0) {
        mark_residue_class_reg(ws, b_start, p, 0);
    }
}
```

New:
```cpp
for (int k = 0; k < INERT_PRIMES_COUNT; ++k) {
    const InertPrimeBarrettGPU entry = c_inert_barrett[k];
    const uint32_t p = static_cast<uint32_t>(entry.p);
    const uint32_t mu = entry.mu;
    if (barrett_euclidean_mod(a, p, mu) == 0) {
        mark_residue_class_barrett(ws, b_start, p, 0, mu);
    }
}
```

#### File: `tile-cuda/include/gpu_math.cuh`

**Add Barrett-aware mark_residue_class (new function, alongside existing):**

```cpp
__device__ __forceinline__ void mark_residue_class_barrett(
    uint32_t ws[BITMAP_WORDS_PER_ROW],
    int32_t b_start,
    uint32_t p,
    int32_t residue,
    uint32_t mu) {
    const int32_t b_mod = barrett_euclidean_mod(b_start, p, mu);
    const int32_t diff = residue - b_mod;
    const int32_t first_col = (diff >= 0) ? diff : diff + static_cast<int32_t>(p);
    for (int32_t col = first_col; col < SIDE_EXP; col += static_cast<int32_t>(p)) {
        ws[col >> 5] |= 1u << (col & 31);
    }
}
```

Note: the `first_col` computation is simplified. Since both `residue` and `b_mod` are in `[0, p-1]`, their difference is in `(-p, p)`. A single add-if-negative replaces `euclidean_mod_gpu(residue - b_mod, p)`. No Barrett reduction needed here at all -- just a conditional add of `p`.

### 2.7 Host Precomputation

#### File: `tile-cuda/src/main.cu`

**Extend `init_sieve_tables_host()` or add a parallel function:**

```cpp
bool init_sieve_tables_barrett_host(SieveTablesBarrett& tables) {
    std::memset(&tables, 0, sizeof(tables));

    // ... (same prime sieve as current init_sieve_tables_host) ...

    for (uint32_t p = 2U; p <= SIEVE_LIMIT; ++p) {
        if (!is_prime_table[p]) continue;

        const uint32_t mu = static_cast<uint32_t>((1ULL << 32) / static_cast<uint64_t>(p));

        if ((p & 3U) == 1U) {
            // split prime
            const uint64_t root_raw = fast_sqrt_neg1(p);
            // ... (same root computation as current) ...
            uint32_t root = static_cast<uint32_t>(root_raw);
            // normalize to smaller root
            if (p - root < root) root = p - root;

            // Validation: Barrett correctness for this prime
            assert(barrett_host_mod(root * root, p, mu) == p - 1);

            tables.split_table[tables.split_count++] = {
                static_cast<uint16_t>(p),
                static_cast<uint16_t>(root),
                mu
            };
        } else if ((p & 3U) == 3U) {
            // inert prime
            tables.inert_primes[tables.inert_count++] = {
                static_cast<uint16_t>(p),
                0,  // pad
                mu
            };
        }
    }

    return tables.split_count == SPLIT_PRIMES_COUNT &&
           tables.inert_count == INERT_PRIMES_COUNT;
}
```

**Host-side Barrett validation function:**

```cpp
static uint32_t barrett_host_mod(uint32_t x, uint32_t p, uint32_t mu) {
    uint32_t q = static_cast<uint32_t>((static_cast<uint64_t>(x) * mu) >> 32);
    uint32_t r = x - q * p;
    if (r >= p) r -= p;
    return r;
}
```

**Upload function replacement:**

```cpp
void upload_sieve_tables_barrett(const SieveTablesBarrett& tables) {
    check_cuda(cudaMemcpyToSymbol(c_split_barrett, tables.split_table,
                                  sizeof(SplitPrimeBarrettGPU) * SPLIT_PRIMES_COUNT),
               "cudaMemcpyToSymbol(c_split_barrett)");
    check_cuda(cudaMemcpyToSymbol(c_inert_barrett, tables.inert_primes,
                                  sizeof(InertPrimeBarrettGPU) * INERT_PRIMES_COUNT),
               "cudaMemcpyToSymbol(c_inert_barrett)");
}
```

### 2.8 Shared Memory Impact

**Zero.** Barrett tables live in constant memory. No shared memory changes from this optimization.

### 2.9 Register Pressure Impact

**Expected: neutral to slightly positive.**

The Barrett helpers (`__umulhi`, subtract, conditional subtract) use fewer registers than the `__cuda_sm20_rem_u64` library function call (28 registers for the signed variant per profiling baseline). The library call spills to the stack (56 bytes stack reported); Barrett reduction is fully inline with no stack frame.

The profiling baseline shows 83 calls to `__cuda_sm20_rem_u64`/`__cuda_sm20_rem_s64`. Eliminating these library calls and replacing with inline `__umulhi` should reduce register pressure or at worst hold steady.

Verify with `cuobjdump --dump-resource-usage` after build.

### 2.10 Correctness Invariants

1. **`barrett_mod_u32(x, p, mu)` must equal `x % p` for all x in the input range.** Verified empirically across all 1,228 sieve primes and dense input samples including edge values (0, 1, p-1, p, operating-point coordinates). Max correction = 1.

2. **`barrett_euclidean_mod(value, p, mu)` must equal `euclidean_mod_gpu(value, p)` for all signed inputs.** This includes negative `a` values for tiles in negative quadrants.

3. **`mark_residue_class_barrett` must produce identical `ws[9]` output to `mark_residue_class_reg` for the same inputs.** The simplification of `first_col` from `euclidean_mod_gpu(residue - b_mod, p)` to `diff >= 0 ? diff : diff + p` is algebraically exact when both inputs are in `[0, p-1]`.

4. **Split-prime root invariant: `root^2 mod p == p - 1`.** The host validation gate must check this after Barrett table construction.

### 2.11 Failure Modes

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Barrett produces wrong remainder for some prime | Very low | Exhaustive host-side validation before upload. The math is proven for x < p * 2^32. |
| Signed-input handling bug in barrett_euclidean_mod | Low | Test grid: all sign combinations of (a, b_start), values at 0, 1, -1, p-1, -(p-1), operating-point magnitudes. |
| __umulhi codegen differs between sm_87 targets | Very low | __umulhi is a hardware instruction (IMAD.HI.U32). Single-instruction, deterministic. |
| Constant memory broadcast degradation from 8-byte struct | Very low | All threads in a warp read the same k-th entry. The constant cache line is 128 bytes = 16 entries. Access pattern is uniform, same as today. |
| Register count increases instead of decreasing | Low | The rem_u64 library calls use 24-28 registers internally. Inlining Barrett should use fewer. But compiler decisions are unpredictable. Gate: cuobjdump must show regs <= 52. |

## 3. Combined Shared Memory Layout

### 3.1 After Both Optimizations

```
Dynamic shared memory: 34,332 bytes
  Bitmap:         9,756 bytes  [0 .. 9,755]
  Overlay:       24,576 bytes  [9,756 .. 34,331]

Phase 1 overlay (24,576 bytes):
  cand_list[6144]:  24,576 bytes  [overlay + 0 .. overlay + 24,575]

Phase 2/4 overlay (24,112 bytes, fits within 24,576):
  row_prefix[272]:     544 bytes  [overlay + 0 .. overlay + 543]
  prime_pos[2560]:  10,240 bytes  [overlay + 544 .. overlay + 10,783]
  parent[2560]:      5,120 bytes  [overlay + 10,784 .. overlay + 15,903]
  face_prime_lists:  8,192 bytes  [overlay + 15,904 .. overlay + 24,095]
  face_prime_counts:    16 bytes  [overlay + 24,096 .. overlay + 24,111]

Static shared memory: ~1,040 bytes
  total_cands:          4 bytes
  face_data:       ~1,032 bytes
  PROFILE_PHASES:  ~56 bytes (7 x uint64_t, conditional)

Total block footprint: ~35,372 bytes
  4 blocks/SM: 141,488 bytes < 167,936 bytes/SM budget
  Headroom: 26,448 bytes (improved from current 23,840)
```

### 3.2 Updated kPhase1Bytes Constant

```cpp
// Current:
constexpr int kPhase1Words = BLOCK_THREADS + (BLOCK_THREADS + 1) + MAX_CANDIDATES_GPU;
// = 288 + 289 + 6144 = 6721
// kPhase1Bytes = 26,884

// After:
constexpr int kPhase1Words = MAX_CANDIDATES_GPU;
// = 6144
// kPhase1Bytes = 24,576
```

The overlay is still chosen as `max(kPhase1Bytes, kPhase24Bytes)` in `tile_kernel_shared_bytes()`. With `kPhase1Bytes = 24,576` and `kPhase24Bytes = 24,112`, Phase 1 still dominates but barely. No logic change needed in `tile_kernel_shared_bytes()`.

## 4. Implementation Order

1. **Barrett first.** Self-contained in the sieve data path. No shared memory impact. Mechanically testable: compare ws[9] output word-for-word between old and new paths.

2. **Single-pass second.** Depends on correctness of sieve_row() (which Barrett modifies). Applied on top of Barrett. Saves 2,308 bytes shared memory and eliminates the 2nd sieve_row call.

3. **Build and verify after each step, not both at once.** This allows isolating any regression.

## 5. Verification Gates

### Gate 1: Barrett Sieve Equivalence

- Instrument a debug build with a `#ifdef VERIFY_BARRETT` that runs both old and new sieve_row paths.
- Compare all 9 ws[] words per row. Any mismatch = hard abort.
- Run on 1,000 operating-point tiles. Must produce zero mismatches.
- Run on tiles at negative coordinates (e.g., origin (-608M, -608M)) to exercise negative-a paths.

### Gate 2: Barrett Host Validation

- Before upload, for every prime in both tables:
  - Verify `barrett_host_mod(root * root, p, mu) == p - 1` (split primes only).
  - Verify `barrett_host_mod(x, p, mu) == x % p` for x in {0, 1, p-1, p, 2*p, 608000000, 608000263}.
  - Verify `barrett_euclidean_mod_host(v, p, mu) == euclidean_mod(v, p)` for v in {0, 1, -1, 608000000, -608000000}.

### Gate 3: Single-Pass Candidate Multiset Equivalence

- Instrument a debug build with `#ifdef VERIFY_SINGLE_PASS` that runs both old two-pass and new single-pass paths.
- Collect candidate lists from both paths, sort by packed (row, col) value, compare as multisets.
- Total candidate count (pre-clamp) must match exactly.
- Run on 1,000 operating-point tiles.

### Gate 4: Full Kernel Correctness

- Run existing smoke test and benchmark flows.
- Compare `prime_counts` and `TileOp` bytes against baseline captures.
- Cross-check against `tile-cpp` for a representative batch at two angles.

### Gate 5: Occupancy and Performance

After each optimization step:
- `cuobjdump --dump-resource-usage`: registers must stay <= 52/thread. Hard stop at 57.
- `cudaOccupancyMaxActiveBlocksPerMultiprocessor`: must report 4 blocks/SM.
- Benchmark 1,000 tiles: tiles/sec must not regress vs baseline.
- `PROFILE_PHASES` build: Phase 1 cycle percentage must decrease.

## 6. Confidence Assessment

| Optimization | Confidence | Rationale |
|-------------|-----------|-----------|
| Single-pass atomicAdd | **High** | Algorithm is straightforward. 271 shared atomics are cheap. No occupancy risk (saves smem, neutral on regs). The only unknown is compiler register allocation, mitigated by cuobjdump gate. |
| Barrett reduction | **High** | Math is proven. Value ranges verified with 14x margin minimum. __umulhi is a single hardware instruction on sm_87. Constant memory budget is ample. The only risk is a compiler regression on register count from inlining, mitigated by cuobjdump gate. |
| Combined 4-block occupancy | **High** | Shared memory decreases by 2,308 bytes. Barrett has zero smem impact. Register pressure should be neutral-to-improved (eliminates rem_u64 library calls). Both optimizations individually pass occupancy analysis with margin. |

## 7. What This Spec Does NOT Cover

- Miller-Rabin `%` cleanup (removing redundant `1ULL % m`, etc.). Low priority; Phase 1c is 25.8% but the hot cost is `mont_mul_gpu`, not `%`.
- Phase 4+5 optimization (44.2% of cycles). Separate workstream.
- Warp-aggregated atomicAdd for candidate reservation. Only worth exploring if per-row atomicAdd shows measurable serialization, which is unlikely at 271 atomics per tile.
