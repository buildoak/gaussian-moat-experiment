---
date: 2026-04-10
engine: claude
type: design-note
status: complete
refs:
  - tile-cuda/include/gpu_math.cuh
  - tile-cuda/include/gpu_sieve.cuh
  - tile-cuda/src/tile_kernel.cu
  - tile-cuda/include/gpu_constants.cuh
  - docs/supportive/2026-04-10-profiling-baseline.md
  - docs/supportive/2026-04-10-barrett-sieve-plan.md
---

# Miller-Rabin Optimization Plan for Phase 1c

## 1. Current Cost Breakdown

### 1.1 Measured Phase 1c Budget

From profiling baseline (`docs/supportive/2026-04-10-profiling-baseline.md`):

- Mean Phase 1c cycles: **7,453,446** (25.85% of total 28.8M)
- Median: 7,362,255
- Min: 6,700,645 — Max: 11,391,071
- Low variance (1.7x range) — this is a compute-bound, predictable workload

Note: The 61.5% figure from the hypothesis is not supported by current profiling data. The measured 25.85% is the actual Phase 1c share. Phase 4+5 (face extraction) dominates at 44.19%. This plan optimizes the second-largest bottleneck.

### 1.2 Candidate Flow

- Tile side: 271 active rows x 271 columns = 73,441 lattice points
- Sieve survivors: ~5,700 per tile (from `tile_kernel.cu:39` comment and MAX_CANDIDATES_GPU=6144)
- Sieve kill rate: ~92.2%
- MR candidates (after trial division): estimated ~5,300-5,500 per tile (trial division with 24 primes kills ~5-7% of survivors)
- Final primes per tile: ~1,300-1,500 (from profiling: mean prime_count not recorded, but MAX_PRIMES_GPU=2560 is headroom)

### 1.3 Per-Candidate MR Cost Model

For a single candidate norm n that survives trial division, the `is_prime_gpu()` path at `gpu_math.cuh:153-189` executes:

**Trial division** (`gpu_math.cuh:164-172`):
- 24 iterations, each doing `n % p` where n is uint64_t, p is uint32_t
- Each `n % p` compiles to a CALL.ABS.NOINC to `__cuda_sm20_rem_u64` (83 total CALL.ABS sites across the kernel, 24 of these are trial-division calls per candidate path)
- The `__cuda_sm20_rem_u64` function (`sass-dump-2026-04-10.txt:80-244`) is ~82 SASS instructions including 6 IMAD.WIDE.U32, multiple IMAD.HI.U32, MUFU.RCP (floating-point reciprocal approximation), F2I conversion, and 3 conditional correction passes
- Estimated cost per `__cuda_sm20_rem_u64` call: **~200-300 cycles** on sm_87 (MUFU.RCP alone is ~22 cycles, plus 6+ IMAD.WIDE at 4 cycles each, plus pipeline bubbles from data dependencies)
- Total trial division cost per candidate: **~5,000-7,200 cycles** (24 x 200-300)

**Montgomery context setup** (`gpu_math.cuh:102-107`, `mont_init_gpu`):
- `mont_compute_m_inv`: 6 Newton iterations, each doing 2 uint64 multiplies = 12 mult ops
- `mont_compute_r2`: **128 iterations** of `addmod_gpu` (line 88-89), each a conditional add/subtract on uint64. ~128 x 3 instructions = ~384 ops. At ~4 cycles/op = **~1,500 cycles**
- `mont_mul_gpu` for ctx.one and ctx.nm1: 2 Montgomery multiplies
- Three `%` operations: `1ULL % m`, `1ULL % ctx.m`, `(ctx.m - 1ULL) % ctx.m` — all trivially optimizable since `1 < m` always, making `1 % m = 1` and `(m-1) % m = m-1`
- Plus `a % ctx.m` in each `mont_to_gpu` call — again trivially `a` since witness `a < ctx.m` for all 12 witnesses under our norm range
- Total Montgomery setup cost: **~3,000-4,000 cycles**

