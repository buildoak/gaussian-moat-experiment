// test_sieve.cu — Tests for the segmented sieve kernel.
//
// Validates:
//   1. CPU simple_sieve_cpu correctness (known prime counts)
//   2. GPU sieve kernel output against CPU sieve (small ranges)
//   3. Full sieve + Cornacchia pipeline against reference files (T0-T3)
//   4. Edge cases: first segment (seg_lo=0), segment boundaries

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

#include <cuda_runtime.h>

#include "types.h"
#include "sieve_base.cuh"
#include "sieve_kernel.cuh"
#include "cornacchia.cuh"

// ============================================================================
// Test infrastructure
// ============================================================================

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define CHECK(cond, ...) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        printf("  FAIL: "); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

// ============================================================================
// Test: CPU simple_sieve_cpu
// ============================================================================

void test_cpu_sieve() {
    printf("=== CPU sieve correctness ===\n");

    auto p100 = simple_sieve_cpu(100);
    CHECK(p100.size() == 25, "pi(100) = %zu, expected 25", p100.size());

    auto p1000 = simple_sieve_cpu(1000);
    CHECK(p1000.size() == 168, "pi(1000) = %zu, expected 168", p1000.size());

    auto p10000 = simple_sieve_cpu(10000);
    CHECK(p10000.size() == 1229, "pi(10000) = %zu, expected 1229", p10000.size());

    auto p100000 = simple_sieve_cpu(100000);
    CHECK(p100000.size() == 9592, "pi(100000) = %zu, expected 9592", p100000.size());

    // Check first few primes
    CHECK(p100[0] == 2, "first prime = %u, expected 2", p100[0]);
    CHECK(p100[1] == 3, "second prime = %u, expected 3", p100[1]);
    CHECK(p100[24] == 97, "25th prime = %u, expected 97", p100[24]);

    // Check partition_primes
    uint32_t tiny_count = partition_primes(p10000, TINY_THRESHOLD);
    CHECK(tiny_count == 54, "tiny primes (p <= 256) = %u, expected 54", tiny_count);

    printf("  CPU sieve: %d/%d passed\n", g_tests_passed, g_tests_run);
}

// ============================================================================
// Helper: run sieve kernel on GPU and return primes
// ============================================================================

static std::vector<uint64_t> run_sieve_gpu(uint64_t norm_lo, uint64_t norm_hi) {
    // Generate base primes
    uint64_t sqrt_hi = (uint64_t)sqrt((double)norm_hi) + 2;
    auto base_primes = simple_sieve_cpu(sqrt_hi);

    uint32_t tiny_count = partition_primes(base_primes, TINY_THRESHOLD);
    uint32_t small_count = partition_primes(base_primes, SEGMENT_SPAN);

    // Upload base primes
    uint32_t* d_base_primes = nullptr;
    cudaMalloc(&d_base_primes, base_primes.size() * sizeof(uint32_t));
    cudaMemcpy(d_base_primes, base_primes.data(),
               base_primes.size() * sizeof(uint32_t), cudaMemcpyHostToDevice);

    // Allocate output
    uint32_t max_output = (uint32_t)(norm_hi / 2 + 4096);
    if (max_output > 20000000u) max_output = 20000000u;
    if (max_output < 4096u) max_output = 4096u;

    uint64_t* d_output = nullptr;
    uint32_t* d_count = nullptr;
    cudaMalloc(&d_output, (size_t)max_output * sizeof(uint64_t));
    cudaMalloc(&d_count, sizeof(uint32_t));
    cudaMemset(d_count, 0, sizeof(uint32_t));

    // Calculate segments and grid
    uint64_t aligned_lo = norm_lo & ~1ULL;
    uint64_t total_span = norm_hi - aligned_lo;
    uint32_t num_segments = (uint32_t)((total_span + SEGMENT_SPAN - 1) / SEGMENT_SPAN);
    int grid = (num_segments < 1024) ? num_segments : 1024;
    if (grid < 1) grid = 1;

    segmented_sieve_kernel<<<grid, THREADS_PER_BLK>>>(
        norm_lo, norm_hi,
        d_base_primes, (uint32_t)base_primes.size(),
        tiny_count, small_count,
        nullptr, nullptr,  // No bucket hits
        d_output, max_output, d_count
    );
    cudaDeviceSynchronize();

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        fprintf(stderr, "  CUDA error: %s\n", cudaGetErrorString(err));
        cudaFree(d_base_primes);
        cudaFree(d_output);
        cudaFree(d_count);
        return {};
    }

    uint32_t count = 0;
    cudaMemcpy(&count, d_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);

    std::vector<uint64_t> result(count);
    if (count > 0) {
        cudaMemcpy(result.data(), d_output, count * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    }

    cudaFree(d_base_primes);
    cudaFree(d_output);
    cudaFree(d_count);

    std::sort(result.begin(), result.end());
    return result;
}

