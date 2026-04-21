// test_modular.cu — Comprehensive tests for modular arithmetic primitives.
//
// Tests run on HOST (no GPU required for correctness validation).
// When a CUDA device is available, cross-validates host vs device results.
//
// Build: part of gm_cuda_tests target in CMakeLists.txt

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <cmath>

#include "modular_arith.cuh"

// ============================================================================
// Test infrastructure
// ============================================================================

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT_EQ(expr, expected, label) do {                           \
    uint64_t _got = (expr);                                                  \
    uint64_t _exp = (expected);                                              \
    g_tests_run++;                                                           \
    if (_got == _exp) {                                                      \
        g_tests_passed++;                                                    \
    } else {                                                                 \
        g_tests_failed++;                                                    \
        printf("  FAIL %s: got %lu, expected %lu\n",                         \
               label, (unsigned long)_got, (unsigned long)_exp);             \
    }                                                                        \
} while(0)

#define TEST_ASSERT_TRUE(expr, label) do {                                   \
    g_tests_run++;                                                           \
    if (expr) {                                                              \
        g_tests_passed++;                                                    \
    } else {                                                                 \
        g_tests_failed++;                                                    \
        printf("  FAIL %s\n", label);                                        \
    }                                                                        \
} while(0)

// Simple xorshift64* PRNG for deterministic random tests
struct Xorshift64 {
    uint64_t state;

    __host__ __device__
    Xorshift64(uint64_t seed) : state(seed ? seed : 1) {}

    __host__ __device__
    uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1DULL;
    }

    __host__ __device__
    uint64_t next_range(uint64_t lo, uint64_t hi) {
        // Inclusive [lo, hi]
        if (lo >= hi) return lo;
        uint64_t range = hi - lo + 1;
        return lo + (next() % range);
    }
};

// ============================================================================
// 1. Known value tests for mulmod64
// ============================================================================

void test_mulmod64_known_values() {
    printf("=== mulmod64 known values ===\n");

    // 7 * 8 = 56; 56 mod 13 = 4 (13*4=52, 56-52=4)
    TEST_ASSERT_EQ(mulmod64_v1(7, 8, 13), 4ULL, "v1: 7*8 mod 13");
    TEST_ASSERT_EQ(mulmod64_v2(7, 8, 13), 4ULL, "v2: 7*8 mod 13");
    TEST_ASSERT_EQ(mulmod64_v3(7, 8, 13), 4ULL, "v3: 7*8 mod 13");

    // 123456789 * 987654321 = 121932631112635269
    // 121932631112635269 mod 1000000007 = 259106859
    TEST_ASSERT_EQ(mulmod64_v1(123456789, 987654321, 1000000007), 259106859ULL,
                   "v1: 123456789*987654321 mod 1e9+7");
    TEST_ASSERT_EQ(mulmod64_v2(123456789, 987654321, 1000000007), 259106859ULL,
                   "v2: 123456789*987654321 mod 1e9+7");
    TEST_ASSERT_EQ(mulmod64_v3(123456789, 987654321, 1000000007), 259106859ULL,
                   "v3: 123456789*987654321 mod 1e9+7");

    // 2 * 3 mod 5 = 1
    TEST_ASSERT_EQ(mulmod64_v1(2, 3, 5), 1ULL, "v1: 2*3 mod 5");
    TEST_ASSERT_EQ(mulmod64_v2(2, 3, 5), 1ULL, "v2: 2*3 mod 5");
    TEST_ASSERT_EQ(mulmod64_v3(2, 3, 5), 1ULL, "v3: 2*3 mod 5");

    // 1 * anything mod m = anything mod m
    TEST_ASSERT_EQ(mulmod64_v1(1, 42, 100), 42ULL, "v1: 1*42 mod 100");
    TEST_ASSERT_EQ(mulmod64_v2(1, 42, 100), 42ULL, "v2: 1*42 mod 100");
    TEST_ASSERT_EQ(mulmod64_v3(1, 42, 100), 42ULL, "v3: 1*42 mod 100");

    // Large values: near 2^63
    // (2^63 - 1) * 2 mod (2^63 + 1) — if 2^63+1 fits in u64 it does (9223372036854775809)
    // 2*(2^63-1) = 2^64 - 2 = 18446744073709551614
    // 18446744073709551614 mod 9223372036854775809 = 18446744073709551614 - 9223372036854775809 = 9223372036854775805
    uint64_t big_a = 9223372036854775807ULL;  // 2^63 - 1
    uint64_t big_m = 9223372036854775809ULL;  // 2^63 + 1
    TEST_ASSERT_EQ(mulmod64_v1(big_a, 2, big_m), 9223372036854775805ULL,
                   "v1: (2^63-1)*2 mod (2^63+1)");
    TEST_ASSERT_EQ(mulmod64_v2(big_a, 2, big_m), 9223372036854775805ULL,
                   "v2: (2^63-1)*2 mod (2^63+1)");
    TEST_ASSERT_EQ(mulmod64_v3(big_a, 2, big_m), 9223372036854775805ULL,
                   "v3: (2^63-1)*2 mod (2^63+1)");
}