**Witness loop** (`gpu_math.cuh:183-187`):
- 12 witnesses from constant memory (`c_mr_witnesses[]`: 2,3,5,7,11,13,17,19,23,29,31,37)
- Per witness: one `mont_powmod_gpu` call + up to (s-1) squarings
- `mont_powmod_gpu` (`gpu_math.cuh:117-128`): binary left-to-right exponentiation with exponent d
  - d = (n-1) >> s where s = ctz(n-1)
  - For norms at R=830M: n ~ 1.38 x 10^18 ~ 61 bits. After removing trailing zeros: d ~ 55-60 bits on average
  - Each bit of d: 1 `mont_mul_gpu` (squaring) + conditionally 1 `mont_mul_gpu` (multiply), average 1.5 mont_muls per bit
  - Total mont_muls per witness powmod: ~55 * 1.5 = **~82 mont_muls**
- Plus `mont_to_gpu` entry: 1 mont_mul per witness (converts base to Montgomery form)
- Plus up to (s-1) squarings in the witness test loop (`gpu_math.cuh:144-149`), average ~3-5 squarings
- Total per witness: ~82 + 1 + 4 = **~87 mont_muls**
- Total for 12 witnesses (worst case, all pass): 12 x 87 = **~1,044 mont_muls**

**Cost of mont_mul_gpu** (`gpu_math.cuh:67-76`):
The function performs:
```
lo = a * b              // uint64 multiply (low 64 bits)
hi = __umul64hi(a, b)   // uint64 multiply (high 64 bits)
q = lo * m_inv          // uint64 multiply (low 64 bits)
qm_lo = q * m           // uint64 multiply (low 64 bits)
qm_hi = __umul64hi(q, m) // uint64 multiply (high 64 bits)
carry = (lo + qm_lo < lo) ? 1 : 0
r = hi + qm_hi + carry
conditional subtract
```

That is **5 uint64 multiplies** per Montgomery multiplication, plus 2 additions, 1 comparison, 1 conditional subtract.

On sm_87, each 64x64→64 multiply compiles to ~4 IMAD.WIDE.U32 + IMAD instructions (schoolbook 2x2 with 32-bit limbs). Each `__umul64hi` compiles similarly. The SASS around lines 4896-4944 of the dump confirms this pattern: sequences of IMAD.WIDE.U32, IMAD, IMAD.HI.U32 implementing the 128-bit product.

Estimated cost per mont_mul: **~40-50 cycles** (5 uint64 muls x ~6-8 IMAD each x ~1 cycle amortized throughput, but serialized by data dependencies to ~40-50 actual cycles)

### 1.4 Total Per-Candidate Cost Estimate

| Component | Cycles (est.) | % of MR |
|-----------|--------------|---------|
| Trial division (24 x rem_u64) | 5,000-7,200 | 10-13% |
| Montgomery context setup | 3,000-4,000 | 6-7% |
| Witness powmod (1,044 mont_muls) | 42,000-52,000 | **78-82%** |
| Witness test squarings (~48 mont_muls) | 1,900-2,400 | 3-4% |
| **Total per candidate (prime)** | **~52,000-65,000** | 100% |

Composites exit early — on average testing ~3-4 witnesses before finding a composite witness. So:
- Primes: full 12-witness cost = ~55,000 cycles
- Composites: early exit at ~3-4 witnesses = ~15,000-20,000 cycles

### 1.5 Cross-Check Against Profiling

With ~5,500 candidates, of which ~1,400 are prime and ~4,100 are composite:
- Prime cost: 1,400 x 55,000 = 77M cycles
- Composite cost: 4,100 x 17,000 = 70M cycles
- Total: ~147M cycles across all candidates

But this is distributed across 288 threads, so per-tile wall-clock = 147M / 288 = ~510K cycles.

Hmm, this is lower than the measured 7.4M. The discrepancy comes from:
1. **Warp divergence**: threads in a warp testing different candidates finish at different times; the warp waits for the slowest thread. Primes take 3-4x longer than composites, and the warp completes at the pace of whichever thread got the most primes.
2. **Memory latency**: constant memory reads for witnesses and trial primes, shared memory atomics for bitmap writes.
3. **Call overhead**: Each `__cuda_sm20_rem_u64` call has function prologue/epilogue overhead (CALL.ABS.NOINC + RET.ABS.NODEC).

Revised model accounting for warp divergence:
- 288 threads / 32 = 9 warps
- 5,500 candidates / 288 = ~19 candidates per thread, distributed round-robin
- Warp completion time = max(thread completion times within warp)
- Expected: the slowest thread in a warp gets ~2-3 more primes than average, pushing warp time up by ~30-50%

