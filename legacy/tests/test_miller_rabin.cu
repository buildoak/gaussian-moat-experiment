// test_miller_rabin.cu — Comprehensive tests for Miller-Rabin primality test.
//
// Tests run on HOST (no GPU required for correctness validation).
// Validates against a reference sieve for all values below 10,000.
//
// Build: part of gm_cuda_tests target in CMakeLists.txt

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "miller_rabin.cuh"

// ============================================================================
// Test infrastructure
// ============================================================================

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT_TRUE(expr, label) do {                                   \
    g_tests_run++;                                                           \
    if (expr) {                                                              \
        g_tests_passed++;                                                    \
    } else {                                                                 \
        g_tests_failed++;                                                    \
        printf("  FAIL %s\n", label);                                        \
    }                                                                        \
} while(0)

#define TEST_ASSERT_FALSE(expr, label) do {                                  \
    g_tests_run++;                                                           \
    if (!(expr)) {                                                           \
        g_tests_passed++;                                                    \
    } else {                                                                 \
        g_tests_failed++;                                                    \
        printf("  FAIL %s (expected false, got true)\n", label);             \
    }                                                                        \
} while(0)

// ============================================================================
// Reference sieve for ground truth below 10,000
// ============================================================================

static bool* build_sieve(int limit) {
    bool* is_composite = (bool*)calloc(limit + 1, sizeof(bool));
    is_composite[0] = true;
    is_composite[1] = true;
    for (int i = 2; (long long)i * i <= limit; ++i) {
        if (!is_composite[i]) {
            for (int j = i * i; j <= limit; j += i) {
                is_composite[j] = true;
            }
        }
    }
    return is_composite;
}

// ============================================================================
// 1. All primes below 10,000 must return true
// ============================================================================

void test_all_primes_below_10000() {
    printf("=== Miller-Rabin: all primes below 10000 ===\n");

    bool* is_composite = build_sieve(10000);
    int false_negatives = 0;
    int prime_count = 0;

    for (uint64_t n = 2; n <= 10000; ++n) {
        if (!is_composite[n]) {
            prime_count++;
            if (!is_prime(n)) {
                false_negatives++;
                if (false_negatives <= 10) {
                    printf("  FALSE NEGATIVE: is_prime(%lu) returned false for actual prime\n",
                           (unsigned long)n);
                }
            }
        }
    }

    g_tests_run++;
    if (false_negatives == 0) {
        g_tests_passed++;
        printf("  All %d primes below 10000 correctly identified. PASS\n", prime_count);
    } else {
        g_tests_failed++;
        printf("  FAIL: %d false negatives out of %d primes\n", false_negatives, prime_count);
    }

    free(is_composite);
}

// ============================================================================
// 2. All composites below 10,000 must return false
// ============================================================================

void test_all_composites_below_10000() {
    printf("=== Miller-Rabin: all composites below 10000 ===\n");

    bool* is_composite = build_sieve(10000);
    int false_positives = 0;
    int composite_count = 0;

    for (uint64_t n = 2; n <= 10000; ++n) {
        if (is_composite[n]) {
            composite_count++;
            if (is_prime(n)) {
                false_positives++;
                if (false_positives <= 10) {
                    printf("  FALSE POSITIVE: is_prime(%lu) returned true for composite\n",
                           (unsigned long)n);
                }
            }
        }
    }

    g_tests_run++;
    if (false_positives == 0) {
        g_tests_passed++;
        printf("  All %d composites below 10000 correctly rejected. PASS\n", composite_count);
    } else {
        g_tests_failed++;
        printf("  FAIL: %d false positives out of %d composites\n", false_positives, composite_count);
    }

    free(is_composite);
}

// ============================================================================
// 3. Edge cases: n < 2
// ============================================================================

void test_edge_cases() {
    printf("=== Miller-Rabin: edge cases ===\n");

    TEST_ASSERT_FALSE(is_prime(0), "is_prime(0)");
    TEST_ASSERT_FALSE(is_prime(1), "is_prime(1)");
    TEST_ASSERT_TRUE(is_prime(2), "is_prime(2)");
    TEST_ASSERT_TRUE(is_prime(3), "is_prime(3)");
    TEST_ASSERT_FALSE(is_prime(4), "is_prime(4)");
    TEST_ASSERT_TRUE(is_prime(5), "is_prime(5)");
    TEST_ASSERT_FALSE(is_prime(6), "is_prime(6)");
    TEST_ASSERT_TRUE(is_prime(7), "is_prime(7)");
    TEST_ASSERT_FALSE(is_prime(8), "is_prime(8)");
    TEST_ASSERT_FALSE(is_prime(9), "is_prime(9)");
    TEST_ASSERT_FALSE(is_prime(10), "is_prime(10)");
    TEST_ASSERT_TRUE(is_prime(11), "is_prime(11)");
}

// ============================================================================
// 4. Carmichael numbers — must all return false
// ============================================================================