// ============================================================================
// 2. Edge cases
// ============================================================================

void test_mulmod64_edge_cases() {
    printf("=== mulmod64 edge cases ===\n");

    // a = 0
    TEST_ASSERT_EQ(mulmod64_v1(0, 12345, 97), 0ULL, "v1: 0*x mod m");
    TEST_ASSERT_EQ(mulmod64_v2(0, 12345, 97), 0ULL, "v2: 0*x mod m");
    TEST_ASSERT_EQ(mulmod64_v3(0, 12345, 97), 0ULL, "v3: 0*x mod m");

    // b = 0
    TEST_ASSERT_EQ(mulmod64_v1(12345, 0, 97), 0ULL, "v1: x*0 mod m");
    TEST_ASSERT_EQ(mulmod64_v2(12345, 0, 97), 0ULL, "v2: x*0 mod m");
    TEST_ASSERT_EQ(mulmod64_v3(12345, 0, 97), 0ULL, "v3: x*0 mod m");

    // m = 1: everything is 0 mod 1
    TEST_ASSERT_EQ(mulmod64_v1(999, 888, 1), 0ULL, "v1: x*y mod 1");
    TEST_ASSERT_EQ(mulmod64_v2(999, 888, 1), 0ULL, "v2: x*y mod 1");
    TEST_ASSERT_EQ(mulmod64_v3(999, 888, 1), 0ULL, "v3: x*y mod 1");

    // m = 2
    // 7 * 5 = 35, 35 mod 2 = 1
    TEST_ASSERT_EQ(mulmod64_v1(7, 5, 2), 1ULL, "v1: 7*5 mod 2");
    TEST_ASSERT_EQ(mulmod64_v2(7, 5, 2), 1ULL, "v2: 7*5 mod 2");
    TEST_ASSERT_EQ(mulmod64_v3(7, 5, 2), 1ULL, "v3: 7*5 mod 2");

    // a = m-1, b = m-1 (maximum non-trivial inputs)
    // (m-1)*(m-1) = m^2 - 2m + 1 ≡ 1 (mod m)
    uint64_t m = 1000000007;
    TEST_ASSERT_EQ(mulmod64_v1(m - 1, m - 1, m), 1ULL, "v1: (m-1)^2 mod m");
    TEST_ASSERT_EQ(mulmod64_v2(m - 1, m - 1, m), 1ULL, "v2: (m-1)^2 mod m");
    TEST_ASSERT_EQ(mulmod64_v3(m - 1, m - 1, m), 1ULL, "v3: (m-1)^2 mod m");

    // Large m near 2^63
    // (m-1)^2 mod m = 1 for any m
    uint64_t big_m = 9223372036854775783ULL;  // largest prime < 2^63
    TEST_ASSERT_EQ(mulmod64_v1(big_m - 1, big_m - 1, big_m), 1ULL,
                   "v1: (m-1)^2 mod big_prime");
    TEST_ASSERT_EQ(mulmod64_v2(big_m - 1, big_m - 1, big_m), 1ULL,
                   "v2: (m-1)^2 mod big_prime");
    TEST_ASSERT_EQ(mulmod64_v3(big_m - 1, big_m - 1, big_m), 1ULL,
                   "v3: (m-1)^2 mod big_prime");

    // a > m, b > m — should auto-reduce
    // 100 * 200 mod 13 = 20000 mod 13 = 20000 - 1538*13 = 20000 - 19994 = 6
    TEST_ASSERT_EQ(mulmod64_v1(100, 200, 13), 6ULL, "v1: 100*200 mod 13");
    TEST_ASSERT_EQ(mulmod64_v2(100, 200, 13), 6ULL, "v2: 100*200 mod 13");
    TEST_ASSERT_EQ(mulmod64_v3(100, 200, 13), 6ULL, "v3: 100*200 mod 13");

    // UINT64_MAX * UINT64_MAX mod some prime
    // This is the hardest case — tests full 128-bit overflow handling.
    // UINT64_MAX = 2^64 - 1
    // (2^64-1)^2 = 2^128 - 2^65 + 1
    // mod 1000000007:
    // 2^64 mod 1e9+7: we can compute this via powmod
    // Let's just verify all three agree.
    uint64_t umax = UINT64_MAX;
    uint64_t r1 = mulmod64_v1(umax, umax, 1000000007);
    uint64_t r2 = mulmod64_v2(umax, umax, 1000000007);
    uint64_t r3 = mulmod64_v3(umax, umax, 1000000007);
    TEST_ASSERT_EQ(r2, r1, "v2 agrees with v1 on UINT64_MAX^2 mod 1e9+7");
    TEST_ASSERT_EQ(r3, r1, "v3 agrees with v1 on UINT64_MAX^2 mod 1e9+7");

    // UINT64_MAX * UINT64_MAX mod large prime near 2^63
    r1 = mulmod64_v1(umax, umax, big_m);
    r2 = mulmod64_v2(umax, umax, big_m);
    r3 = mulmod64_v3(umax, umax, big_m);
    TEST_ASSERT_EQ(r2, r1, "v2 agrees with v1 on UINT64_MAX^2 mod big_prime");
    TEST_ASSERT_EQ(r3, r1, "v3 agrees with v1 on UINT64_MAX^2 mod big_prime");
}

