// test_cornacchia.cu — Comprehensive tests for Cornacchia's algorithm.
//
// For every prime p ≡ 1 (mod 4) below 100,000:
//   1. Verify cornacchia(p) returns true
//   2. Verify a^2 + b^2 == p
//   3. Verify a > b > 0
//
// Also cross-validates against reference data files from the Rust solver.
//
// Build: part of gm_test_cornacchia target in CMakeLists.txt

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

#include "miller_rabin.cuh"
#include "cornacchia.cuh"

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

// ============================================================================
// Reference sieve for ground truth
// ============================================================================

static std::vector<uint64_t> sieve_primes(uint64_t limit) {
    std::vector<bool> is_composite(limit + 1, false);
    is_composite[0] = is_composite[1] = true;
    for (uint64_t i = 2; i * i <= limit; ++i) {
        if (!is_composite[i]) {
            for (uint64_t j = i * i; j <= limit; j += i) {
                is_composite[j] = true;
            }
        }
    }
    std::vector<uint64_t> primes;
    for (uint64_t i = 2; i <= limit; ++i) {
        if (!is_composite[i]) primes.push_back(i);
    }
    return primes;
}

// ============================================================================
// 1. Cornacchia on all primes p ≡ 1 (mod 4) below 100,000
// ============================================================================

void test_cornacchia_all_split_primes_below_100k() {
    printf("=== Cornacchia: all primes p=1(mod 4) below 100000 ===\n");

    auto primes = sieve_primes(100000);
    int count = 0;
    int decomp_failures = 0;
    int norm_failures = 0;
    int order_failures = 0;

    for (uint64_t p : primes) {
        if (p % 4 != 1) continue;
        count++;

        int32_t a, b;
        bool ok = cornacchia(p, &a, &b);

        if (!ok) {
            decomp_failures++;
            if (decomp_failures <= 5) {
                printf("  DECOMP FAIL: cornacchia(%lu) returned false\n",
                       (unsigned long)p);
            }
            continue;
        }

        // Verify a^2 + b^2 == p
        uint64_t norm = (uint64_t)a * a + (uint64_t)b * b;
        if (norm != p) {
            norm_failures++;
            if (norm_failures <= 5) {
                printf("  NORM FAIL: cornacchia(%lu) = (%d, %d), a^2+b^2 = %lu\n",
                       (unsigned long)p, a, b, (unsigned long)norm);
            }
        }

        // Verify a > b > 0
        if (!(a > b && b > 0)) {
            order_failures++;
            if (order_failures <= 5) {
                printf("  ORDER FAIL: cornacchia(%lu) = (%d, %d), expected a > b > 0\n",
                       (unsigned long)p, a, b);
            }
        }
    }

    g_tests_run++;
    int total_failures = decomp_failures + norm_failures + order_failures;
    if (total_failures == 0) {
        g_tests_passed++;
        printf("  All %d split primes decomposed correctly. PASS\n", count);
    } else {
        g_tests_failed++;
        printf("  FAIL: %d decomp, %d norm, %d order failures out of %d primes\n",
               decomp_failures, norm_failures, order_failures, count);
    }
}

// ============================================================================
// 2. Specific known decompositions from the Rust reference
// ============================================================================

