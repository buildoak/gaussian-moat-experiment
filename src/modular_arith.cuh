#ifndef GM_MODULAR_ARITH_CUH
#define GM_MODULAR_ARITH_CUH

// Modular arithmetic primitives for 64-bit unsigned integers.
// Ported from gaussian-moat-solver-final/src/sieve.rs: mod_mul, mod_pow.
//
// Three mulmod64 variants are provided because __int128 may not be available
// or efficient on all GPU targets. Cross-variant agreement is validated in
// test_modular.cu.
//
// Additionally, mulmod_small / powmod_small are provided for moduli < 2^32,
// which avoids all 128-bit arithmetic — a*b fits in 64 bits when a,b < m < 2^32.

#include <cstdint>

// ============================================================================
// Variant 1: __int128 cast
//   Matches the Rust source exactly: ((a as u128 * b as u128) % m as u128) as u64
//   Available on host and device (GCC/Clang support __int128 on aarch64).
//   On CUDA device code __int128 is a compiler extension — works on nvcc for
//   sm_50+ but falls back to software emulation (PTX mul.wide.u64 doesn't
//   exist). Still correct, just slower than Variant 2 on GPU.
// ============================================================================
__host__ __device__ __forceinline__
uint64_t mulmod64_v1(uint64_t a, uint64_t b, uint64_t m) {
    return (unsigned __int128)a * b % m;
}

// ============================================================================
// Variant 2: __umul64hi intrinsic
//   Uses CUDA's __umul64hi(a, b) to get the upper 64 bits of a*b, then
//   manually reduces the 128-bit product (hi:lo) mod m via a Barrett-style
//   approach using repeated subtraction / shift-subtract.
//
//   On host we fall back to __int128 for the hi computation.
// ============================================================================

// Reduce a 128-bit value (hi:lo) mod m, where m is 64-bit.
// Uses the standard shift-and-subtract algorithm (schoolbook division).
__host__ __device__ __forceinline__
uint64_t reduce128(uint64_t hi, uint64_t lo, uint64_t m) {
    // If hi == 0, simple case
    if (hi == 0) {
        return lo % m;
    }

    // Compute (hi * 2^64 + lo) % m by iterating over all 128 bits MSB-first.
    // Each step: rem = 2*rem + bit, reduced mod m.
    uint64_t rem = 0;
    for (int i = 63; i >= 0; --i) {
        // Double
        if (rem >= m - rem) {
            rem = rem - (m - rem);  // rem = 2*rem - m
        } else {
            rem <<= 1;
        }
        // Add bit
        if ((hi >> i) & 1ULL) {
            rem += 1;
            if (rem >= m) rem -= m;
        }
    }
    for (int i = 63; i >= 0; --i) {
        if (rem >= m - rem) {
            rem = rem - (m - rem);
        } else {
            rem <<= 1;
        }
        if ((lo >> i) & 1ULL) {
            rem += 1;
            if (rem >= m) rem -= m;
        }
    }

    return rem;
}

__host__ __device__ __forceinline__
uint64_t mulmod64_v2(uint64_t a, uint64_t b, uint64_t m) {
    if (m == 0) return 0;
    a %= m;
    b %= m;

#ifdef __CUDA_ARCH__
    uint64_t hi = __umul64hi(a, b);
    uint64_t lo = a * b;
#else
    unsigned __int128 prod = (unsigned __int128)a * b;
    uint64_t hi = (uint64_t)(prod >> 64);
    uint64_t lo = (uint64_t)prod;
#endif

    return reduce128(hi, lo, m);
}

// ============================================================================
// Variant 3: Russian peasant multiplication
//   Bit-by-bit addmod loop. Guaranteed correct for all inputs — no overflow
//   possible since we only ever add values < m. Slowest variant but serves
//   as the ground-truth reference.
//
//   Russian peasant: for each bit of b (MSB to LSB), double the accumulator
//   and conditionally add a. All operations mod m.
// ============================================================================
__host__ __device__ __forceinline__
uint64_t addmod64(uint64_t a, uint64_t b, uint64_t m) {
    // a, b < m guaranteed by caller
    uint64_t r = a + b;
    if (r >= m || r < a) {  // r < a catches unsigned overflow
        r -= m;
    }
    return r;
}

__host__ __device__ __forceinline__
uint64_t mulmod64_v3(uint64_t a, uint64_t b, uint64_t m) {
    if (m <= 1) return 0;
    a %= m;
    b %= m;

    uint64_t result = 0;

    // Find highest set bit of b
    // Process bits of b from MSB to LSB
    for (int i = 63; i >= 0; --i) {
        // result = result * 2 mod m
        result = addmod64(result, result, m);

        // If bit i of b is set, result = (result + a) mod m
        if ((b >> i) & 1ULL) {
            result = addmod64(result, a, m);
        }
    }

    return result;
}

// ============================================================================
// Default mulmod64 — selects Variant 1 (__int128)
//
//   __int128 is the fastest correct path for all architectures. On CUDA
//   device code, nvcc compiles __int128 multiplication to a tight sequence
//   of MUL/MADC instructions — no 128-iteration loop. This is faster than
//   Variant 2's __umul64hi + reduce128 (which uses a 128-iteration
//   bit-by-bit schoolbook division loop).
//
//   Variant 2 (__umul64hi + reduce128) exists for reference/testing but
//   should NOT be used as the default — the reduce128 loop is the bottleneck.
//
//   Matches the Rust source directly: ((a as u128 * b as u128) % m as u128) as u64
// ============================================================================
__host__ __device__ __forceinline__
uint64_t mulmod64(uint64_t a, uint64_t b, uint64_t m) {
    return mulmod64_v1(a, b, m);
}