// ============================================================================
// Helper: run full pipeline (sieve + Cornacchia) and return GaussianPrimes
// ============================================================================

struct GPrime {
    int32_t a, b;
    uint64_t norm;

    bool operator<(const GPrime& o) const {
        if (norm != o.norm) return norm < o.norm;
        if (a != o.a) return a < o.a;
        return b < o.b;
    }
    bool operator==(const GPrime& o) const {
        return a == o.a && b == o.b && norm == o.norm;
    }
};

// Helper: test whether a number is prime (trial division for small values)
static bool test_is_prime(uint64_t n) {
    if (n < 2) return false;
    if (n < 4) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (uint64_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return false;
    }
    return true;
}

// Helper: generate inert primes (p == 3 mod 4, norm = p^2) on CPU
static void test_generate_inert_primes(uint64_t norm_lo, uint64_t norm_hi,
                                       std::vector<GPrime>& out) {
    uint64_t p_lo = (norm_lo <= 1) ? 3 : (uint64_t)sqrt((double)norm_lo);
    while (p_lo > 0 && p_lo * p_lo < norm_lo) p_lo++;
    if (p_lo < 3) p_lo = 3;
    if (p_lo % 2 == 0) p_lo++;
    if (p_lo % 4 == 1) p_lo += 2;

    uint64_t p_hi = (uint64_t)sqrt((double)(norm_hi - 1));

    for (uint64_t p = p_lo; p <= p_hi; p += 4) {
        if (p * p >= norm_hi) break;
        if (p * p < norm_lo) continue;
        if (test_is_prime(p)) {
            GPrime gp;
            gp.a = (int32_t)p;
            gp.b = 0;
            gp.norm = p * p;
            out.push_back(gp);
        }
    }
}

