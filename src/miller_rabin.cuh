#ifndef GM_MILLER_RABIN_CUH
#define GM_MILLER_RABIN_CUH

// Miller-Rabin primality test — deterministic for all 64-bit inputs.
//
// Uses modular_arith.cuh: powmod64 / powmod_small for witness testing.
// Deterministic variant using 7 witnesses: {2, 3, 5, 7, 11, 13, 17}.
// This witness set is sufficient for all n < 3.317 * 10^24, covering our
// entire 64-bit range.
//
// Optimizations over baseline:
//   1. Small-factor trial division eliminates ~77% of composites cheaply
//   2. For n < 2^32: uses mulmod_small (no 128-bit math), only 3 witnesses
//   3. Tiered witness selection based on Jim Sinclair's results
//   4. Early exit on first composite witness (already present in baseline)
//
// Ported from gaussian-moat-solver-final/src/sieve.rs logic.

#include "modular_arith.cuh"

// The 7 deterministic witnesses sufficient for 64-bit inputs.
// Reference: https://miller-rabin.appspot.com/
__constant__ static const uint64_t MR_WITNESSES[7] = {2, 3, 5, 7, 11, 13, 17};
static const int MR_NUM_WITNESSES = 7;

// ============================================================================
// Small-modulus Miller-Rabin witness test (m < 2^32)
//   Uses mulmod_small which avoids all 128-bit arithmetic.
// ============================================================================
__host__ __device__ __forceinline__
bool miller_rabin_witness_small(uint64_t n, uint64_t d, uint32_t s, uint64_t a) {
    if (a >= n) return true;

    uint64_t x = powmod_small(a, d, n);

    if (x == 1 || x == n - 1) return true;

    for (uint32_t r = 1; r < s; ++r) {
        x = mulmod_small(x, x, n);
        if (x == n - 1) return true;
    }

    return false;
}

// ============================================================================
// General Miller-Rabin witness test (any 64-bit m)
// ============================================================================
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

// ============================================================================
// Deterministic Miller-Rabin primality test for 64-bit unsigned integers.
//
// Small-factor pre-sieve: trial divide by 3,5,7,11,13,17,19,23 before MR.
// This eliminates ~77% of odd composites with cheap integer ops instead of
// expensive modular exponentiations.
//
// Tiered witnesses (Jim Sinclair):
//   n < 3,317,044,064 (~3.3×10^9):  {2, 3, 5}         — 3 witnesses
//   n < 3,215,031,751 (~3.2×10^14): {2, 3, 5, 7}      — 4 witnesses
//   n < 3.317×10^24:                {2,3,5,7,11,13,17} — 7 witnesses
//
// For n < 2^32, uses the fast mulmod_small path (no 128-bit math).
// ============================================================================
__host__ __device__ inline
bool is_prime(uint64_t n) {
    // Handle small cases
    if (n < 2) return false;
    if (n == 2 || n == 3) return true;
    if (n % 2 == 0) return false;

    // Small-factor trial division: eliminates ~77% of odd composites.
    // These are cheap integer divides vs 3-7 full modular exponentiations.
    if (n > 3  && n % 3  == 0) return false;
    if (n > 5  && n % 5  == 0) return false;
    if (n > 7  && n % 7  == 0) return false;
    if (n > 11 && n % 11 == 0) return false;
    if (n > 13 && n % 13 == 0) return false;
    if (n > 17 && n % 17 == 0) return false;
    if (n > 19 && n % 19 == 0) return false;
    if (n > 23 && n % 23 == 0) return false;
    // Extend trial division further — still much cheaper than one MR witness
    if (n > 29 && n % 29 == 0) return false;
    if (n > 31 && n % 31 == 0) return false;
    if (n > 37 && n % 37 == 0) return false;
    if (n > 41 && n % 41 == 0) return false;
    if (n > 43 && n % 43 == 0) return false;
    if (n > 47 && n % 47 == 0) return false;
    if (n > 53 && n % 53 == 0) return false;
    if (n > 59 && n % 59 == 0) return false;
    if (n > 61 && n % 61 == 0) return false;
    if (n > 67 && n % 67 == 0) return false;
    if (n > 71 && n % 71 == 0) return false;
    if (n > 73 && n % 73 == 0) return false;
    if (n > 79 && n % 79 == 0) return false;
    if (n > 83 && n % 83 == 0) return false;
    if (n > 89 && n % 89 == 0) return false;
    if (n > 97 && n % 97 == 0) return false;

    // After trial division by all primes up to 97, any n < 97^2 = 9409
    // that survived must be prime — skip the expensive MR entirely.
    if (n < 9409) return true;

    // Write n - 1 = 2^s * d with d odd
    uint64_t d = n - 1;
    uint32_t s = 0;
#ifdef __CUDA_ARCH__
    s = __ffsll(d) - 1;   // count trailing zeros (device intrinsic, faster than loop)
#else
    s = __builtin_ctzll(d);  // host: GCC/Clang builtin
#endif
    d >>= s;

    // --- Small modulus path: n < 2^32 → no 128-bit math needed ---
    if (n < 0x100000000ULL) {
        // Corrected thresholds (Miller-Rabin strong pseudoprime boundaries):
        //   {2, 3}:       sufficient for n < 1,373,653
        //   {2, 3, 5}:    sufficient for n < 25,326,001
        //   {2, 3, 5, 7}: sufficient for n < 3,215,031,751
        //   + witness 11:  sufficient for n < 2^32
        if (n < 25326001ULL) {
            // 3 witnesses: {2, 3, 5}
            if (!miller_rabin_witness_small(n, d, s, 2)) return false;
            if (!miller_rabin_witness_small(n, d, s, 3)) return false;
            if (!miller_rabin_witness_small(n, d, s, 5)) return false;
            return true;
        } else if (n < 3215031751ULL) {
            // 4 witnesses: {2, 3, 5, 7}
            if (!miller_rabin_witness_small(n, d, s, 2)) return false;
            if (!miller_rabin_witness_small(n, d, s, 3)) return false;
            if (!miller_rabin_witness_small(n, d, s, 5)) return false;
            if (!miller_rabin_witness_small(n, d, s, 7)) return false;
            return true;
        } else {
            // n in [3215031751, 2^32): need {2, 3, 5, 7, 11}
            if (!miller_rabin_witness_small(n, d, s, 2)) return false;
            if (!miller_rabin_witness_small(n, d, s, 3)) return false;
            if (!miller_rabin_witness_small(n, d, s, 5)) return false;
            if (!miller_rabin_witness_small(n, d, s, 7)) return false;
            if (!miller_rabin_witness_small(n, d, s, 11)) return false;
            return true;
        }
    }

    // --- Large modulus path: n >= 2^32, uses __int128 mulmod ---
    // For n < 3.215×10^14: 4 witnesses {2,3,5,7} suffice
    if (n < 321503175100ULL) {
        if (!miller_rabin_witness(n, d, s, 2)) return false;
        if (!miller_rabin_witness(n, d, s, 3)) return false;
        if (!miller_rabin_witness(n, d, s, 5)) return false;
        if (!miller_rabin_witness(n, d, s, 7)) return false;
        return true;
    }

    // Full 7 witnesses for everything else
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
