#pragma once

#include "gpu_constants.cuh"
#include "gpu_types.cuh"

extern __constant__ SplitPrimeBarrettGPU c_split_barrett[SPLIT_PRIMES_COUNT];
extern __constant__ InertPrimeBarrettGPU c_inert_barrett[INERT_PRIMES_COUNT];
extern __constant__ uint64_t c_mr_witnesses[NUM_MR_WITNESSES];
extern __constant__ uint32_t c_trial_primes[NUM_TRIAL_PRIMES];
extern __constant__ int8_t c_bk_dr[NUM_BACKWARD_OFFSETS];
extern __constant__ int8_t c_bk_dc[NUM_BACKWARD_OFFSETS];

__device__ __forceinline__ int32_t euclidean_mod_gpu(int32_t value, uint32_t modulus) {
    const int32_t mod = static_cast<int32_t>(modulus);
    int32_t rem = value % mod;
    if (rem < 0) {
        rem += mod;
    }
    return rem;
}

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

__device__ __forceinline__ void mark_residue_class_reg(
    uint32_t ws[BITMAP_WORDS_PER_ROW],
    int32_t b_start,
    uint32_t p,
    int32_t residue) {
    const int32_t b_mod = euclidean_mod_gpu(b_start, p);
    const int32_t first_col = euclidean_mod_gpu(residue - b_mod, p);
    for (int32_t col = first_col; col < SIDE_EXP; col += static_cast<int32_t>(p)) {
        ws[col >> 5] |= 1u << (col & 31);
    }
}

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

__device__ __forceinline__ uint64_t addmod_gpu(uint64_t a, uint64_t b, uint64_t m) {
    return (a >= m - b) ? (a - (m - b)) : (a + b);
}

__device__ __forceinline__ uint64_t mont_mul_gpu(uint64_t a, uint64_t b, uint64_t m, uint64_t m_inv) {
    const uint64_t lo = a * b;
    const uint64_t hi = __umul64hi(a, b);
    const uint64_t q = lo * m_inv;
    const uint64_t qm_lo = q * m;
    const uint64_t qm_hi = __umul64hi(q, m);
    const uint64_t carry = (lo + qm_lo < lo) ? 1ULL : 0ULL;
    const uint64_t r = hi + qm_hi + carry;
    return (r >= m) ? (r - m) : r;
}

__device__ __forceinline__ uint64_t mont_compute_m_inv(uint64_t m) {
    uint64_t x = 1ULL;
    for (uint32_t i = 0; i < 6; ++i) {
        x *= 2ULL - m * x;
    }
    return 0ULL - x;
}

__device__ __forceinline__ uint64_t mont_compute_r2(uint64_t m) {
    // m > 1 always for MR candidates, so 1ULL % m == 1.
    uint64_t r = 1ULL;
    for (uint32_t i = 0; i < 128; ++i) {
        r = addmod_gpu(r, r, m);
    }
    return r;
}

struct MontCtxGPU {
    uint64_t m;
    uint64_t m_inv;
    uint64_t r2;
    uint64_t one;
    uint64_t nm1;
};

__device__ __forceinline__ MontCtxGPU mont_init_gpu(uint64_t m) {
    MontCtxGPU ctx{m, mont_compute_m_inv(m), mont_compute_r2(m), 0ULL, 0ULL};
    // m > 1 always: 1ULL % m == 1, (m-1) % m == m-1.
    ctx.one = mont_mul_gpu(1ULL, ctx.r2, ctx.m, ctx.m_inv);
    ctx.nm1 = mont_mul_gpu(ctx.m - 1ULL, ctx.r2, ctx.m, ctx.m_inv);
    return ctx;
}

__device__ __forceinline__ uint64_t mont_to_gpu(uint64_t a, MontCtxGPU ctx) {
    return mont_mul_gpu(a % ctx.m, ctx.r2, ctx.m, ctx.m_inv);
}

__device__ __forceinline__ uint32_t ctz64_gpu(uint64_t value) {
    return static_cast<uint32_t>(__ffsll(static_cast<long long>(value)) - 1);
}

