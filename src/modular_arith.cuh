#ifndef GM_MODULAR_ARITH_CUH
#define GM_MODULAR_ARITH_CUH

// Modular arithmetic primitives for 64-bit unsigned integers.
// Ported from gaussian-moat-solver-final/src/sieve.rs: mod_mul, mod_pow.
//
// Three mulmod64 variants are provided because __int128 may not be available
// or efficient on all GPU targets. Cross-variant agreement is validated in
// test_modular.cu.

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
//   This is the fastest correct path and matches the Rust source directly.
// ============================================================================
__host__ __device__ __forceinline__
uint64_t mulmod64(uint64_t a, uint64_t b, uint64_t m) {
    return mulmod64_v1(a, b, m);
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