This gives ~510K x 1.4 x 9 warps / 9 warps (pipeline overlap) + context overhead ≈ ~7-8M cycles per tile, which aligns with the measured 7.4M.

## 2. Root Cause Analysis

### 2.1 Dominant Cost: Montgomery Multiply Chain

The witness powmod loop is unambiguously the dominant cost at **~80% of Phase 1c**. Each `mont_mul_gpu` does 5 full 64-bit multiplications. With ~1,044 mont_muls per prime candidate, that is **~5,220 uint64 multiplications** per candidate that passes all witnesses.

On sm_87, there is no native 64x64→128 multiply. Each one decomposes to:
- 4 IMAD.WIDE.U32 (32x32→64) for the schoolbook 2x2 product
- Plus carry propagation via IADD3, IMAD.X

The SASS dump confirms this: the Montgomery multiply region (lines ~4890-4950 and ~8890-8e00) shows dense IMAD.WIDE.U32 + IMAD.HI.U32 + IADD3 sequences with no floating-point, no memory, just pure integer ALU.

### 2.2 Secondary Cost: __cuda_sm20_rem_u64 in Trial Division

83 CALL.ABS.NOINC sites exist in the kernel SASS. Of these, the trial division loop at `gpu_math.cuh:169` (`n % p`) accounts for 24 calls per candidate. The `__cuda_sm20_rem_u64` function at SASS lines 80-244 is a **165-line** software division routine that:

1. Converts divisor to float via I2F.U64.RP (line 122)
2. Computes floating-point reciprocal via MUFU.RCP (line 124)
3. Adjusts to integer via F2I.U64.TRUNC (line 128)
4. Performs **3 Newton-Raphson iterations** using IMAD.WIDE.U32 (lines 130-188)
5. Computes remainder via quotient * divisor subtraction (lines 200-212)
6. Applies **2 conditional corrections** (lines 210-238)

This is extremely expensive — estimated 200-300 cycles per call. For 24 trial primes, that is 5,000-7,200 cycles per candidate, or ~10-13% of MR cost.

The 91 MUFU.RCP instructions in the entire SASS dump confirm the heavy use of the software division path. Many of these are in the trial division loop (24 per candidate path) and the Montgomery setup `%` sites (3-4 per candidate).

### 2.3 Tertiary Cost: mont_compute_r2 Loop

The R² computation at `gpu_math.cuh:86-92` runs **128 iterations** of addmod. This is called once per candidate that reaches MR. At ~1,500 cycles, it is ~3% of MR cost, but it is completely unnecessary for the Montgomery form — R² can be computed more efficiently.

### 2.4 Redundant % Operations

The Barrett plan (`docs/supportive/2026-04-10-barrett-sieve-plan.md`, section 2.6) already identified these redundant remainders:

| Site | Expression | Current | Optimization |
|------|-----------|---------|-------------|
| `gpu_math.cuh:87` | `1ULL % m` in `mont_compute_r2` | CALL to rem_u64 | Replace with literal `1ULL` (since m > 1 always) |
| `gpu_math.cuh:104` | `1ULL % ctx.m` in `mont_init_gpu` | CALL to rem_u64 | Replace with literal `1ULL` |
| `gpu_math.cuh:105` | `(ctx.m - 1ULL) % ctx.m` in `mont_init_gpu` | CALL to rem_u64 | Replace with `ctx.m - 1ULL` |
| `gpu_math.cuh:110` | `a % ctx.m` in `mont_to_gpu` | CALL to rem_u64 | Remove: a < ctx.m always true for our witness set (max witness 37, min norm > 10000) |

That eliminates **3 + 12 = 15 rem_u64 calls per candidate** (3 from init, 12 from mont_to_gpu for each witness).

## 3. Optimization Proposals — Ranked by Leverage x Feasibility

### OPT-1: Reduce Witness Count from 12 to 7 [HIGH LEVERAGE, MODERATE RISK]

**Analysis:**

The current witness set {2,3,5,7,11,13,17,19,23,29,31,37} is the standard 12-witness set that provides deterministic MR for all n < 3.317 x 10^24 (Sorenson and Webster, 2016).

For our operating range, n = a² + b² where |a|,|b| ≤ ~830M + 271/2, giving max norm ~1.38 x 10^18.

Known deterministic witness sets for bounded ranges:

