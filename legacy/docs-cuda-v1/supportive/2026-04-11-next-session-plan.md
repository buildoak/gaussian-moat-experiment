---
title: Next 4090 Session Plan — K1 Shared Memory + Sieve Extension
date: 2026-04-11
engine: claude
type: plan
status: draft
refs: [tile_cuda_multi_kernel/, docs/supportive/2026-04-11-4090-hardware-profiling.md, docs/supportive/2026-04-11-4090-tuning-sweep.md]
---

# Next 4090 Session Plan

Two optimizations + re-profiling on a rented RTX 4090 (vast.ai). Target: break 170K tiles/s at 10K tiles (currently 155K).

**Baseline (current best, 10K tiles):** 155,452 tiles/s

| Kernel | Duration (ms) | % | Bottleneck |
|--------|--------------|---|------------|
| K1 Sieve | 14.40 | 22.1% | cmem cache thrash (1.20x scaling, expected 1.56x) |
| K2 MR | 36.07 | 55.4% | INT32 throughput (4,231 SASS instr/candidate) |
| K3 Compact | 0.47 | 0.7% | — |
| K4 UF | 12.44 | 19.1% | INT32 + scatter/gather atomics |
| K5 FaceEncode | 1.75 | 2.7% | — |
| **Total** | **65.09** | | |

---

## Phase 0: Pre-Session Preparation (Mac Mini, before renting)

### 0A. Implement K1 shared memory patch locally

All code changes happen locally. The 4090 session is for build + test + profile only.

**Files to modify:**

1. `tile_cuda_multi_kernel/src/kernel_sieve.cu` — the K1 kernel and sieve table upload
2. `tile_cuda_multi_kernel/include/gpu_constants.cuh` — sieve constants
3. `tile_cuda_multi_kernel/include/gpu_math.cuh` — Barrett math helpers that read from cmem
4. `tile_cuda_multi_kernel/src/main.cu` — sieve table initialization, possible new device memory path

**Current constant memory layout** (`kernel_sieve.cu` lines 14-19):

```cuda
__constant__ SplitPrimeBarrettGPU c_split_barrett[SPLIT_PRIMES_COUNT];   // 609 * 8B = 4,872 B
__constant__ InertPrimeBarrettGPU c_inert_barrett[INERT_PRIMES_COUNT];   // 619 * 8B = 4,952 B
__constant__ uint64_t c_mr_witnesses[NUM_MR_WITNESSES];                   // 7 * 8B = 56 B
__constant__ uint32_t c_trial_primes[NUM_TRIAL_PRIMES];                   // 24 * 4B = 96 B
__constant__ int8_t   c_bk_dr[NUM_BACKWARD_OFFSETS];                      // 64 B
__constant__ int8_t   c_bk_dc[NUM_BACKWARD_OFFSETS];                      // 64 B
```

**Total constant memory for sieve tables:** 4,872 + 4,952 = **9,824 bytes** (~10 KB).

The MR witnesses, trial primes, and backward offsets (280 B total) can stay in constant memory — they are only read by K2/K4/K5 which don't have the same contention pattern, and 280 B fits trivially in the 8 KB L1 constant cache.

### 0B. Prepare sieve extension infrastructure locally

Extend `init_sieve_tables_host()` to support configurable sieve limits. The sieve table sizes (`SPLIT_PRIMES_COUNT`, `INERT_PRIMES_COUNT`) are currently compile-time constants. For the sweep, make them runtime-configurable (or build multiple binaries).

### 0C. Pre-test on Jetson (optional, time permitting)

If Jetson is accessible, validate correctness of the shared memory K1 patch on sm_87 before deploying to vast.ai. The smem approach should work identically on all architectures.

### 0D. Package for deployment

```bash
# From tiles-maxxing/
rsync -avz --delete \
    --exclude 'build/' --exclude '.git' --exclude 'profiling/' \
    -e "ssh -o StrictHostKeyChecking=accept-new -p $PORT" \
    tile_cuda_multi_kernel/ \
    root@$HOST:/root/tile_cuda_multi_kernel/
```

---

## Phase 1: K1 Sieve — Constant Memory to Shared Memory [~25 min]

### 1.1 Problem Analysis

K1's sieve loop (`sieve_row_k1()`, `kernel_sieve.cu` lines 23-58) iterates over two constant memory arrays:

- `c_split_barrett[609]` — 4,872 bytes, accessed sequentially per row (line 32: `const SplitPrimeBarrettGPU entry = c_split_barrett[k]`)
- `c_inert_barrett[619]` — 4,952 bytes, accessed sequentially per row (line 49: `const InertPrimeBarrettGPU entry = c_inert_barrett[k]`)

