---
title: Barrett Reduction + Single Sieve Pass Execution Plan
date: 2026-04-10
engine: codex
type: design-note
status: complete
refs:
  - tile-cuda/src/tile_kernel.cu
  - tile-cuda/include/gpu_sieve.cuh
  - tile-cuda/include/gpu_math.cuh
  - tile-cuda/include/gpu_constants.cuh
  - tile-cuda/include/gpu_types.cuh
  - docs/tile_internals_cuda.md
---

# Barrett Reduction + Single Sieve Pass Execution Plan

## Scope Note

This branch does not match the task text exactly:

- There is no `tile-cuda/include/gpu_miller_rabin.cuh`; Miller-Rabin lives in `tile-cuda/include/gpu_math.cuh`.
- There are no `rem_u64` / `rem_s64` helpers in the current source. The remaining expensive remainder work is inline `%` on 64-bit values.

The plan below is therefore anchored to the current branch state, not the older helper-based design.

## 1. Current State Analysis

### 1.1 Phase boundaries and call sites

- Phase 1a/1b live in `process_tiles_kernel()` at `tile-cuda/src/tile_kernel.cu:104-127`.
- `sieve_row()` is called twice per active row thread:
  - count pass at `tile-cuda/src/tile_kernel.cu:104-110`
  - scatter pass at `tile-cuda/src/tile_kernel.cu:122-127`
- Phase 1c starts at `tile-cuda/src/tile_kernel.cu:138-139` and runs through `mr_test_candidates()` in `tile-cuda/include/gpu_sieve.cuh:77-105`.
- `is_prime_gpu()` is the MR entry point in `tile-cuda/include/gpu_math.cuh:123-159`.

### 1.2 Exact 64-bit remainder inventory

The table below includes every current `%` on a 64-bit value in the CUDA path.

| Location | Expression | Phase | Static count | Dynamic count |
|---|---|---:|---:|---:|
| `tile-cuda/include/gpu_sieve.cuh:27` | `(int64_t(a_mod) * root) % int64_t(p)` | 1a | 1 site | `609` per `sieve_row()` call |
| `tile-cuda/include/gpu_math.cuh:57` | `1ULL % m` | 1c | 1 site | `1` per candidate reaching `mont_compute_r2()` |
| `tile-cuda/include/gpu_math.cuh:74` | `1ULL % ctx.m` | 1c | 1 site | `1` per candidate reaching `mont_init_gpu()` |
| `tile-cuda/include/gpu_math.cuh:75` | `(ctx.m - 1ULL) % ctx.m` | 1c | 1 site | `1` per candidate reaching `mont_init_gpu()` |
| `tile-cuda/include/gpu_math.cuh:80` | `a % ctx.m` | 1c | 1 site | `1` per witness, `12` per candidate worst case |
| `tile-cuda/include/gpu_math.cuh:139` | `n % p` | 1c | 1 site | `24` per candidate worst case |

#### Phase 1a count

- `sieve_row()` iterates `SPLIT_PRIMES_COUNT = 609` at `tile-cuda/include/gpu_sieve.cuh:22-35`.
- Each call executes exactly one 64-bit `%` per split prime at line 27.
- The kernel calls `sieve_row()` twice per active row.
- `ACTIVE_ROWS = 271` (`tile-cuda/include/gpu_constants.cuh:44`).

Dynamic cost:

- `609` 64-bit remainders per row per pass
- `1,218` 64-bit remainders per active row thread across the two passes
- `609 * 271 * 2 = 330,078` 64-bit remainders per tile in Phase 1a/1b

#### Phase 1c count

For a non-axis candidate that survives to MR:

- trial division: `24` remainders at `tile-cuda/include/gpu_math.cuh:134-141`
- Montgomery context setup: `3` remainders at `tile-cuda/include/gpu_math.cuh:57,74,75`
- witness conversion: `12` remainders through `mont_powmod_gpu()` -> `mont_to_gpu()` at `tile-cuda/include/gpu_math.cuh:79-98,153-154`