// ============================================================================
// Montgomery multiplication helpers
//   Used by Miller-Rabin for large odd moduli to avoid repeated 128-bit
//   division during modular exponentiation. The existing mulmod variants stay
//   available for direct testing and for non-Montgomery callers.
// ============================================================================
struct MontgomeryParams {
    uint64_t n;
    uint64_t n_inv;  // -n^(-1) mod 2^64
    uint64_t r2;     // (2^64)^2 mod n
};

__host__ __device__ __forceinline__
uint64_t mulhi64(uint64_t a, uint64_t b) {
#ifdef __CUDA_ARCH__
    return __umul64hi(a, b);
#else
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
#endif
}

__host__ __device__ __forceinline__
MontgomeryParams montgomery_init(uint64_t n) {
    uint64_t inv = 1;
    for (int i = 0; i < 6; ++i) {
        inv *= 2 - n * inv;
    }

    // 2^64 mod n represented as the 128-bit value (1:0) reduced modulo n.
    uint64_t r = reduce128(1, 0, n);

    MontgomeryParams params = {n, (uint64_t)(0 - inv), mulmod64_v2(r, r, n)};
    return params;
}

__host__ __device__ __forceinline__
uint64_t mont_mul(uint64_t a, uint64_t b, uint64_t n, uint64_t n_inv) {
    uint64_t t_lo = a * b;
    uint64_t t_hi = mulhi64(a, b);

    uint64_t m = t_lo * n_inv;
    uint64_t mn_lo = m * n;
    uint64_t mn_hi = mulhi64(m, n);

    uint64_t u_lo = t_lo + mn_lo;
    uint64_t carry = (u_lo < t_lo) ? 1ULL : 0ULL;
    uint64_t u = t_hi + mn_hi + carry;

    return (u >= n) ? u - n : u;
}

__host__ __device__ __forceinline__
uint64_t mont_to(uint64_t a, const MontgomeryParams& p) {
    return mont_mul(a % p.n, p.r2, p.n, p.n_inv);
}

__host__ __device__ __forceinline__
uint64_t mont_from(uint64_t a, const MontgomeryParams& p) {
    return mont_mul(a, 1, p.n, p.n_inv);
}

__host__ __device__ __forceinline__
uint64_t mont_powmod_mont(uint64_t base, uint64_t exp, const MontgomeryParams& p) {
    uint64_t result = mont_to(1, p);
    uint64_t factor = mont_to(base, p);

    while (exp > 0) {
        if (exp & 1ULL) {
            result = mont_mul(result, factor, p.n, p.n_inv);
        }
        factor = mont_mul(factor, factor, p.n, p.n_inv);
        exp >>= 1;
    }

    return result;
}

__host__ __device__ __forceinline__
uint64_t mont_powmod(uint64_t base, uint64_t exp, const MontgomeryParams& p) {
    return mont_from(mont_powmod_mont(base, exp, p), p);
}

// ============================================================================
// Small-modulus fast path: mulmod_small / powmod_small
//   When m < 2^32, a and b (already reduced mod m) each fit in 32 bits.
//   Their product a*b < 2^64, so no 128-bit arithmetic is needed at all.
//   This eliminates the most expensive part of MR for our norm ranges.
// ============================================================================
__host__ __device__ __forceinline__
uint64_t mulmod_small(uint64_t a, uint64_t b, uint64_t m) {
    // Caller guarantees a, b < m < 2^32, so a*b < 2^64.
    return (a * b) % m;
}

__host__ __device__ __forceinline__
uint64_t powmod_small(uint64_t base, uint64_t exp, uint64_t m) {
    if (m == 1) return 0;
    uint64_t result = 1;
    base %= m;
    while (exp > 0) {
        if (exp & 1ULL) {
            result = mulmod_small(result, base, m);
        }
        base = mulmod_small(base, base, m);
        exp >>= 1;
    }
    return result;
}

// ============================================================================
// powmod64 — binary exponentiation using mulmod64
//   Direct port of Rust mod_pow(base, exp, m).
// ============================================================================
__host__ __device__ __forceinline__
uint64_t powmod64(uint64_t base, uint64_t exp, uint64_t m) {
    if (m == 1) return 0;

    uint64_t result = 1;
    base %= m;

    while (exp > 0) {
        if (exp & 1ULL) {
            result = mulmod64(result, base, m);
        }
        base = mulmod64(base, base, m);
        exp >>= 1;
    }
    return result;
}

// Variant-parameterized powmod for testing: uses a specific mulmod variant
template <uint64_t (*MulModFn)(uint64_t, uint64_t, uint64_t)>
__host__ __device__ __forceinline__
uint64_t powmod64_with(uint64_t base, uint64_t exp, uint64_t m) {
    if (m == 1) return 0;

    uint64_t result = 1;
    base %= m;

    while (exp > 0) {
        if (exp & 1ULL) {
            result = MulModFn(result, base, m);
        }
        base = MulModFn(base, base, m);
        exp >>= 1;
    }
    return result;
}

#endif // GM_MODULAR_ARITH_CUH
