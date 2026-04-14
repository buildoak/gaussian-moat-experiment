---
title: MR Optimization Edge Analysis
date: 2026-04-10
engine: codex
type: design-note
status: complete
refs:
  - tile-cuda/include/gpu_math.cuh
  - tile-cuda/include/gpu_sieve.cuh
---

# MR Optimization Edge Analysis

## Scope note

The task brief says Phase 1c is `11.4M` cycles and `61.5%` of kernel time. The current branch baseline says otherwise: `docs/supportive/2026-04-10-profiling-baseline.md` measures Phase 1c at **7,453,446 mean cycles** and **25.85%** of total cycles. The analysis below is anchored to the current branch and the current SASS dump.

## 1. Exact cost of the current Montgomery multiply

`mont_mul_gpu()` in `tile-cuda/include/gpu_math.cuh:67-75` computes:

```cpp
lo    = a * b;
hi    = __umul64hi(a, b);
q     = lo * m_inv;
qm_lo = q * m;
qm_hi = __umul64hi(q, m);
carry = (lo + qm_lo < lo);
r     = hi + qm_hi + carry;
```

If `a = a0 + 2^32 a1`, `b = b0 + 2^32 b1`, `m = m0 + 2^32 m1`, `m_inv = u0 + 2^32 u1`, the exact 32x32 partial-product count is:

- full `a*b` to 128 bits: **4** multiplies
- `q = lo * m_inv mod 2^64`: **3** multiplies
- full `q*m` to 128 bits: **4** multiplies

Total: **11 exact 32x32 multiplies** per generic Montgomery multiply.

That is the right comparison point on sm_87. Source-level “5 uint64 multiplies” is true, but it overstates the real lower bound because two of the products only need low 64-bit output.

## 2. What a radix-2^32 two-limb Montgomery path would cost

For a 61-bit odd modulus, a two-limb CIOS Montgomery reduction with radix `B = 2^32` costs:

- schoolbook `a*b`: **4** multiplies
- reduction step 0: `m0 = t0*n0'` (**1**) plus `m0*n` (**2**)
- reduction step 1: `m1 = t1*n0'` (**1**) plus `m1*n` (**2**)

Total: **10 exact 32x32 multiplies**.

So the best-case arithmetic gain over the current formulation is only:

- `11 -> 10`, or **9.1%**

That is not a 2-4x lever. Even before carry instructions are counted, the asymptotic work is nearly the same.

## 3. PTX/SASS reality

### 3.1 `%` is expensive when it survives

`__cuda_sm20_rem_u64` at `tmp/sass-dump-2026-04-10.txt:80-510` contains **96 SASS instructions** in this dump, including:

- `1` `MUFU.RCP`
- `1` `I2F.U64.RP`
- `1` `F2I.U64.TRUNC`
- `6` `IMAD.WIDE.U32`
- `9` `IMAD`
- `9` `IMAD.HI.U32`
- `5` `IMAD.X`
- `9` `IADD3`
- `6` `IADD3.X`

That is not a cheap helper. If a `%` can be deleted entirely, that is much better than replacing it with a slightly cheaper form.

### 3.2 The hot Montgomery block is already dense

One hot inlined block in the witness loop spans `0x104c0..0x10730` in `tmp/sass-dump-2026-04-10.txt`. It contains **40 SASS instructions**:

- `8` `IMAD.WIDE.U32`
- `2` `IMAD.WIDE.U32.X`
- `4` `IMAD`
- `3` `IMAD.X`
- `6` `IADD3`
- `3` `IADD3.X`
- `4` predicate compares
- `5` `SEL`

The important conclusion is that the generated code already looks like a tightly packed carry chain, not like bloated helper code with an obvious 2x win hiding in codegen.

## 4. Biggest missed optimization: delete trial division on the non-axis norm path

`is_prime_gpu()` in `tile-cuda/include/gpu_math.cuh:164-172` does 24 trial divisions by `3..97`.

For the non-axis path those are redundant:

1. `cand_list` contains only sieve survivors from `sieve_row()` in `tile-cuda/include/gpu_sieve.cuh:11-46`.
2. The sieve already covers **all odd primes up to `SIEVE_LIMIT = 10000`**.
3. Every trial prime in `c_trial_primes` is between `3` and `97`.
4. Therefore a non-axis `norm = a^2+b^2` that reaches `is_prime_gpu(norm)` is already known not to be divisible by any of those 24 primes.

These checks are only needed for the axis scalar path (`is_axis_gaussian_prime_gpu()` at `tile-cuda/include/gpu_math.cuh:196-205`) or for generic tiny integers. They are dead work for the operating non-axis path.

The right fix is to split the entry point:

- `is_prime_norm_gpu(n)` for non-axis norms
- `is_prime_scalar_gpu(n)` for axis / generic fallback

`is_prime_norm_gpu()` can remove:

- the full 24-prime trial loop
- the tiny-number checks that cannot occur at operating radii

This removes **24 dynamic `__cuda_sm20_rem_u64` calls per non-axis candidate**. That is a larger and cleaner win than Barrett-optimizing those divisions.