Worst-case source-level total:

- `39` 64-bit remainders per candidate entering full MR

Important nuance: current MR hot cost is mostly `mont_mul_gpu()` at `tile-cuda/include/gpu_math.cuh:37-46`, not `%`. The task’s “~90 rem calls/thread” statement appears to describe an older branch.

### 1.3 How sieve primes are passed today

Current device storage:

- split primes: `__constant__ uint32_t c_split_table[SPLIT_PRIMES_COUNT]` at `tile-cuda/src/tile_kernel.cu:15`
- inert primes: `__constant__ uint16_t c_inert_primes[INERT_PRIMES_COUNT]` at `tile-cuda/src/tile_kernel.cu:16`

Current host packing:

- `SieveTables` in `tile-cuda/include/gpu_types.cuh:57-62`
- built in `init_sieve_tables_host()` at `tile-cuda/src/main.cu:50-103`
- uploaded in `upload_sieve_tables()` at `tile-cuda/src/tile_kernel.cu:174-181`

ABI today:

- split prime entry: packed `root << 16 | p` at `tile-cuda/src/main.cu:91-92`
- inert prime entry: raw `uint16_t p` at `tile-cuda/src/main.cu:97`

## 2. Barrett Plan

### 2.1 Design target

Use Barrett only where the modulus is fixed across many threads and many tiles:

- yes: sieve split primes and inert primes
- no: full Miller-Rabin modexp, where modulus is the candidate norm and changes per candidate

### 2.2 Recommended Barrett parameter layout

Use a fixed global shift of `32`, not a per-prime shift. All current sieve operands are small enough that a 32-bit reciprocal is sufficient:

- for `a mod p`, `|a|` is about `1e9`, still less than `p * 2^32`
- for split residue reduction, `x = a_mod * root < p^2 < 1e8`

Recommended structs:

```cpp
struct SplitPrimeBarrettGPU {
    uint16_t p;
    uint16_t root;
    uint32_t mu;   // floor(2^32 / p)
};

struct InertPrimeBarrettGPU {
    uint16_t p;
    uint16_t pad;
    uint32_t mu;   // floor(2^32 / p)
};
```

Why this layout:

- `p < 10000`, so `uint16_t` is enough
- split `root < p`, so `uint16_t` is enough
- `mu` fits in `uint32_t` for `shift = 32`
- each entry is `8` bytes, so all `1,228` primes take `9,824` bytes total

This is materially smaller than the task’s initial `16-24` byte estimate and fits comfortably in constant memory.

### 2.3 Memory placement

Keep Barrett tables in constant memory.

Projected constant-memory footprint:

- Barrett sieve tables: `1,228 * 8 = 9,824` bytes
- MR witnesses: `12 * 8 = 96` bytes
- trial primes: `24 * 4 = 96` bytes
- backward offsets: `64 + 64 = 128` bytes
- total tracked constant payload: about `10,144` bytes

That is far below the 64 KB constant-memory limit and preserves the existing warp-broadcast access pattern from `sieve_row()`.

Do not put Barrett tables in shared memory. Shared memory is already the occupancy-sensitive resource.

### 2.4 Host precomputation

Replace or extend `SieveTables` creation in `tile-cuda/src/main.cu:50-103`:

1. For every split prime `p`, keep current `root` computation.
2. Compute `mu = floor((1ULL << 32) / p)`.
3. Store `{p, root, mu}` in a new split-Barrett table.
4. For every inert prime `p`, compute the same `mu`.
5. Upload both new arrays with `cudaMemcpyToSymbol`, parallel to the current `upload_sieve_tables()` path.

Host-side validation gate:

- assert `root * root % p == p - 1` still holds
- assert `barrett_mod_u32(x, p, mu) == x % p` for a dense sample of `x` values before upload

### 2.5 Device replacement pattern

Add two helpers:

```cpp
__device__ __forceinline__ uint32_t barrett_mod_u32(uint32_t x, uint32_t p, uint32_t mu);
__device__ __forceinline__ int32_t barrett_euclidean_mod_s32(int32_t x, uint32_t p, uint32_t mu);
```

