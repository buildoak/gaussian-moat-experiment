#pragma once

#include <cstdint>

#include "cuda_campaign/constants.cuh"

namespace cuda_campaign {

struct SplitPrimeBarrettGPU {
  std::uint16_t p;
  std::uint16_t root;
  std::uint32_t mu;
};
static_assert(sizeof(SplitPrimeBarrettGPU) == 8,
              "Barrett split prime must be 8 bytes");

struct InertPrimeBarrettGPU {
  std::uint16_t p;
  std::uint16_t pad;
  std::uint32_t mu;
};
static_assert(sizeof(InertPrimeBarrettGPU) == 8,
              "Barrett inert prime must be 8 bytes");

struct SieveTablesBarrett {
  SplitPrimeBarrettGPU split_table[SPLIT_PRIMES_COUNT];
  InertPrimeBarrettGPU inert_primes[INERT_PRIMES_COUNT];
  int split_count;
  int inert_count;
};

extern __constant__ SplitPrimeBarrettGPU c_split_barrett[SPLIT_PRIMES_COUNT];
extern __constant__ InertPrimeBarrettGPU c_inert_barrett[INERT_PRIMES_COUNT];
extern __constant__ std::uint32_t c_trial_primes[NUM_TRIAL_PRIMES];
extern __constant__ std::int8_t c_bk_dr[NUM_BACKWARD_OFFSETS];
extern __constant__ std::int8_t c_bk_dc[NUM_BACKWARD_OFFSETS];

__device__ __forceinline__ std::uint32_t barrett_mod_u32(
    std::uint32_t x, std::uint32_t p, std::uint32_t mu) {
  const std::uint32_t q = __umulhi(x, mu);
  std::uint32_t r = x - q * p;
  if (r >= p) {
    r -= p;
  }
  return r;
}

__device__ __forceinline__ std::int32_t barrett_euclidean_mod(
    std::int32_t value, std::uint32_t p, std::uint32_t mu) {
  const std::uint32_t abs_val =
      static_cast<std::uint32_t>(value >= 0 ? value : -value);
  const std::uint32_t r = barrett_mod_u32(abs_val, p, mu);
  return (value < 0 && r != 0) ? static_cast<std::int32_t>(p - r)
                               : static_cast<std::int32_t>(r);
}

__device__ __forceinline__ void mark_residue_class_barrett(
    std::uint32_t ws[BITMAP_WORDS_PER_ROW],
    std::int32_t b_start,
    std::uint32_t p,
    std::int32_t residue,
    std::uint32_t mu) {
  const std::int32_t b_mod = barrett_euclidean_mod(b_start, p, mu);
  const std::int32_t diff = residue - b_mod;
  const std::int32_t first_col = (diff >= 0) ? diff : diff + static_cast<std::int32_t>(p);
  for (std::int32_t col = first_col; col < SIDE_EXP; col += static_cast<std::int32_t>(p)) {
    ws[col >> 5] |= 1U << (col & 31);
  }
}

__device__ __forceinline__ std::uint64_t addmod_gpu(
    std::uint64_t a, std::uint64_t b, std::uint64_t m) {
  return (a >= m - b) ? (a - (m - b)) : (a + b);
}

__device__ __forceinline__ std::uint64_t mont_mul_gpu(
    std::uint64_t a, std::uint64_t b, std::uint64_t m, std::uint64_t m_inv) {
  const std::uint64_t lo = a * b;
  const std::uint64_t hi = __umul64hi(a, b);
  const std::uint64_t q = lo * m_inv;
  const std::uint64_t qm_lo = q * m;
  const std::uint64_t qm_hi = __umul64hi(q, m);
  const std::uint64_t carry = (lo + qm_lo < lo) ? 1ULL : 0ULL;
  const std::uint64_t r = hi + qm_hi + carry;
  return (r >= m) ? (r - m) : r;
}

__device__ __forceinline__ std::uint64_t mont_compute_m_inv(std::uint64_t m) {
  std::uint64_t x = 1ULL;
  for (std::uint32_t i = 0; i < 6; ++i) {
    x *= 2ULL - m * x;
  }
  return 0ULL - x;
}

__device__ __forceinline__ std::uint64_t mont_compute_r2(std::uint64_t m) {
  std::uint64_t r = 1ULL;
  for (std::uint32_t i = 0; i < 128; ++i) {
    r = addmod_gpu(r, r, m);
  }
  return r;
}

struct MontCtxGPU {
  std::uint64_t m;
  std::uint64_t m_inv;
  std::uint64_t r2;
  std::uint64_t one;
  std::uint64_t nm1;
};

__device__ __forceinline__ MontCtxGPU mont_init_gpu(std::uint64_t m) {
  MontCtxGPU ctx{m, mont_compute_m_inv(m), mont_compute_r2(m), 0ULL, 0ULL};
  ctx.one = mont_mul_gpu(1ULL, ctx.r2, ctx.m, ctx.m_inv);
  ctx.nm1 = mont_mul_gpu(ctx.m - 1ULL, ctx.r2, ctx.m, ctx.m_inv);
  return ctx;
}

__device__ __forceinline__ std::uint64_t mont_to_gpu(
    std::uint64_t a, MontCtxGPU ctx) {
  return mont_mul_gpu(a % ctx.m, ctx.r2, ctx.m, ctx.m_inv);
}

__device__ __forceinline__ std::uint32_t ctz64_gpu(std::uint64_t value) {
  return static_cast<std::uint32_t>(__ffsll(static_cast<long long>(value)) - 1);
}

__device__ __forceinline__ std::uint64_t mont_powmod_gpu(
    std::uint64_t base, std::uint64_t exp, MontCtxGPU ctx) {
  std::uint64_t result = ctx.one;
  std::uint64_t base_mont = mont_to_gpu(base, ctx);
  while (exp != 0ULL) {
    if ((exp & 1ULL) != 0ULL) {
      result = mont_mul_gpu(result, base_mont, ctx.m, ctx.m_inv);
    }
    exp >>= 1;
    if (exp != 0ULL) {
      base_mont = mont_mul_gpu(base_mont, base_mont, ctx.m, ctx.m_inv);
    }
  }
  return result;
}

__device__ __forceinline__ bool miller_rabin_witness_mont_gpu(
    MontCtxGPU ctx, std::uint64_t d, std::uint32_t s, std::uint64_t a) {
  if (a >= ctx.m) {
    a %= ctx.m;
    if (a == 0ULL) {
      return true;
    }
  }

  std::uint64_t x = mont_powmod_gpu(a, d, ctx);
  if (x == ctx.one || x == ctx.nm1) {
    return true;
  }

  for (std::uint32_t r = 1; r < s; ++r) {
    x = mont_mul_gpu(x, x, ctx.m, ctx.m_inv);
    if (x == ctx.nm1) {
      return true;
    }
  }
  return false;
}

__device__ __forceinline__ bool is_prime_fj64_gpu(
    std::uint64_t n, const std::uint16_t* __restrict__ fj64_table) {
  if (n < 2ULL) return false;
  if (n == 2ULL || n == 3ULL) return true;
  if ((n & 1ULL) == 0ULL) return false;

  for (int i = 0; i < NUM_TRIAL_PRIMES; ++i) {
    const std::uint64_t p = static_cast<std::uint64_t>(c_trial_primes[i]);
    if (n == p) return true;
    if ((n % p) == 0ULL) return false;
  }

  std::uint64_t d = n - 1ULL;
  const std::uint32_t s = ctz64_gpu(d);
  d >>= s;

  const MontCtxGPU ctx = mont_init_gpu(n);
  if (!miller_rabin_witness_mont_gpu(ctx, d, s, 2ULL)) {
    return false;
  }

  std::uint64_t h = n;
  h = ((h >> 32) ^ h) * 0x45d9f3b3335b369ULL;
  h = ((h >> 32) ^ h) * 0x3335b36945d9f3bULL;
  h = ((h >> 32) ^ h);
  return miller_rabin_witness_mont_gpu(
      ctx, d, s, static_cast<std::uint64_t>(fj64_table[h & 0x3FFFFULL]));
}

__device__ __forceinline__ std::uint64_t abs_i64_to_u64_gpu(std::int64_t value) {
  return value < 0 ? static_cast<std::uint64_t>(-value)
                   : static_cast<std::uint64_t>(value);
}

__device__ __forceinline__ bool is_axis_gaussian_prime_gpu(
    std::int64_t a, std::int64_t b, const std::uint16_t* __restrict__ fj64_table) {
  if (a == 0) {
    const std::uint64_t mag = abs_i64_to_u64_gpu(b);
    return ((mag & 3ULL) == 3ULL) && is_prime_fj64_gpu(mag, fj64_table);
  }
  if (b == 0) {
    const std::uint64_t mag = abs_i64_to_u64_gpu(a);
    return ((mag & 3ULL) == 3ULL) && is_prime_fj64_gpu(mag, fj64_table);
  }
  return false;
}

}  // namespace cuda_campaign