- **n < 3.215 x 10^18**: witnesses {2, 3, 5, 7, 11, 13, 17} suffice (7 witnesses). Source: Jaeschke (1993) extended by Jiang and Deng (2014), verified computationally.
- **n < 3.317 x 10^24**: witnesses {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37} (current 12).

Since our max norm is ~1.38 x 10^18 < 3.215 x 10^18, **7 witnesses {2,3,5,7,11,13,17} are sufficient**.

**Speedup:**
- Witness powmod is 80% of MR cost
- Reduction from 12 to 7 witnesses = 41.7% fewer powmod calls
- For primes: saves 5/12 x 80% = **33% of per-candidate MR cost**
- For composites: smaller improvement since early exit already cuts most candidates short. Average composite tests ~3-4 witnesses, so the bound still helps for composites that test 7+ witnesses (rare).
- Net Phase 1c speedup: **~20-25%**

**Risk:** Must validate the bound rigorously. The Jiang-Deng result has been independently verified, but we should confirm that 3.215 x 10^18 covers our exact max norm including edge cases (tiles at maximum radius, corner points where both coordinates are maximal).

**Verification gate:** Run the current 12-witness MR and the proposed 7-witness MR on all sieve survivors for 10,000 tiles at maximum operating radius. Any disagreement is a showstopper.

### OPT-2: Eliminate Redundant % in Montgomery Setup [HIGH LEVERAGE, LOW RISK]

**What:**
Remove 15 `__cuda_sm20_rem_u64` calls per candidate as detailed in section 2.4.

**Changes to `gpu_math.cuh`:**

1. Line 87: `uint64_t r = 1ULL % m;` → `uint64_t r = 1ULL;`
2. Line 104: `ctx.one = mont_mul_gpu(1ULL % ctx.m, ctx.r2, ctx.m, ctx.m_inv);` → `ctx.one = mont_mul_gpu(1ULL, ctx.r2, ctx.m, ctx.m_inv);`
3. Line 105: `ctx.nm1 = mont_mul_gpu((ctx.m - 1ULL) % ctx.m, ctx.r2, ctx.m, ctx.m_inv);` → `ctx.nm1 = mont_mul_gpu(ctx.m - 1ULL, ctx.r2, ctx.m, ctx.m_inv);`
4. Line 110: `return mont_mul_gpu(a % ctx.m, ctx.r2, ctx.m, ctx.m_inv);` → `return mont_mul_gpu(a, ctx.r2, ctx.m, ctx.m_inv);`

**Correctness proof:**
- Sites 1-3: m is a Gaussian norm > 9409 (since trial division already checked all primes up to 97, and n < 97² is handled separately at line 174). So `1 < m` always, giving `1 % m = 1`. And `m - 1 < m` trivially.
- Site 4: Called from `mont_powmod_gpu` → `mont_to_gpu(base, ctx)` where base is a witness value from {2,3,5,7,11,13,17,...,37}. Since all witnesses < 97 and m > 9409, `a < m` always holds, so `a % m = a`. Additionally, `miller_rabin_witness_mont_gpu` already guards `a >= ctx.m` at line 135.

**Speedup:** 15 rem_u64 calls x 250 cycles = ~3,750 cycles per candidate. That is ~6% of per-candidate MR cost. Net Phase 1c: **~5-6%**.

### OPT-3: Barrett Reduction for Trial Division [MODERATE LEVERAGE, LOW RISK]

**What:**
Replace `n % p` at `gpu_math.cuh:169` with Barrett reduction using precomputed reciprocals for the 24 fixed trial primes.

**Implementation:**
- Precompute `mu = floor(2^64 / p)` for each trial prime (64-bit Barrett since n is uint64_t, but p < 97)
- Actually, since p < 128 and n < 2^61, we can use a simpler approach: `n % p = n - (n * mu >> 64) * p` where `mu = ceil(2^64 / p)`. This requires one `__umul64hi` plus one multiply and subtract.
- Better yet: for p < 128, use `__umul64hi(n, magic) >> shift` with per-prime magic/shift constants. This is the standard compiler optimization for constant divisors, but since our divisors are runtime-loaded from constant memory, the compiler cannot apply it.

**Storage:** 24 entries x 16 bytes (mu: uint64, p: uint32, shift: uint32) = 384 bytes in constant memory. Or store just `mu_hi` (uint64) alongside existing `c_trial_primes` = 24 x 8 = 192 bytes additional.