Preferred implementation shape:

- `q = __umulhi(x, mu)` for the approximate quotient
- `r = x - q * p`
- correct with one or two conditional subtracts if `r >= p`
- for signed inputs, reduce `abs(x)` and map back to Euclidean form

Replacement points:

- `tile-cuda/include/gpu_sieve.cuh:27`
  - replace `% p` on `a_mod * root`
- `tile-cuda/include/gpu_sieve.cuh:31` and `:39`
  - replace `euclidean_mod_gpu()` for `a` and `-residue`
- `tile-cuda/include/gpu_math.cuh:21-31`
  - add a Barrett-aware variant of `mark_residue_class_reg()` so `b_start mod p` and `first_col mod p` also stop using `%`

Important detail: a partial Barrett port that only changes line 27 leaves a lot of 32-bit division in `euclidean_mod_gpu()`. The better sieve plan is to migrate all modulus work in the sieve path to Barrett helpers, not just the one 64-bit `%`.

### 2.6 Miller-Rabin-specific approach

Do not replace Montgomery modexp with candidate-local Barrett reduction as the first MR optimization.

Reason:

- the modulus changes per candidate
- the hot MR work is `mont_mul_gpu()`, not the handful of `%` sites
- per-candidate Barrett setup would add new reciprocal computation and new reduction plumbing without removing the dominant modular-multiply loop

Better MR plan on this branch:

1. Keep Montgomery for the witness loop.
2. Remove the redundant remainder sites:
   - `1ULL % m` at `gpu_math.cuh:57` -> literal `1ULL`
   - `1ULL % ctx.m` at `gpu_math.cuh:74` -> literal `1ULL`
   - `(ctx.m - 1ULL) % ctx.m` at `gpu_math.cuh:75` -> `ctx.m - 1ULL`
   - `a % ctx.m` at `gpu_math.cuh:80` -> `a`, because `miller_rabin_witness_mont_gpu()` already guards `a >= ctx.m` at `gpu_math.cuh:105-107`
3. If trial division still matters, add a tiny Barrett table for `c_trial_primes[24]` and replace `n % p` at `gpu_math.cuh:139`.

Net: Barrett should be a sieve optimization first. MR should stay Montgomery-based, with local cleanup of avoidable `%` and optional Barrett only for the fixed 24 trial divisors.

## 3. Single Sieve Pass Plan

### 3.1 What `sieve_row()` computes now

`sieve_row()` at `tile-cuda/include/gpu_sieve.cuh:14-43` builds a row-local composite mask:

- `ws[9]` is the full 271-bit row bitmap, packed into 9 words
- parity is pre-seeded into `ws`
- split primes mark one or two residue classes
- inert primes mark the zero residue class when `a mod p == 0`

Then:

- `count_sieve_survivors()` at `tile-cuda/include/gpu_sieve.cuh:45-56` popcounts `~ws`
- `scatter_survivors()` at `tile-cuda/include/gpu_sieve.cuh:58-75` enumerates the same `~ws`

Today that whole sieve computation is repeated because `ws` is not preserved across the block-wide scan.

### 3.2 Option A: cache `ws[9]` across the scan

Two placements are possible:

- registers
- shared memory

#### A1. Registers

Cost:

- `9` extra `uint32_t` values live across `__syncthreads()`
- best-case increase is about `+9` registers/thread

Occupancy implication using the current design-note numbers (`46` regs/thread, `4` blocks/SM):

- current: `46 * 288 * 4 = 52,992` registers
- with `+9`: `55 * 288 * 4 = 63,360` registers
- with `+10`: `56 * 288 * 4 = 64,512` registers
- with `+11`: `57 * 288 * 4 = 65,664`, which breaks 4-block residency on a 65,536-register SM budget

Conclusion:

- register-caching is possible in theory
- it is too close to the 4-block cliff to be the first implementation choice
- it must be gated on actual `ptxas` register counts and spill metrics