All 288 threads in a block read the same `c_split_barrett[k]` on the same iteration — this is the ideal broadcast pattern for constant memory. **But:** at 128 SMs with 5 blocks/SM = 640 concurrent blocks, 640 blocks may be at different loop iterations, reading different table entries simultaneously. The L1 constant cache (8 KB per SM) can hold the entire 10 KB table, but with 5 blocks at different `k` values, the working set per SM is 5 distinct entries = 40 bytes — well within cache. The real bottleneck is that all SMs hit the same L2 constant cache lines simultaneously with different addresses, creating L2 contention.

**Why shared memory fixes this:** Each SM loads the table once into its own shared memory. No L2 traffic after the initial load. Each SM becomes self-sufficient.

### 1.2 Implementation Approach — Primary: Shared Memory

**Shared memory cost:** 9,824 bytes per block.

**Budget check on 4090:**
- Total smem per SM (opt-in): 100 KB (102,400 bytes)
- K1 currently uses 0 dynamic smem, 4 bytes static smem (the `__shared__ uint32_t total_cands`)
- With sieve tables: 9,824 + 4 = 9,828 bytes per block
- At 5 blocks/SM: 49,140 bytes — well within 100 KB
- Occupancy impact: **none** — 5 blocks/SM still fits

**Code changes to `kernel_sieve.cu`:**

1. **Add shared memory arrays** inside `kernel_sieve()`:

```cuda
__shared__ SplitPrimeBarrettGPU s_split_barrett[SPLIT_PRIMES_COUNT];  // 4,872 B
__shared__ InertPrimeBarrettGPU s_inert_barrett[INERT_PRIMES_COUNT];  // 4,952 B
__shared__ uint32_t total_cands;
```

2. **Cooperative load at kernel start** (before the sieve loop, after `__syncthreads` for total_cands):

```cuda
// Load split table cooperatively: 609 entries, 288 threads -> 3 iterations
for (int i = tid; i < SPLIT_PRIMES_COUNT; i += BLOCK_THREADS) {
    s_split_barrett[i] = c_split_barrett[i];  // or from global memory pointer
}
// Load inert table cooperatively: 619 entries, 288 threads -> 3 iterations
for (int i = tid; i < INERT_PRIMES_COUNT; i += BLOCK_THREADS) {
    s_inert_barrett[i] = c_inert_barrett[i];  // or from global memory pointer
}
__syncthreads();
```

3. **Replace all `c_split_barrett[k]` with `s_split_barrett[k]`** in `sieve_row_k1()`.
   - Line 32: `c_split_barrett[k]` -> `s_split_barrett[k]`
   - Line 49: `c_inert_barrett[k]` -> `s_inert_barrett[k]`
   - This requires `sieve_row_k1()` to take the shared arrays as parameters, since `__shared__` variables cannot be accessed from device functions unless passed as pointers.

4. **Signature change for `sieve_row_k1()`:**

```cuda
__device__ void sieve_row_k1(
    uint32_t ws[BITMAP_WORDS_PER_ROW],
    int32_t a, int32_t b_start,
    const SplitPrimeBarrettGPU* split_table,
    int split_count,
    const InertPrimeBarrettGPU* inert_table,
    int inert_count);
```

5. **Two paths for the sieve table source.** The tables can be loaded from either:
   - (a) Constant memory into shared memory (minimal code change — keep `cudaMemcpyToSymbol`, just copy cmem -> smem at kernel start)
   - (b) Global memory into shared memory (pass device pointers as kernel parameters, eliminate `__constant__` declarations for sieve tables)

   **Choose (b)** — it eliminates the constant memory path entirely for sieve tables, removing any residual cmem traffic. Pass `d_split_barrett` and `d_inert_barrett` as kernel parameters.

6. **New kernel signature:**

```cuda
__global__ void kernel_sieve(
    const TileCoord* __restrict__ coords,
    uint32_t* __restrict__ d_cand_list,
    uint32_t* __restrict__ d_total_cands,
    const SplitPrimeBarrettGPU* __restrict__ d_split_barrett,
    const InertPrimeBarrettGPU* __restrict__ d_inert_barrett,
    int num_tiles);
```

7. **Host-side changes** (`main.cu`):
   - Add `SplitPrimeBarrettGPU* d_split_barrett` and `InertPrimeBarrettGPU* d_inert_barrett` to `TileBatchDeviceMemory`
   - `cudaMalloc` + `cudaMemcpy` (H2D) instead of `cudaMemcpyToSymbol`
   - Pass new pointers to `kernel_sieve<<<>>>` in `launch_pipeline()`
   - Keep `upload_sieve_tables()` for the cmem path as fallback (conditional compilation with `#ifdef USE_SMEM_SIEVE`)