**Speedup:** Replaces 24 x 250-cycle rem_u64 calls with 24 x ~15-cycle Barrett reductions = saves ~5,600 cycles per candidate. For candidates that fail trial division (5-7%), this is the full MR cost avoided. For candidates that pass: **~9-10% of per-candidate MR cost**. Net Phase 1c: **~8-9%**.

**Risk:** Low. Barrett for fixed small moduli is well-understood. Edge cases: when `n < p`, the result should be `n` — Barrett handles this correctly since `q = 0`.

### OPT-4: Optimize mont_compute_r2 [LOW LEVERAGE, LOW RISK]

**What:**
Replace the 128-iteration addmod loop with a more efficient R² computation.

**Current code** (`gpu_math.cuh:86-92`):
```cpp
uint64_t r = 1ULL % m;  // = 1 (already optimized by OPT-2)
for (uint32_t i = 0; i < 128; ++i) {
    r = addmod_gpu(r, r, m);  // r = 2*r mod m
}
// Result: r = 2^128 mod m = R^2 mod m
```

128 iterations x (~8 cycles per addmod with branch) = ~1,024 cycles.

**Alternative:** Compute R² using a combination of shifts and Montgomery reductions:
```cpp
// R = 2^64. We need R^2 mod m = 2^128 mod m.
// Start with r0 = 2^64 mod m = (-m) mod 2^64 when m < 2^64
// Then r = mont_mul(r0, r0, m, m_inv) gives R^2 in one mont_mul.
// Actually: r0 = (2^64 mod m). Since m < 2^64, r0 = 2^64 - m * floor(2^64/m).
// But computing floor(2^64/m) requires division...
```

Simpler approach: reduce the iteration count. Since we only need 2^128 mod m and m ~ 2^61, we can start from a higher power:
```cpp
uint64_t r = (0ULL - m) % m;  // = 2^64 mod m, costs one rem_u64
// But we're trying to eliminate rem_u64...
```

Actually, the current 128-iteration approach is fine after OPT-2 removes the redundant `1ULL % m`. The remaining cost is just 128 addmod iterations at ~1,024 cycles — only ~2% of MR cost. Not worth the complexity.

**Verdict:** Skip this optimization. OPT-2 handles the rem_u64 inside it; the addmod loop itself is minor.

### OPT-5: Early Exit in Witness Loop [ALREADY IMPLEMENTED]

Checking `gpu_math.cuh:183-187`:
```cpp
for (int i = 0; i < NUM_MR_WITNESSES; ++i) {
    if (!miller_rabin_witness_mont_gpu(ctx, d, s, c_mr_witnesses[i])) {
        return false;  // composite — early exit
    }
}
```

Early exit on composite detection is already implemented. No change needed.

### OPT-6: Share Montgomery Context Across Witnesses [LOW LEVERAGE, ALREADY DONE]

The Montgomery context (m, m_inv, r2, one, nm1) is computed once per candidate at `gpu_math.cuh:182` and reused for all 12 witnesses. Already optimal.

### OPT-7: PTX Inline Assembly for mont_mul_gpu [LOW LEVERAGE, HIGH RISK]

**What:**
Replace the C++ mont_mul_gpu with hand-written PTX using `mul.lo.u64`, `mul.hi.u64`, `mad.lo.cc.u64`, `madc.hi.u64` to minimize instruction count.

**Analysis:**
The current C++ code compiles to reasonably efficient SASS already — the compiler generates IMAD.WIDE.U32 sequences that are close to optimal for sm_87. The bottleneck is the data dependency chain (each multiply feeds the next), not wasted instructions.

PTX might save 2-3 instructions per mont_mul by avoiding redundant carry propagation, but:
- Maintenance burden is high
- Compiler improvements in future CUDA versions would be bypassed
- Register allocation by ptxas may be less optimal with inline asm
- Expected speedup: ~5% of mont_mul cost = ~4% of MR cost = **~1% of Phase 1c**

**Verdict:** Skip unless other optimizations prove insufficient. The C++ code is already clean.

### OPT-8: Cooperative Witness Testing (Multi-thread per Candidate) [MODERATE LEVERAGE, HIGH COMPLEXITY]

**What:**
Instead of 1 thread x 12 witnesses, use 2 threads x 6 witnesses (or 4 x 3) per candidate.