static std::vector<GPrime> run_full_pipeline(uint64_t norm_lo, uint64_t norm_hi) {
    std::vector<GPrime> result;

    // Ramified prime
    if (norm_lo <= 2 && norm_hi > 2) {
        result.push_back({1, 1, 2});
    }

    // Generate base primes
    uint64_t sqrt_hi = (uint64_t)sqrt((double)norm_hi) + 2;
    auto base_primes = simple_sieve_cpu(sqrt_hi);

    uint32_t tiny_count = partition_primes(base_primes, TINY_THRESHOLD);
    uint32_t small_count = partition_primes(base_primes, SEGMENT_SPAN);

    // Upload base primes
    uint32_t* d_base_primes = nullptr;
    cudaMalloc(&d_base_primes, base_primes.size() * sizeof(uint32_t));
    cudaMemcpy(d_base_primes, base_primes.data(),
               base_primes.size() * sizeof(uint32_t), cudaMemcpyHostToDevice);

    // Sieve output
    uint32_t max_sieve = (uint32_t)(norm_hi / 2 + 4096);
    if (max_sieve > 20000000u) max_sieve = 20000000u;
    if (max_sieve < 4096u) max_sieve = 4096u;

    uint64_t* d_sieve_out = nullptr;
    uint32_t* d_sieve_count = nullptr;
    cudaMalloc(&d_sieve_out, (size_t)max_sieve * sizeof(uint64_t));
    cudaMalloc(&d_sieve_count, sizeof(uint32_t));
    cudaMemset(d_sieve_count, 0, sizeof(uint32_t));

    // Launch sieve
    uint64_t aligned_lo = norm_lo & ~1ULL;
    uint64_t total_span = norm_hi - aligned_lo;
    uint32_t num_segments = (uint32_t)((total_span + SEGMENT_SPAN - 1) / SEGMENT_SPAN);
    int grid = (num_segments < 1024) ? num_segments : 1024;
    if (grid < 1) grid = 1;

    segmented_sieve_kernel<<<grid, THREADS_PER_BLK>>>(
        norm_lo, norm_hi,
        d_base_primes, (uint32_t)base_primes.size(),
        tiny_count, small_count,
        nullptr, nullptr,
        d_sieve_out, max_sieve, d_sieve_count
    );
    cudaDeviceSynchronize();

    uint32_t sieve_count = 0;
    cudaMemcpy(&sieve_count, d_sieve_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);

    // Cornacchia dispatch
    GaussianPrime* d_gp_out = nullptr;
    uint32_t* d_gp_count = nullptr;
    cudaMalloc(&d_gp_out, (size_t)max_sieve * sizeof(GaussianPrime));
    cudaMalloc(&d_gp_count, sizeof(uint32_t));
    cudaMemset(d_gp_count, 0, sizeof(uint32_t));

    if (sieve_count > 0) {
        int corn_grid = (sieve_count + 255) / 256;
        cornacchia_dispatch_kernel<<<corn_grid, 256>>>(
            d_sieve_out, sieve_count,
            d_gp_out, max_sieve, d_gp_count,
            norm_lo, norm_hi
        );
        cudaDeviceSynchronize();
    }

    uint32_t gp_count = 0;
    cudaMemcpy(&gp_count, d_gp_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);

    if (gp_count > 0) {
        std::vector<GaussianPrime> gpu_primes(gp_count);
        cudaMemcpy(gpu_primes.data(), d_gp_out, gp_count * sizeof(GaussianPrime),
                   cudaMemcpyDeviceToHost);
        for (const auto& gp : gpu_primes) {
            result.push_back({gp.a, gp.b, gp.norm});
        }
    }

    cudaFree(d_base_primes);
    cudaFree(d_sieve_out);
    cudaFree(d_sieve_count);
    cudaFree(d_gp_out);
    cudaFree(d_gp_count);

    // Add inert primes (p == 3 mod 4, norm = p^2) from CPU
    test_generate_inert_primes(norm_lo, norm_hi, result);

    std::sort(result.begin(), result.end());
    return result;
}

// ============================================================================
// Test: GPU sieve correctness against CPU sieve
// ============================================================================

void test_gpu_sieve_vs_cpu() {
    printf("=== GPU sieve vs CPU sieve ===\n");

    struct TestCase {
        uint64_t lo, hi;
        const char* label;
    };

    TestCase cases[] = {
        {0, 50, "T0 (0-50)"},
        {0, 200, "T1 (0-200)"},
        {0, 1000, "0-1000"},
        {0, 10000, "T2 (0-10000)"},
        {0, 100000, "0-100000"},
        {100, 10000, "100-10000"},
        {50000, 100000, "50000-100000"},
    };

    for (const auto& tc : cases) {
        auto gpu_primes = run_sieve_gpu(tc.lo, tc.hi);

        // CPU reference: primes p == 1 mod 4 in [lo, hi)
        // (Sieve now only outputs primes == 1 mod 4; inert primes handled on CPU)
        auto cpu_all = simple_sieve_cpu(tc.hi);
        std::vector<uint64_t> cpu_primes;
        for (uint64_t p : cpu_all) {
            if (p >= tc.lo && p < tc.hi) {
                if (p > 2 && (p & 3u) == 1u) cpu_primes.push_back(p);
            }
        }
        std::sort(cpu_primes.begin(), cpu_primes.end());

        // GPU output already filtered to mod-1 primes
        std::vector<uint64_t> gpu_filtered;
        for (uint64_t p : gpu_primes) {
            if (p > 2) gpu_filtered.push_back(p);
        }

        bool match = (gpu_filtered.size() == cpu_primes.size());
        if (match) {
            for (size_t i = 0; i < gpu_filtered.size(); i++) {
                if (gpu_filtered[i] != cpu_primes[i]) {
                    match = false;
                    printf("  First mismatch at [%zu]: GPU=%lu, CPU=%lu\n",
                           i, (unsigned long)gpu_filtered[i], (unsigned long)cpu_primes[i]);
                    break;
                }
            }
        }

        CHECK(match, "%s: GPU=%zu primes, CPU=%zu primes",
              tc.label, gpu_filtered.size(), cpu_primes.size());

        if (match) {
            printf("  %s: %zu primes — exact match. PASS\n", tc.label, gpu_filtered.size());
        } else {
            printf("  %s: GPU=%zu vs CPU=%zu primes\n", tc.label, gpu_filtered.size(), cpu_primes.size());
            // Show first few diffs
            size_t gi = 0, ci = 0;
            int diffs = 0;
            while (gi < gpu_filtered.size() && ci < cpu_primes.size() && diffs < 5) {
                if (gpu_filtered[gi] == cpu_primes[ci]) { gi++; ci++; }
                else if (gpu_filtered[gi] < cpu_primes[ci]) {
                    printf("    EXTRA in GPU: %lu\n", (unsigned long)gpu_filtered[gi]);
                    gi++; diffs++;
                } else {
                    printf("    MISSING from GPU: %lu\n", (unsigned long)cpu_primes[ci]);
                    ci++; diffs++;
                }
            }
        }
    }
}