8. **gpu_math.cuh changes:** The `mark_residue_class_barrett()` and `barrett_euclidean_mod()` functions don't read cmem directly — they only take parameters. No changes needed there. The cmem reads are only in `sieve_row_k1()`.

### 1.3 Alternative: `__ldg()` with Global Memory (Fallback)

If shared memory causes unexpected occupancy issues (e.g., interaction with the register file that we cannot predict without ncu):

- Keep tables in global memory (`__device__` or kernel parameter pointer)
- Access via `__ldg()` for read-only L2 caching
- Pros: zero shared memory cost, simpler code change
- Cons: still hits L2 (shared by all SMs), may not fully eliminate contention
- Implementation: change `c_split_barrett[k]` to `__ldg(&d_split_barrett[k])`

**Decision:** Implement shared memory as primary. If regression, fall back to `__ldg()`. Both can be toggled with a `#define`.

### 1.4 Verification Gate

1. **Byte-identical output:** Run `test` mode (2 canonical tiles). Compare `prime_count` and `tileop[0..3]` bytes against known-good values:
   - `(600000000, 600000000)` -> `prime_count=1978` (per `vast-ai/README.md` note: these are the current reference values from the tuning sweep)
   - `(699999744, 400000000)` -> `prime_count=2057`
   
   If values differ from what the current binary produces, capture both old and new, compare. Any difference = bug.

2. **Benchmark:** Run `./tile_kernel_multi 10000` three times. Capture per-kernel timing. K1 must be faster. If K1 regresses, stop and diagnose.

3. **Per-kernel timing comparison:**
   - K1 Sieve: expect **11.0-12.0 ms** (down from 14.40 ms = ~17-24% reduction)
   - K2-K5: expect **no change** (within measurement noise)
   - Total: expect **62-63 ms** (down from 65.09 ms)
   - tiles/s: expect **~159,000-161,000** (up from 155,452)

### 1.5 Expected Improvement

K1 currently takes 14.40 ms at 10K tiles. The 3090->4090 scaling was 1.20x instead of 1.56x. If the cmem fix restores linear scaling, K1 should be:

- **Ideal:** K1_3090 / 1.56 = (K1_3090_time) / 1.56. We don't have raw 3090 K1 time, but we can estimate. At 3090 66,800 tiles/s and K1 = ~16% of pipeline (from Jetson proportions), K1_3090 ~ 2.4 ms for 2K tiles, so ~12 ms for 10K. On 4090 with full 1.56x scaling: ~7.7 ms. Current: 14.40 ms. 
- **Realistic estimate:** Constant memory contention explains part of the gap, not all of it. INT32 pipe sharing on Ada also contributes. Expect K1 to drop to **10-12 ms** — a 17-31% reduction in K1 time, or **2.4-4.4 ms saved** off the 65.09 ms total.
- **Pipeline impact:** K1 is 22.1% of pipeline. A 20% K1 speedup = ~4.4% total = ~162K tiles/s. A 30% K1 speedup = ~6.6% total = ~166K tiles/s.

---

## Phase 2: Sieve Extension — 10K to 50K+ Primes [~35 min]

### 2.1 Current Sieve Architecture

**Sieve generation** (`main.cu`, `init_sieve_tables_host()`, lines 97-146):

```
HOST_SIEVE_LIMIT = 10,000 (line 70)
```

Eratosthenes sieve on host generates all primes up to 10,000. For each prime p:
- If p = 1 (mod 4): **split prime**. Compute sqrt(-1) mod p, store as `SplitPrimeBarrettGPU{p, root, mu}`. These have two residue classes to sieve.
- If p = 3 (mod 4): **inert prime**. Store as `InertPrimeBarrettGPU{p, 0, mu}`. These only sieve the zero residue class (when a = 0 mod p).

**Current counts** (`gpu_constants.cuh` lines 13-14):
```
SPLIT_PRIMES_COUNT = 609   (p = 1 mod 4, up to 10K)
INERT_PRIMES_COUNT = 619   (p = 3 mod 4, up to 10K)
```

**K1 sieve loop:** For each of the 271 active rows (one per thread), iterate over all 609 split primes and 619 inert primes. Each iteration does Barrett modular reduction + bitmap marking.

### 2.2 Candidate Reduction Estimates

The sieve eliminates composites. After sieving, the surviving candidates go to K2 for Miller-Rabin. The number of candidates directly determines K2 cost.

**Current candidate count:** From census data (AGENTS.md), mean ~5,686 candidates per tile at operating radii. The tile has 271 rows x 271 columns = 73,441 lattice points. After parity filter, ~36,720 survive. After sieve to 10K: ~5,686 survive = **84.5% elimination rate** from the parity-filtered set.

