#ifndef GM_CORNACCHIA_CUH
#define GM_CORNACCHIA_CUH

// Cornacchia's algorithm — decomposes a prime p = 1 (mod 4) as p = a^2 + b^2.
//
// Port of sieve.rs: cornacchia(), fast_sqrt_neg1(), tonelli_shanks().
//
// Depends on:
//   - modular_arith.cuh: mulmod64, powmod64

#include "modular_arith.cuh"
#include "types.h"

// ============================================================================
// isqrt — integer square root (floor)
// Matches Rust isqrt() exactly: float seed + up/down correction.
// ============================================================================
__host__ __device__ __forceinline__
uint64_t isqrt64(uint64_t n) {
    if (n == 0) return 0;
    uint64_t x = (uint64_t)sqrt((double)n);

    // Correct upward
    while ((__uint128_t)(x + 1) * (x + 1) <= (__uint128_t)n) {
        x++;
    }
    // Correct downward
    while ((__uint128_t)x * x > (__uint128_t)n) {
        x--;
    }
    return x;
}

// ============================================================================
// Tonelli-Shanks: compute sqrt(n) mod p
//
// Direct port of sieve.rs tonelli_shanks(n, p).
// Returns the square root if n is a QR mod p, or UINT64_MAX on failure.
// ============================================================================
__host__ __device__
uint64_t tonelli_shanks(uint64_t n, uint64_t p) {
    if (p == 2) {
        return n % p;
    }

    n = n % p;
    if (n == 0) return 0;

    // Euler criterion: n^((p-1)/2) must be 1
    if (powmod64(n, (p - 1) / 2, p) != 1) {
        return UINT64_MAX;  // not a QR
    }

    // p ≡ 3 (mod 4): direct formula
    if (p % 4 == 3) {
        return powmod64(n, (p + 1) / 4, p);
    }

    // Factor p - 1 = 2^s * q with q odd
    uint64_t q = p - 1;
    uint32_t s = 0;
    while ((q & 1) == 0) {
        q >>= 1;
        s++;
    }

    // Find a non-residue z
    uint64_t z = 2;
    while (powmod64(z, (p - 1) / 2, p) != p - 1) {
        z++;
    }

    uint32_t m = s;
    uint64_t c = powmod64(z, q, p);
    uint64_t t = powmod64(n, q, p);
    // r = n^((q+1)/2) mod p
    uint64_t r = powmod64(n, (q + 1) / 2, p);

    while (t != 1) {
        // Find the least i such that t^(2^i) = 1
        uint32_t i = 1;
        uint64_t t2 = mulmod64(t, t, p);
        while (t2 != 1) {
            t2 = mulmod64(t2, t2, p);
            i++;
            if (i >= m) {
                return UINT64_MAX;  // should not happen for valid input
            }
        }

        uint64_t b = powmod64(c, 1ULL << (m - i - 1), p);
        r = mulmod64(r, b, p);
        c = mulmod64(b, b, p);
        t = mulmod64(t, c, p);
        m = i;
    }

    return r;
}

// ============================================================================
// fast_sqrt_neg1: compute sqrt(-1) mod p for p ≡ 1 (mod 4)
//
// Direct port of sieve.rs fast_sqrt_neg1(p).
// Fast path for p ≡ 5 (mod 8): try r = 2^((p-1)/4) mod p.
// Otherwise, use Tonelli-Shanks with n = p-1 (i.e., sqrt(-1)).
// Returns UINT64_MAX on failure.
// ============================================================================
__host__ __device__
uint64_t fast_sqrt_neg1(uint64_t p) {
    if (p <= 2 || p % 4 != 1) {
        return UINT64_MAX;
    }

    uint64_t exp = (p - 1) >> 2;

    // Fast path for p ≡ 5 (mod 8)
    if (p % 8 == 5) {
        uint64_t r = powmod64(2, exp, p);
        if (mulmod64(r, r, p) == p - 1) {
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
__host__ __device__
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
    __uint128_t b_sq = (__uint128_t)p - (__uint128_t)a * a;
    uint64_t b = isqrt64((uint64_t)b_sq);

    // Verify b^2 == p - a^2 (exact square check)
    if ((__uint128_t)b * b != b_sq) {
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