void test_carmichael_numbers() {
    printf("=== Miller-Rabin: Carmichael numbers ===\n");

    // Carmichael numbers fool the Fermat test but not Miller-Rabin with
    // proper witnesses.
    uint64_t carmichaels[] = {
        561, 1105, 1729, 2465, 2821, 6601, 8911,
        // More Carmichael numbers
        10585, 15841, 29341, 41041, 46657, 52633, 62745,
        63973, 75361, 101101, 115921, 126217, 162401,
        172081, 188461, 252601, 278545, 294409, 314821,
        334153, 340561, 399001, 410041, 449065, 488881,
        512461
    };

    int failures = 0;
    for (uint64_t c : carmichaels) {
        if (is_prime(c)) {
            failures++;
            printf("  FAIL: is_prime(%lu) returned true for Carmichael number\n",
                   (unsigned long)c);
        }
    }

    g_tests_run++;
    if (failures == 0) {
        g_tests_passed++;
        printf("  All %lu Carmichael numbers correctly rejected. PASS\n",
               (unsigned long)(sizeof(carmichaels) / sizeof(carmichaels[0])));
    } else {
        g_tests_failed++;
        printf("  FAIL: %d Carmichael numbers misidentified\n", failures);
    }
}

// ============================================================================
// 5. Large known primes near 2^32 and 2^63 boundaries
// ============================================================================

void test_large_known_primes() {
    printf("=== Miller-Rabin: large known primes ===\n");

    // Primes near 2^32
    TEST_ASSERT_TRUE(is_prime(4294967291ULL), "is_prime(2^32 - 5)");       // largest prime < 2^32
    TEST_ASSERT_TRUE(is_prime(4294967279ULL), "is_prime(2^32 - 17)");
    TEST_ASSERT_TRUE(is_prime(4294967231ULL), "is_prime(2^32 - 65)");

    // Composites near 2^32
    TEST_ASSERT_FALSE(is_prime(4294967296ULL), "is_prime(2^32)");           // 2^32 = even
    TEST_ASSERT_FALSE(is_prime(4294967295ULL), "is_prime(2^32 - 1)");      // 3 * 5 * 17 * 257 * 65537
    TEST_ASSERT_FALSE(is_prime(4294967293ULL), "is_prime(2^32 - 3)");      // composite

    // Primes near 2^63
    TEST_ASSERT_TRUE(is_prime(9223372036854775783ULL), "is_prime(largest prime < 2^63)");
    TEST_ASSERT_TRUE(is_prime(4611686018427387847ULL), "is_prime(prime near 2^62)");

    // Mersenne prime 2^61 - 1
    TEST_ASSERT_TRUE(is_prime(2305843009213693951ULL), "is_prime(2^61 - 1, Mersenne)");

    // Composites near 2^63
    TEST_ASSERT_FALSE(is_prime(9223372036854775808ULL), "is_prime(2^63)");  // even
    TEST_ASSERT_FALSE(is_prime(9223372036854775807ULL), "is_prime(2^63 - 1)"); // 7^2 * 73 * ...

    // Large known composites
    TEST_ASSERT_FALSE(is_prime(9223372036854775806ULL), "is_prime(2^63 - 2)"); // even
}

// ============================================================================
// 6. Primes congruent to 1 mod 4 (Gaussian split primes — our kernel's focus)
// ============================================================================

void test_primes_1_mod_4() {
    printf("=== Miller-Rabin: primes p = 1 (mod 4) below 1000 ===\n");

    bool* is_composite = build_sieve(1000);
    int count = 0;
    int failures = 0;

    for (uint64_t n = 5; n <= 1000; n += 4) {
        if (!is_composite[n]) {
            count++;
            if (!is_prime(n)) {
                failures++;
                printf("  FAIL: is_prime(%lu) false for prime p=1(mod 4)\n",
                       (unsigned long)n);
            }
        }
    }

    g_tests_run++;
    if (failures == 0) {
        g_tests_passed++;
        printf("  All %d primes p=1(mod 4) below 1000 correctly identified. PASS\n", count);
    } else {
        g_tests_failed++;
        printf("  FAIL: %d misidentified out of %d\n", failures, count);
    }

    free(is_composite);
}

// ============================================================================
// 7. Strong pseudoprimes to base 2 — these fool base-2 MR but not 7-witness
// ============================================================================

void test_strong_pseudoprimes_base2() {
    printf("=== Miller-Rabin: strong pseudoprimes to base 2 ===\n");

    // Strong pseudoprimes to base 2 (OEIS A001262)
    // These pass the single-witness test for a=2 but are composite.
    // Our 7-witness test must catch them all.
    uint64_t spsp2[] = {
        2047, 3277, 4033, 4681, 8321, 15841, 29341, 42799, 49141,
        52633, 65281, 74665, 80581, 85489, 88357, 90751
    };

    int failures = 0;
    for (uint64_t n : spsp2) {
        if (is_prime(n)) {
            failures++;
            printf("  FAIL: is_prime(%lu) returned true for spsp-2\n",
                   (unsigned long)n);
        }
    }

    g_tests_run++;
    if (failures == 0) {
        g_tests_passed++;
        printf("  All %lu strong pseudoprimes to base 2 correctly rejected. PASS\n",
               (unsigned long)(sizeof(spsp2) / sizeof(spsp2[0])));
    } else {
        g_tests_failed++;
        printf("  FAIL: %d spsp-2 numbers misidentified\n", failures);
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("gaussian-moat-cuda test suite — Miller-Rabin primality\n");
    printf("======================================================\n\n");

    test_edge_cases();
    test_all_primes_below_10000();
    test_all_composites_below_10000();
    test_carmichael_numbers();
    test_large_known_primes();
    test_primes_1_mod_4();
    test_strong_pseudoprimes_base2();

    printf("\n======================================================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           g_tests_passed, g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("SOME TESTS FAILED\n");
        return 1;
    }

    printf("ALL TESTS PASSED\n");
    return 0;
}