#### A2. Shared memory

Cost:

- active rows only: `271 * 9 * 4 = 9,756` bytes
- all 288 threads: `288 * 9 * 4 = 10,368` bytes

This breaks the current 4-block shared-memory budget:

- current total block footprint: `37,680` bytes
- plus active-row `ws` cache: `47,436` bytes
- `47,436 * 4 = 189,744` bytes, which exceeds the `167,936` bytes/SM budget recorded in `docs/tile_internals_cuda.md:556-563`

Conclusion: shared-memory `ws` caching should be rejected.

### 3.3 Option B: true single-pass row reservation

This is the recommended plan.

Algorithm:

1. Each active row thread computes `ws[9]` once.
2. It calls `count_sieve_survivors(ws)` locally.
3. If count is nonzero, it reserves a contiguous range in `cand_list` with one shared `atomicAdd`.
4. It immediately calls `scatter_survivors(ws, cand_list, base, tid)` using that reservation.
5. After all rows finish, one thread clamps `total_cands` to `MAX_CANDIDATES_GPU`.

Why this is better:

- no second `sieve_row()` call
- no long-lived `ws[9]` across a block barrier
- no extra shared memory for `ws`
- no `cand_counts` or `cand_prefix` arrays
- candidate order becomes nondeterministic across rows, but MR does not require row-stable ordering

Required kernel changes:

- replace `cand_counts` / `cand_prefix` in `tile-cuda/src/tile_kernel.cu:82-84`
- add one shared counter for raw candidate count
- rewrite the Phase 1a/1b block at `tile-cuda/src/tile_kernel.cu:104-136`

Shared-memory effect:

- current phase-1 overlay: `26,884` bytes
- atomic-reservation phase-1 overlay: `6,144 * 4 + 4 = 24,580` bytes
- dynamic shared total falls from `36,640` to `34,336` bytes
- total block footprint falls from `37,680` to `35,376` bytes

This improves, rather than hurts, occupancy margin.

### 3.4 Option C: warp-ballot or per-survivor atomic scatter

Do not do this first.

Reason:

- more complex than per-row reservation
- per-survivor atomics are obviously worse
- warp-ballot packing still needs either a global reservation or a second prefix stage
- no clear evidence it beats one atomic per active row

If per-row `atomicAdd` shows measurable serialization, revisit warp-aggregated reservation later. It should not be the first implementation attempt.

## 4. Shared Memory Budget

### 4.1 Current kernel footprint

From source:

- bitmap: `ACTIVE_ROWS * BITMAP_WORDS_PER_ROW * 4 = 271 * 9 * 4 = 9,756` bytes
- phase-1 overlay: `(288 + 289 + 6,144) * 4 = 26,884` bytes
- dynamic shared total: `36,640` bytes from `tile-cuda/src/tile_kernel.cu:44-50,203-205`
- static shared:
  - `total_cands`: `4` bytes at `tile-cuda/src/tile_kernel.cu:92`
  - `face_data`: about `1,032` bytes at `tile-cuda/src/tile_kernel.cu:161`
  - rounded total in design note: `1,040` bytes
- total block footprint: `36,640 + 1,040 = 37,680` bytes

### 4.2 After Barrett only

Barrett tables live in constant memory, so shared memory is unchanged:

- dynamic shared: `36,640` bytes
- total block footprint: `37,680` bytes

### 4.3 After recommended single-pass sieve

With row-reservation scatter:

- phase-1 overlay drops to `24,580` bytes
- dynamic shared becomes `34,336` bytes
- total block footprint becomes `35,376` bytes

### 4.4 After rejected shared-memory `ws` cache

- total block footprint would rise to `47,436` bytes with an active-row cache
- that exceeds 4-block residency against the `167,936` bytes/SM budget

## 5. Risks

### Correctness risks