__device__ __forceinline__ uint64_t mont_powmod_gpu(uint64_t base, uint64_t exp, MontCtxGPU ctx) {
    uint64_t result = ctx.one;
    uint64_t base_mont = mont_to_gpu(base, ctx);
    while (exp != 0ULL) {
        if ((exp & 1ULL) != 0ULL) {
            result = mont_mul_gpu(result, base_mont, ctx.m, ctx.m_inv);
        }
        base_mont = mont_mul_gpu(base_mont, base_mont, ctx.m, ctx.m_inv);
        exp >>= 1;
    }
    return result;
}

__device__ __forceinline__ bool miller_rabin_witness_mont_gpu(
    MontCtxGPU ctx,
    uint64_t d,
    uint32_t s,
    uint64_t a) {
    // Reduce base mod n. Needed for Sinclair bases (e.g., 1795265022)
    // which may exceed small n. If a == 0 mod n, witness is trivially
    // "probably prime" — skip (cannot be a witness).
    if (a >= ctx.m) {
        a = a % ctx.m;
        if (a == 0ULL) {
            return true;
        }
    }

    uint64_t x = mont_powmod_gpu(a, d, ctx);
    if (x == ctx.one || x == ctx.nm1) {
        return true;
    }

    for (uint32_t r = 1; r < s; ++r) {
        x = mont_mul_gpu(x, x, ctx.m, ctx.m_inv);
        if (x == ctx.nm1) {
            return true;
        }
    }
    return false;
}

__device__ __forceinline__ bool is_prime_gpu(uint64_t n) {
    if (n < 2ULL) {
        return false;
    }
    if (n == 2ULL || n == 3ULL) {
        return true;
    }
    if ((n & 1ULL) == 0ULL) {
        return false;
    }

    for (int i = 0; i < NUM_TRIAL_PRIMES; ++i) {
        const uint64_t p = static_cast<uint64_t>(c_trial_primes[i]);
        if (n == p) {
            return true;
        }
        if (n % p == 0ULL) {
            return false;
        }
    }

    if (n < 97ULL * 97ULL) {
        return true;
    }

    uint64_t d = n - 1ULL;
    const uint32_t s = ctz64_gpu(d);
    d >>= s;

    const MontCtxGPU ctx = mont_init_gpu(n);
    for (int i = 0; i < NUM_MR_WITNESSES; ++i) {
        if (!miller_rabin_witness_mont_gpu(ctx, d, s, c_mr_witnesses[i])) {
            return false;
        }
    }
    return true;
}

// Optimized MR-only primality test for sieve survivors.
// Skips trial division (sieve already covers primes to 10,000) and
// tiny-number checks (norms at operating radii are always > 10^17).
// Only valid for odd n > 1 that survived the sieve.
__device__ __forceinline__ bool is_prime_norm_gpu(uint64_t n) {
    uint64_t d = n - 1ULL;
    const uint32_t s = ctz64_gpu(d);
    d >>= s;

    const MontCtxGPU ctx = mont_init_gpu(n);
    for (int i = 0; i < NUM_MR_WITNESSES; ++i) {
        if (!miller_rabin_witness_mont_gpu(ctx, d, s, c_mr_witnesses[i])) {
            return false;
        }
    }
    return true;
}

__device__ __forceinline__ uint64_t abs_i32_to_u64_gpu(int32_t value) {
    const int64_t wide = static_cast<int64_t>(value);
    return (wide < 0) ? static_cast<uint64_t>(-wide) : static_cast<uint64_t>(wide);
}

__device__ __forceinline__ bool is_axis_gaussian_prime_gpu(int32_t a, int32_t b) {
    if (a == 0) {
        const uint64_t mag = abs_i32_to_u64_gpu(b);
        return ((mag & 3ULL) == 3ULL) && is_prime_gpu(mag);
    }
    if (b == 0) {
        const uint64_t mag = abs_i32_to_u64_gpu(a);
        return ((mag & 3ULL) == 3ULL) && is_prime_gpu(mag);
    }
    return false;
}