// ============================================================================
// Parse reference file: "a b norm\n" per line
// ============================================================================

static std::vector<GPrime> parse_reference_file(const char* path) {
    std::vector<GPrime> result;
    FILE* f = fopen(path, "r");
    if (!f) return result;

    int32_t a, b;
    uint64_t norm;
    while (fscanf(f, "%d %d %lu", &a, &b, &norm) == 3) {
        result.push_back({a, b, norm});
    }
    fclose(f);
    return result;
}

// ============================================================================
// Test: full pipeline against reference files
// ============================================================================

void test_full_pipeline_reference() {
    printf("=== Full pipeline vs reference files ===\n");

    struct RefTest {
        const char* path;
        uint64_t norm_bound;
        const char* tier;
    };

    RefTest refs[] = {
        {"../reference/primes_T0_50.txt", 50, "T0"},
        {"../reference/primes_T1_200.txt", 200, "T1"},
        {"../reference/primes_T2_10000.txt", 10000, "T2"},
        {"../reference/primes_T3_1000000.txt", 1000000, "T3"},
    };

    for (const auto& rt : refs) {
        printf("  --- %s (norm <= %lu) ---\n", rt.tier, (unsigned long)rt.norm_bound);

        auto reference = parse_reference_file(rt.path);
        if (reference.empty()) {
            g_tests_run++;
            g_tests_failed++;
            printf("  SKIP: cannot load %s\n", rt.path);
            continue;
        }

        auto pipeline = run_full_pipeline(0, rt.norm_bound + 1);

        printf("  Pipeline: %zu primes, Reference: %zu primes\n",
               pipeline.size(), reference.size());

        // Compare
        int diffs = 0;
        size_t gi = 0, ri = 0;
        while (gi < pipeline.size() && ri < reference.size()) {
            if (pipeline[gi] == reference[ri]) { gi++; ri++; }
            else if (pipeline[gi] < reference[ri]) {
                diffs++;
                if (diffs <= 5)
                    printf("    EXTRA:   (%d, %d, %lu)\n",
                           pipeline[gi].a, pipeline[gi].b, (unsigned long)pipeline[gi].norm);
                gi++;
            } else {
                diffs++;
                if (diffs <= 5)
                    printf("    MISSING: (%d, %d, %lu)\n",
                           reference[ri].a, reference[ri].b, (unsigned long)reference[ri].norm);
                ri++;
            }
        }
        while (gi < pipeline.size()) {
            diffs++;
            if (diffs <= 5)
                printf("    EXTRA:   (%d, %d, %lu)\n",
                       pipeline[gi].a, pipeline[gi].b, (unsigned long)pipeline[gi].norm);
            gi++;
        }
        while (ri < reference.size()) {
            diffs++;
            if (diffs <= 5)
                printf("    MISSING: (%d, %d, %lu)\n",
                       reference[ri].a, reference[ri].b, (unsigned long)reference[ri].norm);
            ri++;
        }

        CHECK(diffs == 0, "%s: %d differences", rt.tier, diffs);
        if (diffs == 0) {
            printf("  %s: ZERO diff — bit-exact match. PASS\n", rt.tier);
        }
    }
}