- Barrett correction logic must be exact for negative `a` / `b_start` inputs, not just positive values.
- Split-prime residue computation must preserve Euclidean semantics when `a < 0`.
- Single-pass atomic reservation removes row-stable candidate ordering. That should be behaviorally irrelevant, but verify there are no hidden assumptions in debugging or downstream instrumentation.
- Candidate-list clamping must stay safe when `raw_total > MAX_CANDIDATES_GPU`; writes past `cand_list` must be prevented, not just ignored after the fact.

### Performance risks

- Register-cached `ws[9]` may push the kernel from 4 blocks/SM to 3 if `ptxas` lands at `57+` registers/thread or spills.
- Shared-memory atomics may serialize if candidate counts spike, though `271` row-level atomics per tile is small relative to the removed second sieve pass.
- Barrett can underperform if implemented with 64-bit multiplies everywhere. The point of the recommended design is to use 32-bit reciprocals and `__umulhi` where possible.

### Branch-state risks

- The task’s MR narrative assumes helper-based software division. This branch already uses Montgomery multiplication. Do not spend implementation time building candidate-local Barrett reduction for MR unless fresh profiling shows `%` still dominates Phase 1c.

## 6. Implementation Order

1. Barrett for sieve tables and sieve helper path.
   - Largest clearly-aligned win.
   - No shared-memory impact.
   - Mechanically contained to sieve data tables and modulus helpers.

2. Single-pass sieve with per-row atomic reservation.
   - Removes the second `sieve_row()` call without threatening occupancy.
   - Simplifies Phase 1 shared-memory layout.

3. MR cleanup, not full Barrett.
   - Remove redundant `%` in Montgomery setup and witness conversion.
   - Then benchmark.
   - Only if needed, Barrett-optimize the 24 fixed trial divisors.

4. Revisit register-cached `ws` only if the atomic-reservation variant disappoints.

## 7. Verification Gates

Use operating-point tiles only. Do not validate at near-origin radii.

### Gate A: Barrett correctness

- Add host-side exhaustive tests for every sieve prime over a dense range of `x` values:
  - positive and negative `a`
  - positive and negative `b_start`
  - split residue products `a_mod * root`
- Confirm equality with current `%`-based results.
- Confirm the generated split/inert Barrett tables still have counts `609` and `619`.

### Gate B: Sieve-path equivalence

- Instrument a debug build that compares old `sieve_row()` output and new Barrett `sieve_row()` output for the same operating-point tile set.
- Compare:
  - all 9 `ws` words per row
  - per-row survivor counts
  - total raw candidate count

### Gate C: Single-pass equivalence

- Compare old two-pass and new single-pass candidate sets for the same rows.
- Candidate order may differ; compare as multisets or by sorting packed `(row,col)` values.
- Confirm `total_cands` matches the old path before clamp and after clamp.

### Gate D: Full-kernel correctness

- Run the existing CUDA smoke/bench flow on operating-point tiles only.
- Validate:
  - `prime_counts`
  - `TileOp` bytes
  - overflow sentinel rate
- Cross-check against `tile-cpp` on Jetson for a representative tile batch at two angles, for example:
  - near 30 degrees
  - near 45 degrees

### Gate E: Performance and occupancy

- Capture `cudaFuncGetAttributes()` output after each step:
  - registers/thread
  - static shared memory
  - constant memory
- Capture `cudaOccupancyMaxActiveBlocksPerMultiprocessor()`.
- Benchmark at the current operating point with enough tiles to amortize startup.
- Required checks:
  - Barrett-only build must not reduce active blocks/SM.
  - single-pass build must keep 4-block residency and should reduce Phase 1 time.
  - if register count rises to `57+`, stop and reassess before merging.

## Bottom Line

- Barrett belongs in the fixed-prime sieve, with compact constant-memory reciprocal tables.
- Full MR should remain Montgomery-based on this branch; only clean up redundant `%` and optionally Barrett-optimize the 24 fixed trial divisors.
- The best single-pass sieve plan is not shared-memory `ws` caching. It is one `sieve_row()` call plus one row-level `atomicAdd` reservation into `cand_list`.
