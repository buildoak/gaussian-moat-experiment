#ifndef GM_CORNACCHIA_CUH
#define GM_CORNACCHIA_CUH

// Cornacchia's algorithm — decomposes a prime p = 1 (mod 4) as p = a^2 + b^2.
//
// Port of sieve.rs: cornacchia(), fast_sqrt_neg1(), tonelli_shanks().
//
// Depends on:
//   - modular_arith.cuh: mulmod64, powmod64, mulmod_small, powmod_small

#include "modular_arith.cuh"
#include "types.h"

// ============================================================================
// isqrt — integer square root (floor)
// Matches Rust isqrt() exactly: float seed + up/down correction.
//
// For n < 2^32, uses pure 64-bit arithmetic (no __uint128_t) since
// x*x fits in 64 bits when x < 2^16. For n < 2^32, x < 2^16.
// ============================================================================
__host__ __device__ __forceinline__
uint64_t isqrt64(uint64_t n) {
    if (n == 0) return 0;
    uint64_t x = (uint64_t)sqrt((double)n);

    if (n < 0x100000000ULL) {
        // Small path: x < 2^16, so (x+1)^2 < 2^32, fits in uint64_t
        while ((x + 1) * (x + 1) <= n) x++;
        while (x * x > n) x--;
    } else {
        // Large path: need 128-bit to avoid overflow
        while ((__uint128_t)(x + 1) * (x + 1) <= (__uint128_t)n) x++;
        while ((__uint128_t)x * x > (__uint128_t)n) x--;
    }
    return x;
}

// ============================================================================
// Tonelli-Shanks: compute sqrt(n) mod p
//
// For p < 2^32, uses the fast mulmod_small/powmod_small path.
// For larger p, uses the general 128-bit path.
// ============================================================================
__host__ __device__ inline
uint64_t tonelli_shanks(uint64_t n, uint64_t p) {
    if (p == 2) {
        return n % p;
    }

    n = n % p;
    if (n == 0) return 0;

    // Select fast vs general path
    bool small_p = (p < 0x100000000ULL);

    // Euler criterion: n^((p-1)/2) must be 1
    uint64_t euler = small_p ? powmod_small(n, (p - 1) / 2, p)
                             : powmod64(n, (p - 1) / 2, p);
    if (euler != 1) {
        return UINT64_MAX;  // not a QR
    }

    // p ≡ 3 (mod 4): direct formula
    if (p % 4 == 3) {
        return small_p ? powmod_small(n, (p + 1) / 4, p)
                       : powmod64(n, (p + 1) / 4, p);
    }

    // Factor p - 1 = 2^s * q with q odd
    uint64_t q = p - 1;
    uint32_t s;
#ifdef __CUDA_ARCH__
    s = __ffsll(q) - 1;   // count trailing zeros (device intrinsic)
#else
    s = __builtin_ctzll(q);  // host: GCC/Clang builtin
#endif
    q >>= s;

    // Find a non-residue z
    uint64_t z = 2;
    while (true) {
        uint64_t check = small_p ? powmod_small(z, (p - 1) / 2, p)
                                 : powmod64(z, (p - 1) / 2, p);
        if (check == p - 1) break;
        z++;
    }

    uint32_t m = s;
    uint64_t c = small_p ? powmod_small(z, q, p) : powmod64(z, q, p);
    uint64_t t = small_p ? powmod_small(n, q, p) : powmod64(n, q, p);
    uint64_t r = small_p ? powmod_small(n, (q + 1) / 2, p) : powmod64(n, (q + 1) / 2, p);

    if (small_p) {
        while (t != 1) {
            uint32_t i = 1;
            uint64_t t2 = mulmod_small(t, t, p);
            while (t2 != 1) {
                t2 = mulmod_small(t2, t2, p);
                i++;
                if (i >= m) return UINT64_MAX;
            }
            uint64_t b = powmod_small(c, 1ULL << (m - i - 1), p);
            r = mulmod_small(r, b, p);
            c = mulmod_small(b, b, p);
            t = mulmod_small(t, c, p);
            m = i;
        }
    } else {
        while (t != 1) {
            uint32_t i = 1;
            uint64_t t2 = mulmod64(t, t, p);
            while (t2 != 1) {
                t2 = mulmod64(t2, t2, p);
                i++;
                if (i >= m) return UINT64_MAX;
            }
            uint64_t b = powmod64(c, 1ULL << (m - i - 1), p);
            r = mulmod64(r, b, p);
            c = mulmod64(b, b, p);
            t = mulmod64(t, c, p);
            m = i;
        }
    }

    return r;
}

// ============================================================================
// fast_sqrt_neg1: compute sqrt(-1) mod p for p ≡ 1 (mod 4)
//
// Fast path for p ≡ 5 (mod 8): try r = 2^((p-1)/4) mod p.
// Otherwise, use Tonelli-Shanks with n = p-1 (i.e., sqrt(-1)).
// ============================================================================
__host__ __device__ inline
uint64_t fast_sqrt_neg1(uint64_t p) {
    if (p <= 2 || p % 4 != 1) {
        return UINT64_MAX;
    }

    bool small_p = (p < 0x100000000ULL);
    uint64_t exp = (p - 1) >> 2;

    // Fast path for p ≡ 5 (mod 8)
    if (p % 8 == 5) {
        uint64_t r = small_p ? powmod_small(2, exp, p) : powmod64(2, exp, p);
        uint64_t r2 = small_p ? mulmod_small(r, r, p) : mulmod64(r, r, p);
        if (r2 == p - 1) {
            return r;
        }
    }

    // General case: find sqrt(p-1) mod p = sqrt(-1) mod p
    return tonelli_shanks(p - 1, p);
}

// ============================================================================
// cornacchia: decompose prime p ≡ 1 (mod 4) as p = a^2 + b^2
//
// Direct port of sieve.rs cornacchia(p).
// Returns true and writes (a, b) with a > b > 0 on success.
// Returns false if decomposition fails (should not happen for valid primes).
// ============================================================================
__host__ __device__ inline
bool cornacchia(uint64_t p, int32_t* out_a, int32_t* out_b) {
    if (p <= 2 || p % 4 != 1) {
        return false;
    }

    uint64_t r = fast_sqrt_neg1(p);
    if (r == UINT64_MAX) {
        return false;
    }

    // Ensure r > p/2 (Rust: if r <= p/2 { r = p - r })
    if (r <= p / 2) {
        r = p - r;
    }

    // Euclidean reduction: gcd-style iteration until r1 <= sqrt(p)
    uint64_t limit = isqrt64(p);
    uint64_t r0 = p;
    uint64_t r1 = r;
    while (r1 > limit) {
        uint64_t tmp = r0 % r1;
        r0 = r1;
        r1 = tmp;
    }

    uint64_t a = r1;
    // b^2 = p - a^2
    // For p < 2^32: a < sqrt(p) < 2^16, so a*a < 2^32 fits in uint64_t
    uint64_t b_sq = p - a * a;
    uint64_t b = isqrt64(b_sq);

    // Verify b^2 == p - a^2 (exact square check)
    if (b * b != b_sq) {
        return false;
    }

    // Ensure a >= b (Rust convention: return (max, min))
    if (a >= b) {
        *out_a = (int32_t)a;
        *out_b = (int32_t)b;
    } else {
        *out_a = (int32_t)b;
        *out_b = (int32_t)a;
    }

    return true;
}

#endif // GM_CORNACCHIA_CUH