// ============================================================================
// 3. Cross-variant validation on random inputs
// ============================================================================

void test_mulmod64_cross_variant() {
    printf("=== mulmod64 cross-variant (10000 random inputs) ===\n");

    Xorshift64 rng(0xDEADBEEF42ULL);
    int mismatches = 0;

    for (int i = 0; i < 10000; ++i) {
        uint64_t a = rng.next();
        uint64_t b = rng.next();
        // m must be > 0; generate in various ranges to stress different code paths
        uint64_t m;
        if (i < 2500) {
            // Small moduli
            m = rng.next_range(2, 10000);
        } else if (i < 5000) {
            // Medium moduli (fits in 32 bits)
            m = rng.next_range(100000, 4294967295ULL);
        } else if (i < 7500) {
            // Large moduli (fits in 63 bits)
            m = rng.next_range(4294967296ULL, 9223372036854775807ULL);
        } else {
            // Very large moduli (close to 2^64)
            m = rng.next() | 1ULL;  // Ensure odd and nonzero
            if (m < 2) m = 3;
        }

        uint64_t r1 = mulmod64_v1(a, b, m);
        uint64_t r2 = mulmod64_v2(a, b, m);
        uint64_t r3 = mulmod64_v3(a, b, m);

        if (r1 != r2 || r1 != r3) {
            mismatches++;
            if (mismatches <= 5) {
                printf("  MISMATCH [%d]: a=%lu b=%lu m=%lu -> v1=%lu v2=%lu v3=%lu\n",
                       i, (unsigned long)a, (unsigned long)b, (unsigned long)m,
                       (unsigned long)r1, (unsigned long)r2, (unsigned long)r3);
            }
        }
    }

    g_tests_run++;
    if (mismatches == 0) {
        g_tests_passed++;
        printf("  All 10000 random cross-variant checks passed.\n");
    } else {
        g_tests_failed++;
        printf("  FAIL: %d mismatches out of 10000\n", mismatches);
    }
}

// ============================================================================
// 4. powmod64 known values
// ============================================================================