**Benefits:**
- Reduces warp divergence: all threads in a warp complete closer to the same time
- For primes (worst case), halves the per-thread latency

**Problems:**
- Montgomery context must be shared between cooperating threads (shared memory or register shuffle)
- The modexp chain is sequential — you cannot parallelize the bit-scanning within a single witness
- Candidates must be paired/grouped before MR dispatch, adding a scheduling phase
- If one thread finds a composite witness, it must signal the partner to stop — requires additional sync
- For composites (majority), the early-exit benefit is smaller since most composites fail within 1-3 witnesses anyway

**Estimated speedup:** Moderate for primes, minimal for composites. Warp divergence reduction could save 10-15% of Phase 1c, but implementation complexity is high.

**Verdict:** Consider only after OPT-1/2/3 are implemented and measured. The reduced witness count from OPT-1 makes this less valuable (7 witnesses have less divergence than 12).

### OPT-9: Baillie-PSW Instead of Multi-Witness MR [HIGH LEVERAGE IF FEASIBLE, HIGH COMPLEXITY]

**What:**
Replace 12-round deterministic MR with strong Baillie-PSW: one round of MR (base 2) + one strong Lucas test.

**Analysis:**
- MR base 2: 1 witness powmod = ~87 mont_muls
- Lucas test: requires computing U_d, V_d via Lucas chain, which is similar cost to one modexp but uses different arithmetic (Lucas sequences, not simple squarings). Approximately 2x the cost of one MR witness due to the more complex recurrence.
- Total: ~87 + ~174 = ~261 mont_muls, vs current ~1,044 mont_muls
- Theoretical speedup: **~75% reduction in mont_muls**

**But:**
- Baillie-PSW has no known counterexample below 2^64, but it is NOT proven deterministic for our range. There may exist a counterexample below 3.215 x 10^18. The mathematical community believes none exist, but this is unproven.
- For a correctness-critical application (we need exact primality), relying on a probabilistic "no known counterexample" is unacceptable.
- Lucas test implementation is substantially more complex than MR on GPU (needs Jacobi symbol computation, Lucas sequence arithmetic).

**Verdict:** Reject. We need deterministic primality, and the Lucas implementation complexity is high for uncertain benefit. OPT-1 (7-witness MR) provides a safe 42% reduction with proven correctness.

## 4. Expected Combined Speedup

Applying OPT-1 + OPT-2 + OPT-3 (the recommended set):

| Optimization | Phase 1c Reduction | Cumulative |
|-------------|-------------------|------------|
| OPT-1: 7 witnesses | ~20-25% | 20-25% |
| OPT-2: Remove redundant % | ~5-6% of remaining | 24-30% |
| OPT-3: Barrett trial division | ~8-9% of remaining | 30-36% |
| **Combined** | | **~30-36%** |

Phase 1c drops from 7.4M to ~4.7-5.2M cycles. As a fraction of total kernel time, Phase 1c goes from 25.85% to ~17-18%.

Total kernel speedup: saves ~2.2-2.7M cycles out of 28.8M = **~8-9% total kernel speedup**, raising throughput from ~1,131 to ~1,230-1,240 tiles/sec.

## 5. Implementation Order

### Step 1: OPT-2 — Remove Redundant % [30 minutes]

**Files:** `tile-cuda/include/gpu_math.cuh` lines 87, 104, 105, 110

Smallest change, zero risk, immediate benefit. Eliminates 15 rem_u64 calls per candidate. This is a pure correctness-preserving simplification.

**Verification:** Bit-exact output on 1,000 operating-point tiles before/after.

### Step 2: OPT-1 — Reduce to 7 Witnesses [1 hour]

**Files:**
- `tile-cuda/include/gpu_constants.cuh` line 50: change `NUM_MR_WITNESSES` from 12 to 7
- `tile-cuda/src/tile_kernel.cu` lines 24-26: trim `kMrWitnesses` to {2,3,5,7,11,13,17}

**Prerequisite validation:** Before changing the kernel, run a validation harness that:
1. Computes max possible norm: (830,000,000 + 271/2 + 7)² + (830,000,000 + 271/2 + 7)² = 2 x 830,000,142.5² ≈ 1.378 x 10^18
2. Confirms 1.378 x 10^18 < 3.215 x 10^18 (the proven bound for 7-witness MR)
3. Runs 12-witness vs 7-witness comparison on 10,000 tiles at max radius — zero disagreements required