## 5. Witness reduction: the existing plan picked the wrong 7 witnesses

The current 12 bases are the first primes through `37`. The existing plan proposed dropping to the first 7 primes `{2,3,5,7,11,13,17}`. That is not sound for this range.

Known deterministic bounds:

- `{2,3,5,7,11,13,17}` is enough only for `n < 341,550,071,728,321`
- `{2,3,5,7,11,13,17,19,23}` is enough for `n < 3,825,123,056,546,413,051`
- first 12 prime bases through `37` are enough for all `n < 2^64`

Your norm range is about `1.38e18`, so:

- 7 consecutive prime bases are **not** enough
- 9 consecutive prime bases through `23` **are** enough

There is a better missed option: the known deterministic 7-base set for all `n < 2^64`:

- `{2, 325, 9375, 28178, 450775, 9780504, 1795265022}`

Why this set is attractive here:

- it cuts `12 -> 7` witnesses
- it remains deterministic for the full 64-bit range
- all bases fit in `uint32_t`
- for operating non-axis norms, every base is still `< n`, so the current `a >= ctx.m` guard remains dead

This is a real `41.7%` reduction in prime-path witness count. In whole-Phase-1c terms it is more like **20-30%**, because many composites already exit early.

## 6. Alternative approaches

### 6.1 Exploiting “norms are sums of two squares”

Useful:

- every non-axis odd norm is `1 mod 4`
- this justifies a specialized non-axis primality path

Not useful:

- checking `n % 4 == 1` filters nothing because every non-axis odd norm already satisfies it
- switching to Lucas/BPSW would either lose determinism or introduce a comparably expensive 64-bit modular-arithmetic path

### 6.2 Extra sieve before MR

Extending the sieve to `50K` or `100K` is a bad trade on the current kernel.

Using the local density model for `a^2+b^2` survivors:

| limit | split primes | inert primes | entries | survivor ratio vs 10K |
|---|---:|---:|---:|---:|
| 10K | 609 | 619 | 1,228 | 1.000 |
| 50K | 2,549 | 2,583 | 5,132 | 0.853 |
| 100K | 4,783 | 4,808 | 9,591 | 0.802 |

So relative to the current 10K sieve:

- `50K` removes only about **14.7%** more candidates
- `100K` removes only about **19.8%** more candidates

But sieve work scales almost linearly in the number of entries:

- `1,228 -> 5,132` entries at `50K`: **4.18x**
- `1,228 -> 9,591` entries at `100K`: **7.81x**

Current measured sieve cost is Phase 1a + 1b = **5.15M cycles**. Linear scaling gives rough sieve costs of:

- `50K`: `5.15M * 4.18 ~= 21.5M`
- `100K`: `5.15M * 7.81 ~= 40.2M`

That is far larger than the MR cycles those extra sieves would save.

Constant memory is also a hard limit with the current 8-byte Barrett entries:

- `10K`: `9,824` bytes
- `50K`: `41,056` bytes
- `100K`: `76,728` bytes

`100K` does not fit in 64 KB constant memory.

### 6.3 CGBN / other CUDA bignum libraries

CGBN’s public README says it targets:

- **32-bit through 32K-bit** integers
- with **4, 8, 16, or 32 threads per big-number instance**
- and includes fast odd-modulus `powm` / Miller-Rabin samples

That supports one conclusion: mature CUDA modular arithmetic still reduces to 32-bit limb arithmetic. There is no missing native sm_87 64-bit modular primitive here.

But CGBN is a bad fit for this kernel:

- each candidate here is only 61 bits, i.e. two 32-bit limbs
- minimum group size is 4 threads per integer
- moving from 1-thread-per-candidate to 4-thread groups would cut candidate-level parallelism by at least 4x before arithmetic savings

So the right direction is still scalar single-thread arithmetic, not a library transplant.

## 7. What the edge actually looks like

High-leverage, sound steps:

1. Split `is_prime_gpu()` into norm-specialized and scalar-generic paths.
2. Delete the 24-prime trial loop from the norm path.
3. Replace the 12 prime witnesses with the deterministic 7-base 64-bit set.
4. Keep Montgomery reduction.
5. Only after that, if Phase 1c still matters, experiment with a hand-written two-limb radix-`2^32` Montgomery reducer.

Expected upside:

- remove non-axis trial division: roughly **1.1x to 1.15x** on Phase 1c
- 12 witnesses -> deterministic 7-base set: roughly **1.2x to 1.3x** on Phase 1c
- dead `%` cleanup in `mont_init_gpu()` / `mont_to_gpu()`: small but worth taking
- two-limb radix-`2^32` Montgomery rewrite: likely **single-digit to low-double-digit percent**, not more

Combined, a realistic target is about **1.4x to 1.7x faster Phase 1c**.

I do **not** see a defensible 2-4x path from arithmetic micro-optimization alone on sm_87. The code is already close to the limb-arithmetic floor. The remaining big gains come from doing less MR work, not from re-encoding the same MR work.