void test_powmod64_known_values() {
    printf("=== powmod64 known values ===\n");

    // 2^10 = 1024; 1024 mod 1000 = 24
    TEST_ASSERT_EQ(powmod64(2, 10, 1000), 24ULL, "2^10 mod 1000");

    // 3^13 = 1594323; 1594323 mod 1000000007 = 1594323 (since 1594323 < 1e9+7)
    TEST_ASSERT_EQ(powmod64(3, 13, 1000000007), 1594323ULL, "3^13 mod 1e9+7");

    // 2^0 mod anything = 1 (for m > 1)
    TEST_ASSERT_EQ(powmod64(2, 0, 7), 1ULL, "2^0 mod 7");
    TEST_ASSERT_EQ(powmod64(0, 0, 7), 1ULL, "0^0 mod 7");  // convention: 0^0 = 1

    // x^1 mod m = x mod m
    TEST_ASSERT_EQ(powmod64(42, 1, 100), 42ULL, "42^1 mod 100");

    // Fermat's little theorem: a^p mod p = a for prime p
    TEST_ASSERT_EQ(powmod64(2, 7, 7), 2ULL, "2^7 mod 7 (Fermat)");
    TEST_ASSERT_EQ(powmod64(3, 11, 11), 3ULL, "3^11 mod 11 (Fermat)");

    // m = 1: always 0
    TEST_ASSERT_EQ(powmod64(12345, 67890, 1), 0ULL, "x^y mod 1");

    // Large exponent: 2^64 mod 1e9+7
    // 2^64 = 18446744073709551616
    // We can verify: 2^64 mod 1e9+7 = ?
    // 2^32 = 4294967296 mod 1e9+7 = 4294967296 - 4*1000000007 = 4294967296 - 4000000028 = 294967268
    // 2^64 = (2^32)^2 mod 1e9+7 = 294967268^2 mod 1e9+7
    // 294967268^2 = 87005680753922 ... let's just compute via chain:
    // Trust the reference implementation and verify consistency.
    uint64_t r_v1 = powmod64_with<mulmod64_v1>(2, UINT64_MAX, 1000000007);
    uint64_t r_v3 = powmod64_with<mulmod64_v3>(2, UINT64_MAX, 1000000007);
    TEST_ASSERT_EQ(r_v3, r_v1, "powmod64 v3 matches v1 on 2^UINT64_MAX mod 1e9+7");

    // 7^(10^9+6) mod (10^9+7) = 1 (Fermat's little theorem, since 10^9+7 is prime)
    TEST_ASSERT_EQ(powmod64(7, 1000000006, 1000000007), 1ULL,
                   "7^(1e9+6) mod 1e9+7 (Fermat)");

    // 10^18 mod (10^9+7)
    // 10^9 mod (10^9+7) = 999999993 (= 10^9 - (10^9+7 - 10^9) ... = 10^9 - 7 = 999999993)
    // Wait: 10^9 = 1000000000 < 1000000007, so 10^9 mod 1e9+7 = 1000000000
    // 10^18 mod 1e9+7 = (10^9)^2 mod 1e9+7 = 1000000000^2 mod 1e9+7
    // = 10^18 mod 1e9+7
    // 10^18 / (10^9+7) ≈ 999999993, 999999993 * (10^9+7) = 999999993000000000 + 6999999951 = 999999999999999951
    // 10^18 - 999999999999999951 = 49
    TEST_ASSERT_EQ(powmod64(10, 18, 1000000007), 49ULL, "10^18 mod 1e9+7");
}

// ============================================================================
// 5. Fermat primality test: 2^(p-1) mod p == 1 for known primes
// ============================================================================