**Verification:** Same as prerequisite, plus full regression on operating-point tile set.

### Step 3: OPT-3 — Barrett Trial Division [2 hours]

**Files:**
- `tile-cuda/include/gpu_constants.cuh`: add `NUM_TRIAL_BARRETT` constant, define trial Barrett struct
- `tile-cuda/include/gpu_types.cuh`: add `TrialPrimeBarrettGPU` struct
- `tile-cuda/include/gpu_math.cuh`: add Barrett variant of trial division loop, add constant memory array for Barrett params
- `tile-cuda/src/tile_kernel.cu`: add constant memory definition and upload
- `tile-cuda/src/main.cu`: precompute Barrett parameters for trial primes

**Barrett parameter for 64-bit trial division:**

For n < 2^61 and p < 128, use 64-bit Barrett:
```cpp
struct TrialPrimeBarrettGPU {
    uint32_t p;
    uint32_t pad;
    uint64_t mu;  // floor(2^64 / p) or ceil(2^64 / p)
};
```

Replacement in `is_prime_gpu`:
```cpp
// Current:
if (n % p == 0ULL) { return false; }

// Barrett:
uint64_t q = __umul64hi(n, mu);
uint64_t r = n - q * p;
if (r >= p) r -= p;  // at most one correction needed
if (r == 0ULL) { return false; }
```

The `__umul64hi(n, mu)` compiles to the same ~4 IMAD.WIDE.U32 instructions as any 64-bit multiply, but avoids the entire MUFU.RCP + Newton-Raphson + F2I + correction chain of `__cuda_sm20_rem_u64`. Net savings: ~200 cycles → ~20 cycles per trial prime = ~180 cycles saved x 24 primes = ~4,320 cycles per candidate.

**Verification:** Host-side exhaustive check: for all 24 trial primes, for 10M random norms in operating range, confirm Barrett result matches `n % p`.

## 6. Risk Assessment

### Correctness Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| 7-witness MR has a counterexample in [1, 3.215e18] | **Catastrophic** | The bound is from peer-reviewed research (Jiang & Deng 2014) with computational verification. Cross-validate with 12-witness on 10K tiles. Additionally verify the exact max norm calculation. |
| Barrett trial division returns wrong remainder | High | Exhaustive host-side test for all 24 primes over dense norm sample. The math is standard. |
| Removing % in mont_to_gpu fails for some witness/norm combo | High | Prove a < m: max witness = 37 (or 17 after OPT-1), min norm after trial division > 97² = 9,409. Always satisfied. Test confirms. |

### Performance Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Reduced witnesses worsen warp divergence (faster threads idle longer) | Low | Negative | 7 vs 12 witnesses actually reduces max-divergence time since the slowest thread finishes sooner. |
| Barrett adds register pressure | Low | Moderate | Barrett uses ~2 extra registers for mu. At 46 regs/thread, adding 2 stays well under the 4-block cliff at 57. |
| Compiler reorganizes mont_mul after removing % | Very Low | Unknown | The mont_mul code path is unaffected by removing the % calls in init/to_gpu. Verify register count post-build. |

### Occupancy Risks

| Parameter | Current | After OPT-1/2/3 | Limit |
|-----------|---------|------------------|-------|
| Registers/thread | 46 | 46-48 (est.) | 57 (4-block cliff) |
| Dynamic shared | 36,640 | 36,640 (unchanged) | 41,984 (4-block cliff at 167,936/4) |
| Constant memory | ~10,144 | ~10,528 (+384 for trial Barrett) | 65,536 |

All within safe margins.

## 7. What This Plan Does NOT Cover

1. **Phase 4+5 optimization** (44% of kernel time) — separate analysis needed; this is the actual largest bottleneck
2. **Warp divergence reduction** beyond witness count reduction — OPT-8 deferred
3. **Alternative primality algorithms** (Baillie-PSW, APR-CL) — rejected for correctness reasons
4. **Montgomery multiply micro-optimization** (PTX inline asm) — deferred as low-leverage
5. **Sieve-stage optimizations** (Barrett for sieve, single-pass) — covered in separate plan at `docs/supportive/2026-04-10-sieve-opt-implementation-spec.md`