void test_cornacchia_known_values() {
    printf("=== Cornacchia: known decompositions ===\n");

    struct { int32_t a; int32_t b; uint64_t p; } known[] = {
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

    for (auto& k : known) {
        int32_t a, b;
        bool ok = cornacchia(k.p, &a, &b);

        char label[128];
        snprintf(label, sizeof(label), "cornacchia(%lu) = (%d,%d), expected (%d,%d)",
                 (unsigned long)k.p, a, b, k.a, k.b);

        g_tests_run++;
        if (ok && a == k.a && b == k.b) {
            g_tests_passed++;
        } else {
            g_tests_failed++;
            printf("  FAIL %s (ok=%d)\n", label, ok);
        }
    }
}

// ============================================================================
// 3. Tonelli-Shanks basic tests
// ============================================================================

void test_tonelli_shanks_basic() {
    printf("=== Tonelli-Shanks: basic tests ===\n");

    // sqrt(4) mod 7 = 2 (since 2^2 = 4 ≡ 4 mod 7)
    {
        uint64_t r = tonelli_shanks(4, 7);
        // r should satisfy r^2 ≡ 4 (mod 7)
        g_tests_run++;
        if (r != UINT64_MAX && mulmod64(r, r, 7) == 4) {
            g_tests_passed++;
        } else {
            g_tests_failed++;
            printf("  FAIL sqrt(4) mod 7: got %lu\n", (unsigned long)r);
        }
    }

    // sqrt(-1) mod 5 = 2 or 3 (since 2^2 = 4 ≡ -1 mod 5)
    {
        uint64_t r = tonelli_shanks(4, 5);  // 4 ≡ -1 mod 5
        g_tests_run++;
        if (r != UINT64_MAX && mulmod64(r, r, 5) == 4) {
            g_tests_passed++;
        } else {
            g_tests_failed++;
            printf("  FAIL sqrt(4) mod 5: got %lu\n", (unsigned long)r);
        }
    }

    // sqrt(-1) mod 13: sqrt(12) mod 13
    {
        uint64_t r = tonelli_shanks(12, 13);
        g_tests_run++;
        if (r != UINT64_MAX && mulmod64(r, r, 13) == 12) {
            g_tests_passed++;
        } else {
            g_tests_failed++;
            printf("  FAIL sqrt(12) mod 13: got %lu\n", (unsigned long)r);
        }
    }

    // Non-residue test: 3 is not a QR mod 7
    {
        uint64_t r = tonelli_shanks(3, 7);
        g_tests_run++;
        if (r == UINT64_MAX) {
            g_tests_passed++;
        } else {
            g_tests_failed++;
            printf("  FAIL: 3 is not QR mod 7 but got %lu\n", (unsigned long)r);
        }
    }
}

// ============================================================================
// 4. fast_sqrt_neg1 for all primes p ≡ 1 (mod 4) below 10,000
// ============================================================================

void test_fast_sqrt_neg1() {
    printf("=== fast_sqrt_neg1: all primes p=1(mod 4) below 10000 ===\n");

    auto primes = sieve_primes(10000);
    int count = 0;
    int failures = 0;

    for (uint64_t p : primes) {
        if (p % 4 != 1) continue;
        count++;

        uint64_t r = fast_sqrt_neg1(p);
        if (r == UINT64_MAX) {
            failures++;
            if (failures <= 5)
                printf("  FAIL: fast_sqrt_neg1(%lu) returned UINT64_MAX\n",
                       (unsigned long)p);
            continue;
        }

        // Verify r^2 ≡ -1 (mod p)
        uint64_t r2 = mulmod64(r, r, p);
        if (r2 != p - 1) {
            failures++;
            if (failures <= 5)
                printf("  FAIL: fast_sqrt_neg1(%lu) = %lu, r^2 = %lu, expected %lu\n",
                       (unsigned long)p, (unsigned long)r, (unsigned long)r2,
                       (unsigned long)(p - 1));
        }
    }

    g_tests_run++;
    if (failures == 0) {
        g_tests_passed++;
        printf("  All %d primes verified. PASS\n", count);
    } else {
        g_tests_failed++;
        printf("  FAIL: %d failures out of %d primes\n", failures, count);
    }
}

// ============================================================================
// 5. Large primes p ≡ 1 (mod 4) — stress test Cornacchia near 32-bit boundary
// ============================================================================

void test_cornacchia_large() {
    printf("=== Cornacchia: large primes near 2^31 ===\n");

    // Primes ≡ 1 (mod 4) near INT32_MAX (2147483647)
    // We need primes where a^2 + b^2 = p fits in i32 outputs.
    // 2147483629 is prime and ≡ 1 (mod 4)
    uint64_t large_primes[] = {
        2147483629ULL,  // largest prime ≡ 1 (mod 4) < 2^31
        1000000021ULL,  // prime ≡ 1 (mod 4)
        999999937ULL,   // prime ... check mod 4: 999999937 % 4 = 1
        104729ULL,      // 10000th prime, 104729 % 4 = 1
    };

    for (uint64_t p : large_primes) {
        if (p % 4 != 1) {
            printf("  SKIP: %lu is not 1 mod 4\n", (unsigned long)p);
            continue;
        }
        if (!is_prime(p)) {
            printf("  SKIP: %lu is not prime\n", (unsigned long)p);
            continue;
        }

        int32_t a, b;
        bool ok = cornacchia(p, &a, &b);

        char label[128];
        snprintf(label, sizeof(label), "cornacchia(%lu)", (unsigned long)p);

        g_tests_run++;
        if (!ok) {
            g_tests_failed++;
            printf("  FAIL %s returned false\n", label);
            continue;
        }

        uint64_t norm = (uint64_t)a * a + (uint64_t)b * b;
        if (norm == p && a > b && b > 0) {
            g_tests_passed++;
        } else {
            g_tests_failed++;
            printf("  FAIL %s: (%d,%d), norm=%lu, expected a>b>0\n",
                   label, a, b, (unsigned long)norm);
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("gaussian-moat-cuda test suite — Cornacchia's algorithm\n");
    printf("======================================================\n\n");

    test_tonelli_shanks_basic();
    test_fast_sqrt_neg1();
    test_cornacchia_known_values();
    test_cornacchia_all_split_primes_below_100k();
    test_cornacchia_large();

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