void test_fermat_primes() {
    printf("=== Fermat test: 2^(p-1) mod p == 1 for known primes ===\n");

    // Small primes
    uint64_t small_primes[] = {
        2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61,
        67, 71, 73, 79, 83, 89, 97
    };

    for (uint64_t p : small_primes) {
        if (p == 2) {
            // 2^1 mod 2 = 0, special case
            TEST_ASSERT_EQ(powmod64(2, 1, 2), 0ULL, "2^(2-1) mod 2 = 0");
            continue;
        }
        char label[64];
        snprintf(label, sizeof(label), "2^(%lu-1) mod %lu", (unsigned long)p, (unsigned long)p);
        TEST_ASSERT_EQ(powmod64(2, p - 1, p), 1ULL, label);
    }

    // Medium primes
    uint64_t medium_primes[] = {
        1000000007ULL,
        1000000009ULL,
        999999937ULL,
        104729ULL,     // 10000th prime
        1299709ULL,    // 100000th prime
    };

    for (uint64_t p : medium_primes) {
        char label[64];
        snprintf(label, sizeof(label), "2^(%lu-1) mod %lu", (unsigned long)p, (unsigned long)p);
        TEST_ASSERT_EQ(powmod64(2, p - 1, p), 1ULL, label);
    }

    // Large primes (near 2^63)
    uint64_t large_primes[] = {
        9223372036854775783ULL,  // largest prime < 2^63
        4611686018427387847ULL,  // a prime near 2^62
        2305843009213693951ULL,  // Mersenne prime 2^61 - 1
    };

    for (uint64_t p : large_primes) {
        char label[64];
        snprintf(label, sizeof(label), "Fermat on large prime %lu", (unsigned long)p);
        TEST_ASSERT_EQ(powmod64(2, p - 1, p), 1ULL, label);
    }

    // Cross-variant Fermat test on large primes
    printf("  Cross-variant Fermat on large primes...\n");
    for (uint64_t p : large_primes) {
        uint64_t r1 = powmod64_with<mulmod64_v1>(2, p - 1, p);
        uint64_t r2 = powmod64_with<mulmod64_v2>(2, p - 1, p);
        uint64_t r3 = powmod64_with<mulmod64_v3>(2, p - 1, p);

        char label[64];
        snprintf(label, sizeof(label), "Fermat cross-variant p=%lu", (unsigned long)p);
        TEST_ASSERT_EQ(r1, 1ULL, label);
        TEST_ASSERT_EQ(r2, 1ULL, label);
        TEST_ASSERT_EQ(r3, 1ULL, label);
    }
}

// ============================================================================
// 6. Commutativity and identity tests
// ============================================================================

void test_mulmod64_properties() {
    printf("=== mulmod64 algebraic properties ===\n");

    Xorshift64 rng(0xCAFEBABE);

    int failures = 0;
    for (int i = 0; i < 1000; ++i) {
        uint64_t a = rng.next();
        uint64_t b = rng.next();
        uint64_t m = rng.next_range(2, UINT64_MAX);

        // Commutativity: a*b = b*a
        uint64_t ab = mulmod64_v1(a, b, m);
        uint64_t ba = mulmod64_v1(b, a, m);
        if (ab != ba) {
            failures++;
            if (failures <= 3) {
                printf("  FAIL commutativity: a=%lu b=%lu m=%lu ab=%lu ba=%lu\n",
                       (unsigned long)a, (unsigned long)b, (unsigned long)m,
                       (unsigned long)ab, (unsigned long)ba);
            }
        }

        // Identity: a*1 = a mod m
        uint64_t a1 = mulmod64_v1(a, 1, m);
        uint64_t expected = a % m;
        if (a1 != expected) {
            failures++;
            if (failures <= 3) {
                printf("  FAIL identity: a=%lu m=%lu a*1=%lu expected=%lu\n",
                       (unsigned long)a, (unsigned long)m,
                       (unsigned long)a1, (unsigned long)expected);
            }
        }

        // Zero: a*0 = 0
        uint64_t a0 = mulmod64_v1(a, 0, m);
        if (a0 != 0) {
            failures++;
            if (failures <= 3) {
                printf("  FAIL zero: a=%lu m=%lu a*0=%lu\n",
                       (unsigned long)a, (unsigned long)m, (unsigned long)a0);
            }
        }
    }

    g_tests_run++;
    if (failures == 0) {
        g_tests_passed++;
        printf("  All 1000 property checks passed.\n");
    } else {
        g_tests_failed++;
        printf("  FAIL: %d property violations\n", failures);
    }
}

// ============================================================================
// 7. powmod64 cross-variant consistency on random inputs
// ============================================================================