// ============================================================================
// Test: self-consistency (a^2 + b^2 == norm for all output)
// ============================================================================

void test_self_consistency() {
    printf("=== Self-consistency check ===\n");

    auto primes = run_full_pipeline(0, 10001);
    int failures = 0;

    for (const auto& gp : primes) {
        uint64_t computed = (uint64_t)gp.a * gp.a + (uint64_t)gp.b * gp.b;
        if (computed != gp.norm) {
            failures++;
            if (failures <= 5)
                printf("  (%d,%d) has a^2+b^2=%lu != norm=%lu\n",
                       gp.a, gp.b, (unsigned long)computed, (unsigned long)gp.norm);
        }
        if (!(gp.a >= gp.b && gp.b >= 0)) {
            failures++;
            if (failures <= 5)
                printf("  (%d,%d) violates a >= b >= 0\n", gp.a, gp.b);
        }
    }

    CHECK(failures == 0, "%d consistency failures in %zu primes", failures, primes.size());
    if (failures == 0) {
        printf("  All %zu primes self-consistent. PASS\n", primes.size());
    }
}

// ============================================================================
// Test: sieve handles segment boundaries correctly
// ============================================================================

void test_segment_boundaries() {
    printf("=== Segment boundary test ===\n");

    // Test a range that spans the segment boundary at SEGMENT_SPAN (262144).
    // 262153 is prime and == 1 mod 4 (262153 % 4 == 1).
    auto primes = run_sieve_gpu(262130, 262200);

    bool found_262153 = false;
    for (uint64_t p : primes) {
        if (p == 262153) found_262153 = true;
    }
    CHECK(found_262153, "Prime 262153 (==1 mod 4) found near segment boundary");

    // Test that the range across multiple segments gives same result as CPU
    // (only p == 1 mod 4 primes, matching new sieve behaviour)
    auto gpu_wide = run_sieve_gpu(0, 500000);
    auto cpu_all = simple_sieve_cpu(500000);
    std::vector<uint64_t> cpu_mod1;
    for (uint64_t p : cpu_all) {
        if (p > 2 && (p & 3u) == 1u) cpu_mod1.push_back(p);
    }

    CHECK(gpu_wide.size() == cpu_mod1.size(),
          "Multi-segment: GPU=%zu vs CPU=%zu (range 0-500000)",
          gpu_wide.size(), cpu_mod1.size());

    if (gpu_wide.size() == cpu_mod1.size()) {
        bool all_match = true;
        for (size_t i = 0; i < gpu_wide.size(); i++) {
            if (gpu_wide[i] != cpu_mod1[i]) {
                all_match = false;
                printf("  First mismatch at [%zu]: GPU=%lu, CPU=%lu\n",
                       i, (unsigned long)gpu_wide[i], (unsigned long)cpu_mod1[i]);
                break;
            }
        }
        CHECK(all_match, "Multi-segment: all primes match");
        if (all_match) printf("  Multi-segment (0-500000): %zu primes exact match. PASS\n", gpu_wide.size());
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("gaussian-moat-cuda test suite — Segmented Sieve\n");
    printf("================================================\n\n");

    // Check CUDA device
    int device_count = 0;
    cudaGetDeviceCount(&device_count);
    if (device_count == 0) {
        printf("FATAL: No CUDA devices found\n");
        return 1;
    }
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("Device: %s (SM %d.%d)\n\n", prop.name, prop.major, prop.minor);

    test_cpu_sieve();
    printf("\n");

    test_gpu_sieve_vs_cpu();
    printf("\n");

    test_segment_boundaries();
    printf("\n");

    test_self_consistency();
    printf("\n");

    test_full_pipeline_reference();
    printf("\n");

    printf("================================================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           g_tests_passed, g_tests_failed, g_tests_run);

    if (g_tests_failed > 0) {
        printf("SOME TESTS FAILED\n");
        return 1;
    }

    printf("ALL TESTS PASSED\n");
    return 0;
}
