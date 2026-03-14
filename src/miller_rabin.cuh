#ifndef GM_MILLER_RABIN_CUH
#define GM_MILLER_RABIN_CUH

// Miller-Rabin primality test — deterministic for all 64-bit inputs.
//
// Uses modular_arith.cuh: powmod64 for witness testing.
// Deterministic variant using 7 witnesses: {2, 3, 5, 7, 11, 13, 17}.
// This witness set is sufficient for all n < 3.317 * 10^24, covering our
// entire 64-bit range.
//
// Ported from gaussian-moat-solver-final/src/sieve.rs logic (the Rust solver
// uses a sieve-based approach, but this Miller-Rabin serves as the per-candidate
// primality test for the CUDA kernel).

#include "modular_arith.cuh"

// The 7 deterministic witnesses sufficient for 64-bit inputs.
// Reference: https://miller-rabin.appspot.com/
__constant__ static const uint64_t MR_WITNESSES[7] = {2, 3, 5, 7, 11, 13, 17};
static const int MR_NUM_WITNESSES = 7;

// Single-witness Miller-Rabin test.
// Returns true if n is "probably prime" for witness a.
// Returns false if n is definitely composite for witness a.
__host__ __device__ __forceinline__
bool miller_rabin_witness(uint64_t n, uint64_t d, uint32_t s, uint64_t a) {
    // Skip if witness >= n (would give trivial result)
    if (a >= n) return true;

    uint64_t x = powmod64(a, d, n);

    // If x == 1 or x == n-1, pass this witness
    if (x == 1 || x == n - 1) return true;

    // Square s-1 times, checking for n-1
    for (uint32_t r = 1; r < s; ++r) {
        x = mulmod64(x, x, n);
        if (x == n - 1) return true;
    }

    // Composite for this witness
    return false;
}

// Deterministic Miller-Rabin primality test for 64-bit unsigned integers.
// Returns true if n is prime, false if composite.
__host__ __device__
bool is_prime(uint64_t n) {
    // Handle small cases
    if (n < 2) return false;
    if (n == 2 || n == 3) return true;
    if (n % 2 == 0) return false;
    if (n == 5 || n == 7 || n == 11 || n == 13 || n == 17) return true;

    // Write n - 1 = 2^s * d with d odd
    uint64_t d = n - 1;
    uint32_t s = 0;
    while ((d & 1) == 0) {
        d >>= 1;
        s++;
    }

    // Test all 7 witnesses
    // Unrolled for GPU performance (avoids array access overhead on device)
    if (!miller_rabin_witness(n, d, s, 2))  return false;
    if (!miller_rabin_witness(n, d, s, 3))  return false;
    if (!miller_rabin_witness(n, d, s, 5))  return false;
    if (!miller_rabin_witness(n, d, s, 7))  return false;
    if (!miller_rabin_witness(n, d, s, 11)) return false;
    if (!miller_rabin_witness(n, d, s, 13)) return false;
    if (!miller_rabin_witness(n, d, s, 17)) return false;

    return true;
}

#endif // GM_MILLER_RABIN_CUH