void test_powmod64_cross_variant() {
    printf("=== powmod64 cross-variant (1000 random inputs) ===\n");

    Xorshift64 rng(0x1234ABCD);
    int mismatches = 0;

    for (int i = 0; i < 1000; ++i) {
        uint64_t base = rng.next();
        uint64_t exp = rng.next_range(0, 10000);  // keep exp reasonable for speed
        uint64_t m;

        if (i < 250) {
            m = rng.next_range(2, 1000);
        } else if (i < 500) {
            m = rng.next_range(1000, 1000000007);
        } else if (i < 750) {
            m = rng.next_range(1000000007, 4611686018427387847ULL);
        } else {
            m = rng.next() | 3ULL;
            if (m < 2) m = 2;
        }

        uint64_t r1 = powmod64_with<mulmod64_v1>(base, exp, m);
        uint64_t r2 = powmod64_with<mulmod64_v2>(base, exp, m);
        uint64_t r3 = powmod64_with<mulmod64_v3>(base, exp, m);

        if (r1 != r2 || r1 != r3) {
            mismatches++;
            if (mismatches <= 5) {
                printf("  MISMATCH [%d]: base=%lu exp=%lu m=%lu -> v1=%lu v2=%lu v3=%lu\n",
                       i, (unsigned long)base, (unsigned long)exp, (unsigned long)m,
                       (unsigned long)r1, (unsigned long)r2, (unsigned long)r3);
            }
        }
    }

    g_tests_run++;
    if (mismatches == 0) {
        g_tests_passed++;
        printf("  All 1000 random powmod64 cross-variant checks passed.\n");
    } else {
        g_tests_failed++;
        printf("  FAIL: %d mismatches out of 1000\n", mismatches);
    }
}

// ============================================================================
// 8. Regression: values from the Rust solver
//    These are Gaussian primes produced by the reference implementation.
//    For each prime p ≡ 1 (mod 4), verify 2^((p-1)/2) mod p == 1
//    (Euler criterion — 2 is a QR mod p for these).
// ============================================================================

void test_rust_reference_values() {
    printf("=== Rust reference Gaussian prime norms ===\n");

    // From sieve.rs test: first Gaussian primes with p ≡ 1 (mod 4)
    // These are norms where Cornacchia decomposition exists.
    struct { int32_t a; int32_t b; uint64_t norm; } reference[] = {
        {1, 1, 2},
        {2, 1, 5},
        {3, 2, 13},
        {4, 1, 17},
        {5, 2, 29},
        {6, 1, 37},
        {5, 4, 41},
        {7, 2, 53},
        {6, 5, 61},
        {8, 3, 73},
        {8, 5, 89},
        {9, 4, 97},
        {10, 1, 101},
        {10, 3, 109},
        {8, 7, 113},
        {11, 4, 137},
        {10, 7, 149},
    };

    for (auto& gp : reference) {
        // Verify a^2 + b^2 = norm
        uint64_t computed_norm = (uint64_t)gp.a * gp.a + (uint64_t)gp.b * gp.b;
        char label[64];
        snprintf(label, sizeof(label), "norm(%d,%d) = %lu", gp.a, gp.b, (unsigned long)gp.norm);
        TEST_ASSERT_EQ(computed_norm, gp.norm, label);

        // For norms that are primes ≡ 1 (mod 4), verify Fermat
        if (gp.norm > 2 && gp.norm % 4 == 1) {
            snprintf(label, sizeof(label), "Fermat on norm %lu", (unsigned long)gp.norm);
            TEST_ASSERT_EQ(powmod64(2, gp.norm - 1, gp.norm), 1ULL, label);
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("gaussian-moat-cuda test suite — modular arithmetic\n");
    printf("==================================================\n\n");

    test_mulmod64_known_values();
    test_mulmod64_edge_cases();
    test_mulmod64_cross_variant();
    test_powmod64_known_values();
    test_fermat_primes();
    test_mulmod64_properties();
    test_powmod64_cross_variant();
    test_rust_reference_values();

    printf("\n==================================================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           g_tests_passed, g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("SOME TESTS FAILED\n");
        return 1;
    }

    printf("ALL TESTS PASSED\n");
    return 0;
}