**Prime density by sieve limit** (using Mertens' theorem approximation):

The fraction of integers surviving a sieve to limit L is approximately:
```
e^(-gamma) / ln(L)
```
where gamma = 0.5772... (Euler-Mascheroni). But for Gaussian integers, the density is different because split primes remove 2 residue classes each. A more precise model:

For split primes (p = 1 mod 4), each prime p removes a fraction ~2/p of candidates.
For inert primes (p = 3 mod 4), each prime p removes a fraction ~1/p of candidates (only the a = 0 mod p class).

The product of survival probabilities:
```
Survivors ~ N * product_{p split, p <= L} (1 - 2/p) * product_{p inert, p <= L} (1 - 1/p)
```

**Estimated candidates per tile by sieve limit:**

| Sieve Limit | Split Count | Inert Count | Est. Candidates | K2 Reduction | K2 Time (est.) |
|-------------|-------------|-------------|-----------------|--------------|----------------|
| 10,000 (current) | 609 | 619 | ~5,686 | baseline | 36.07 ms |
| 20,000 | ~1,127 | ~1,130 | ~4,800 | ~15.6% | ~30.4 ms |
| 30,000 | ~1,614 | ~1,617 | ~4,350 | ~23.5% | ~27.6 ms |
| 50,000 | ~2,553 | ~2,556 | ~3,800 | ~33.1% | ~24.1 ms |
| 100,000 | ~4,781 | ~4,783 | ~3,200 | ~43.7% | ~20.3 ms |

**These are rough estimates.** The actual candidate reduction depends on the specific norm distribution at operating radii. The sweep will measure the real numbers.

### 2.3 K1 Cost Increase

More sieve primes = more K1 work per tile. The sieve loop iterates over every prime, so K1 time scales linearly with prime count (for split primes — inert primes are cheaper because they only check `a mod p == 0`).

**K1 cost model:**
- Current K1: 14.40 ms for 609 split + 619 inert = 1,228 total primes
- Split primes dominate cost (2 residue classes + root computation per prime)
- Rough: K1 cost ~ a * split_count + b * inert_count
- With shared memory fix (Phase 1), K1 baseline drops to ~11 ms

**Estimated K1 times after smem fix:**

| Sieve Limit | Split | Inert | K1 Time (est.) | K1 Delta |
|-------------|-------|-------|----------------|----------|
| 10,000 | 609 | 619 | ~11.0 ms | baseline |
| 20,000 | 1,127 | 1,130 | ~20.3 ms | +9.3 ms |
| 30,000 | 1,614 | 1,617 | ~29.1 ms | +18.1 ms |
| 50,000 | 2,553 | 2,556 | ~46.1 ms | +35.1 ms |
| 100,000 | 4,781 | 4,783 | ~86.4 ms | +75.4 ms |

### 2.4 Net Pipeline Impact Estimate

Combining K1 cost increase with K2 reduction (K3-K5 unchanged):

| Sieve Limit | K1 (ms) | K2 (ms) | K3+K4+K5 (ms) | Total (ms) | tiles/s (10K) |
|-------------|---------|---------|----------------|------------|---------------|
| 10K (smem) | 11.0 | 36.1 | 14.7 | 61.8 | ~162,000 |
| 20K | 20.3 | 30.4 | 14.7 | 65.4 | ~153,000 |
| 30K | 29.1 | 27.6 | 14.7 | 71.4 | ~140,000 |
| 50K | 46.1 | 24.1 | 14.7 | 84.9 | ~118,000 |
| 100K | 86.4 | 20.3 | 14.7 | 121.4 | ~82,000 |

**Predicted optimal: 10K-15K sieve limit.** Beyond ~15K, K1 cost growth outpaces K2 savings. The crossover where sieve extension hurts is around 20K.

**However:** This model assumes K1 scales linearly with prime count. In practice:
- The Barrett reduction loop in K1 has excellent ILP (independent iterations)
- SM utilization may be sublinear at higher prime counts (more register pressure)
- The shared memory table will grow: at 50K primes, the split table alone is ~20 KB — shared memory starts to bite occupancy

### 2.5 Implementation

**Two approaches to handle variable sieve limits:**

**(A) Compile-time constants (multiple builds):**
Change `SPLIT_PRIMES_COUNT` and `INERT_PRIMES_COUNT` in `gpu_constants.cuh`, rebuild for each sieve limit. Clean but requires 5 separate builds.

**(B) Runtime-configurable (single build, max allocation):**
Set `SPLIT_PRIMES_COUNT` and `INERT_PRIMES_COUNT` to the maximum needed (for 100K: ~4,781 split, ~4,783 inert). Pass the actual count as a kernel parameter. The sieve loop runs `k < actual_count` instead of `k < SPLIT_PRIMES_COUNT`.

**Choose (B)** — faster iteration during the sweep. One build, multiple runs.

**Code changes:**

1. **`gpu_constants.cuh`:** Change to maximum counts:
```cuda
constexpr int MAX_SPLIT_PRIMES = 5000;   // enough for sieve to 100K
constexpr int MAX_INERT_PRIMES = 5000;
constexpr uint32_t MAX_SIEVE_LIMIT = 100000U;
```

2. **`gpu_types.cuh`:** Update `SieveTablesBarrett` to use max size:
```cuda
struct SieveTablesBarrett {
    SplitPrimeBarrettGPU split_table[MAX_SPLIT_PRIMES];
    InertPrimeBarrettGPU inert_primes[MAX_INERT_PRIMES];
    int split_count;
    int inert_count;
};
```

3. **`kernel_sieve.cu`:** Pass actual counts to kernel:
```cuda
__global__ void kernel_sieve(
    const TileCoord* __restrict__ coords,
    uint32_t* __restrict__ d_cand_list,
    uint32_t* __restrict__ d_total_cands,
    const SplitPrimeBarrettGPU* __restrict__ d_split_barrett,
    int split_count,
    const InertPrimeBarrettGPU* __restrict__ d_inert_barrett,
    int inert_count,
    int num_tiles);
```

And in `sieve_row_k1()`, replace `SPLIT_PRIMES_COUNT` with `split_count` parameter, `INERT_PRIMES_COUNT` with `inert_count`.

4. **Shared memory for variable-size tables:** At higher sieve limits, the table may not fit in shared memory.
   - 10K: 9,824 B -> fits in smem (5 blocks/SM OK)
   - 20K: ~18 KB -> fits (5 blocks/SM at 90 KB total still under 100 KB)
   - 30K: ~26 KB -> fits (5 blocks at 130 KB > 100 KB limit). **Only 3 blocks/SM.** Occupancy drops to 56.3%.
   - 50K: ~41 KB -> fits (2 blocks/SM). Occupancy drops to 37.5%.
   - 100K: ~77 KB -> barely fits 1 block. Occupancy = 18.8%. **Terrible.**

   **Hybrid approach:** For sieve limits where smem table > 20 KB, fall back to global memory with `__ldg()`. Only use smem for the first N primes that fit.

   **Simpler approach:** Use global memory + `__ldg()` for the extended primes (beyond 10K). Keep the original 10K table in shared memory. The extended primes are a separate kernel parameter pointer. Two loops in `sieve_row_k1()`: first loop over smem (fast, 10K primes), second loop over global memory (larger primes via `__ldg`).

5. **`main.cu`:** Make sieve limit a command-line parameter:
```bash
./tile_kernel_multi sweep 10000 10000    # sieve_limit=10000, tiles=10000
./tile_kernel_multi sweep 20000 10000    # sieve_limit=20000, tiles=10000
./tile_kernel_multi sweep 50000 10000    # etc.
```

6. **`init_sieve_tables_host()`:** Parameterize by `uint32_t sieve_limit`:
```cpp
bool init_sieve_tables_host(SieveTablesBarrett& tables, uint32_t sieve_limit) {
    // Eratosthenes up to sieve_limit
    // ...
    // Return false if counts exceed MAX_SPLIT_PRIMES or MAX_INERT_PRIMES
}
```

**Potential issue:** `SplitPrimeBarrettGPU` uses `uint16_t p` — max value 65,535. For sieve limits up to 100K, the primes up to 65,535 fit. Primes from 65,536 to 100,000 need `uint32_t p`. **Solution:** Use a separate struct `SplitPrimeBarrettGPU32` with `uint32_t p` for extended primes, or change the existing struct to use `uint32_t p` (increases table size by 2 bytes per entry, negligible).

**Actually:** Checking `gpu_types.cuh` line 65-70: `SplitPrimeBarrettGPU` has `uint16_t p` and `uint16_t root`. For primes > 65,535, we need 32-bit fields. Since the vast majority of sieve work is for primes < 65,536 (all primes up to 65K), and there are only ~830 primes between 65,536 and 100,000, a two-table approach works:
- Original table (uint16_t fields) for primes up to 65,535
- Extended table (uint32_t fields) for primes 65,536-100,000

For the initial sweep up to 50K, this is not an issue — all primes fit in uint16_t.

### 2.6 Sweep Protocol

Run on the 4090 instance, in tmux:

```bash
# All sweeps at 10K tiles for stable measurement
for LIMIT in 10000 12000 15000 20000 30000 50000; do
    echo "=== SIEVE LIMIT: $LIMIT ==="
    ./tile_kernel_multi sweep $LIMIT 10000 2>&1 | tee /root/sweep-$LIMIT.txt
    echo ""
done
```

**Capture for each sweep point:**
- Sieve limit
- Actual split/inert prime counts
- Candidates per tile (mean, min, max — need to add candidate count reporting)
- K1 Sieve time (ms)
- K2 MR time (ms)
- K3+K4+K5 time (ms)
- Total time (ms)
- tiles/s
- Correctness check (overflow sentinel count, spot-check prime counts)

**Adding candidate count reporting:** Modify `run_bench()` to download `d_total_cands` and print statistics:
```cuda
std::vector<uint32_t> cand_counts(tile_count);
cudaMemcpy(cand_counts.data(), mem.buf.d_total_cands,
           sizeof(uint32_t) * tile_count, cudaMemcpyDeviceToHost);
// Print mean, min, max, p99
```

### 2.7 Verification Gate

1. **Correctness at each sweep point:** Run `test` mode with the extended sieve. Prime counts must match the baseline exactly — sieve extension removes composites, but never primes. If prime counts differ, the sieve is incorrectly marking primes as composite (bug in Barrett reduction for larger primes, or uint16 overflow).

2. **Monotonicity check:** As sieve limit increases, candidate count must monotonically decrease. If candidates ever increase, something is wrong.

3. **Candidate count sanity:** At 50K sieve limit, candidates should be ~33% fewer than at 10K. If reduction is much less, the extended primes aren't covering the norm range properly (possible: large primes have fewer multiples in the tile, so marginal returns diminish — this is expected but the reduction should still be monotonic).

### 2.8 Expected Crossover

Based on the cost model in 2.4, the optimal sieve limit is likely **10K-15K**. The sweep will reveal the true crossover. If the crossover is at 10K (i.e., any extension hurts), then the sieve is already optimal and Phase 2 is a confirmed dead end — valuable information.

**Key insight:** The crossover depends on the K1 smem fix from Phase 1. A faster K1 raises the crossover point (more sieve work becomes affordable before K2 savings are exhausted). Run Phase 1 first, then Phase 2.

---

## Phase 3: Re-Profiling [~15 min]

### 3.1 nsys Timeline

Profile the optimized pipeline at the best sieve limit from Phase 2:

```bash
nsys profile \
    -o /root/profile-optimized \
    --force-overwrite true \
    ./tile_kernel_multi 10000
```

**What to capture:**
- Per-kernel duration comparison vs baseline (14.40 / 36.07 / 0.47 / 12.44 / 1.75 ms)
- K1 shared memory activity in the timeline (should see smem load phase at kernel start)
- Inter-kernel gaps (should remain 2-3 us)
- Any new bottlenecks introduced

### 3.2 ncu Attempt

**Option A:** Try a vast.ai image that provides `CAP_SYS_ADMIN`:
```bash
# Search for bare-metal or privileged instances
vastai search offers 'gpu_name=RTX_4090 cuda_vers>=12.0 disk_space>=20 num_gpus=1 dph<=0.40 machine_type=bare-metal' -o 'dph'
```

**Option B:** If no bare-metal available, try `--privileged` Docker flag:
```bash
vastai create instance $OFFER_ID --image pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel --disk 25 --ssh --env '-e NVIDIA_DISABLE_REQUIRE=1' --onstart-cmd 'apt-get update && apt-get install -y tmux'
```

**Option C:** Use ncu on a 3090 instance (may be cheaper / more available as bare-metal).

**ncu command if available:**
```bash
ncu --set full \
    --target-processes all \
    --kernel-name kernel_sieve \
    --launch-count 1 \
    -o /root/ncu-k1-optimized \
    ./tile_kernel_multi test

ncu --set full \
    --kernel-name kernel_mr \
    --launch-count 1 \
    -o /root/ncu-k2 \
    ./tile_kernel_multi test
```

**Key ncu metrics to look for:**
- `sm__throughput.avg.pct_of_peak_sustained_elapsed` — overall SM utilization
- `l1tex__throughput.avg.pct_of_peak_sustained_elapsed` — L1 throughput for K1 (should decrease with smem fix)
- `smsp__inst_executed_pipe_lsu.avg.pct_of_peak_sustained_elapsed` — LSU pipe utilization
- `smsp__warps_launched.avg.per_cycle_active` — warp launch rate
- Memory throughput breakdown: smem vs L1 vs L2 vs DRAM

### 3.3 Per-Kernel Timing Breakdown

Run `./tile_kernel_multi 10000` three times at the optimized configuration. Record:

```
| Kernel | Baseline (ms) | Optimized (ms) | Delta |
|--------|--------------|----------------|-------|
| K1     | 14.40        | ???            | ???   |
| K2     | 36.07        | ???            | ???   |
| K3     | 0.47         | ???            | ???   |
| K4     | 12.44        | ???            | ???   |
| K5     | 1.75         | ???            | ???   |
| Total  | 65.09        | ???            | ???   |
| tiles/s| 155,452      | ???            | ???   |
```

### 3.4 Copy Profiling Artifacts Back

```bash
scp -P $PORT root@$HOST:/root/profile-optimized.nsys-rep \
    ./profiling/4090/
scp -P $PORT root@$HOST:/root/sweep-*.txt \
    ./profiling/4090/
# ncu files if available
scp -P $PORT root@$HOST:/root/ncu-*.ncu-rep \
    ./profiling/4090/
```

---

## Phase 4: Documentation and Cleanup [~5 min]

- Write results document to `docs/supportive/2026-04-11-smem-sieve-extension.md`
- Update `AGENTS.md` Performance Record with new best
- Update `AGENTS.md` Operational Learnings with findings
- Commit all code changes + results
- **DESTROY instance**

---

## Session Timeline

| Phase | Task | Time (est.) | Cumulative |
|-------|------|-------------|------------|
| 0 | Pre-session (Mac Mini) | 30-60 min | 0 |
| — | Rent 4090, rsync code | 5 min | 5 min |
| 1.1 | Build with smem patch | 3 min | 8 min |
| 1.2 | Verify correctness (test mode) | 2 min | 10 min |
| 1.3 | Benchmark K1 smem (10K tiles, 3 runs) | 5 min | 15 min |
| 1.4 | If regression, try `__ldg()` fallback | 5 min | 20 min |
| 2.1 | Build sweep binary | 3 min | 23 min |
| 2.2 | Verify correctness at each sweep point | 5 min | 28 min |
| 2.3 | Run sieve sweep (6 points, 3 runs each) | 15 min | 43 min |
| 3.1 | nsys profile optimized pipeline | 5 min | 48 min |
| 3.2 | ncu attempt (optional, may skip) | 5 min | 53 min |
| 3.3 | Copy artifacts, document, cleanup | 5 min | 58 min |
| — | DESTROY instance | 1 min | 59 min |
| **Total cloud time** | | **~60 min** | |

---

## Rollback Plan

### If K1 smem patch hurts performance:
1. The original kernel_sieve.cu is in git. `git checkout -- tile_cuda_multi_kernel/src/kernel_sieve.cu`
2. Alternatively, use `#ifdef USE_SMEM_SIEVE` to toggle. Build without the flag to get original constant memory path.
3. Fall back to `__ldg()` approach (global memory, no smem, compiler-cached reads).

### If sieve extension hurts at all sweep points:
1. Confirms 10K is already optimal. Revert `gpu_constants.cuh` to fixed counts.
2. The sweep itself is non-destructive — it just runs with different parameters. No code needs reverting if the sweep binary supports fallback to 10K.

### If correctness breaks:
1. Run `test` mode with 2 canonical tiles. Compare prime_count.
2. If smem patch changes output: Barrett reduction in smem reads may have alignment issues. Check that shared memory loads are `sizeof(SplitPrimeBarrettGPU)` = 8 bytes aligned.
3. If sieve extension changes output at the same sieve limit: the extended sieve is marking primes as composite. Debug by comparing candidate lists before/after extension.

### If instance dies mid-session:
1. All code is local (Mac Mini). No work is lost.
2. Rent a new instance, rsync, continue from last completed phase.
3. Phase 1 and Phase 2 are independent — either can be done alone.

---

## Expected Outcomes

### Optimistic (both optimizations work)
- K1: 11 ms (smem fix) -> 14 ms (with 15K sieve extension)
- K2: 36 ms -> ~30 ms (15K sieve reduces candidates ~15%)
- Total: ~59 ms -> **~170K tiles/s** (+10% over baseline)

### Realistic (smem works, sieve extension marginal)
- K1: 11 ms (smem fix, keep 10K sieve)
- K2: 36 ms (unchanged — sieve extension crossover at 10K)
- Total: ~62 ms -> **~162K tiles/s** (+4% over baseline)

### Conservative (smem gives partial improvement, extension is dead end)
- K1: 12.5 ms (partial cmem fix)
- K2: 36 ms (unchanged)
- Total: ~63.5 ms -> **~157K tiles/s** (+1% over baseline)

### Pessimistic (both optimizations fail)
- K1: 14.4 ms (no improvement — issue is INT32 pipe, not cmem)
- Total: 65 ms -> **155K tiles/s** (unchanged)
- Value: confirmed that the pipeline is at hardware limits. Valuable negative result.

---

## Cost Estimate

- **Instance type:** RTX 4090, vast.ai
- **Hourly rate:** ~$0.27/hr
- **Estimated session:** 60 minutes
- **Estimated cost:** ~$0.27
- **Maximum budget:** $0.50 (if ncu requires a second instance or bare-metal)

Previous sessions: $0.14 (tuning sweep), $0.09 (profiling), $0.09 (3090 baseline) = $0.32 total. This session would bring cumulative to ~$0.60.

---

## Risk Register

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| K1 smem patch changes output (correctness bug) | Low | High | Test with canonical tiles before any benchmarking. Shared memory alignment is 8-byte for the Barrett structs. |
| Sieve extension causes uint16 overflow for primes > 65535 | Medium (if sweeping to 100K) | Medium | Cap sweep at 50K. All primes < 50K fit in uint16_t (max prime < 49,999). |
| Smem occupancy interaction with K1 register file | Low | Medium | K1 uses 30 regs. At 5 blocks/SM: 30*288*5 = 43,200 regs + 5*10KB smem = 50KB smem. Both within limits (65K regs, 100K smem). |
| Sieve extension exhausts MAX_CANDIDATES_GPU (6144) | Very Low | Low | Extended sieve *reduces* candidates. Cannot increase them. |
| Instance lacks nvcc or has wrong CUDA version | Low | Medium | Use known-good image `pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel`. |
| SSH drop during sweep | Medium | Low | All commands in tmux. Reconnect and check tmux output. |
| ncu still blocked (no CAP_SYS_ADMIN) | High | Low | ncu is nice-to-have, not blocking. nsys + per-kernel timing are sufficient. |
| Sieve extension changes K4 behavior (fewer primes -> different UF topology) | Very Low | Low | K4 time should decrease or stay flat with fewer primes. Monitor. |
| Clock throttling skews measurements | Low | Medium | Run 3 iterations of each measurement. Use 10K tiles (longer runs smooth out transients). Cannot lock clocks without sudo on vast.ai. |
| Shared memory bank conflicts in cooperative load | Very Low | Low | Sequential 8-byte loads across threads — no bank conflicts (8B stride maps to consecutive banks on sm_89). |

---

## Appendix: Exact Code Locations Reference

| Item | File | Lines | Notes |
|------|------|-------|-------|
| Constant memory declarations | `src/kernel_sieve.cu` | 14-19 | `c_split_barrett`, `c_inert_barrett`, etc. |
| Sieve row function | `src/kernel_sieve.cu` | 23-58 | `sieve_row_k1()` — reads cmem tables |
| Sieve kernel | `src/kernel_sieve.cu` | 116-163 | `kernel_sieve()` — one block per tile |
| Upload sieve tables | `src/kernel_sieve.cu` | 176-183 | `upload_sieve_tables()` — cudaMemcpyToSymbol |
| Sieve limit | `src/main.cu` | 70 | `HOST_SIEVE_LIMIT = 10000U` |
| Host sieve init | `src/main.cu` | 97-146 | `init_sieve_tables_host()` |
| Split/inert counts | `include/gpu_constants.cuh` | 13-14 | `SPLIT_PRIMES_COUNT=609`, `INERT_PRIMES_COUNT=619` |
| Barrett structs | `include/gpu_types.cuh` | 65-77 | `SplitPrimeBarrettGPU` (8B), `InertPrimeBarrettGPU` (8B) |
| Barrett math | `include/gpu_math.cuh` | 23-50 | `barrett_mod_u32()`, `mark_residue_class_barrett()` |
| Extern cmem declarations | `include/gpu_math.cuh` | 7-12 | Used by K2/K4/K5 for MR witnesses etc. |
| MR kernel | `src/kernel_mr.cu` | 21-71 | `kernel_mr()` — reads from d_cand_list |
| Pipeline launcher | `src/main.cu` | 287-360 | `launch_pipeline()` — kernel launch sequence |
| Kernel signatures in main | `src/main.cu` | 22-57 | Forward declarations — must update for new K1 params |
| Makefile arch flag | `Makefile` | 10 | `BASE_FLAGS := -arch=sm_87` — change to sm_89 on instance |
| Per-kernel reg caps | `Makefile` | 15-19 | K1=40, K2=44, K3=32, K4=40, K5=40 |
| MAX_CANDIDATES_GPU | `include/gpu_constants.cuh` | 54 | 6144 — may need increase if sieve reduces less than expected (unlikely) |
| TileBatchDeviceMemory | `src/main.cu` | 237-276 | Device memory allocation — add sieve table pointers |
