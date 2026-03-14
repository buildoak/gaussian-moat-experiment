// main.cu — Production Gaussian prime generator
//
// Enumerates all Gaussian primes with norms in [norm_lo, norm_hi).
// Three categories:
//   1. Ramified prime: (1,1) with norm 2  (always if range includes 2)
//   2. Split primes: p ≡ 1 (mod 4), decompose via Cornacchia → (a,b,p)
//   3. Inert primes: p ≡ 3 (mod 4), emit (p,0,p²)
//
// Split primes are processed on GPU in batches (MR + Cornacchia).
// Ramified and inert primes are handled CPU-side.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>
#include <chrono>
#include <fstream>

#include <cuda_runtime.h>

#include "types.h"
#include "modular_arith.cuh"
#include "miller_rabin.cuh"
#include "cornacchia.cuh"

// Declared in kernel.cu
extern __global__ void gaussian_prime_kernel(
    uint64_t batch_start,
    uint64_t batch_end,
    GaussianPrime* output,
    uint32_t max_output,
    uint32_t* output_count
);

// ---------------------------------------------------------------------------
// CLI argument parsing
// ---------------------------------------------------------------------------
struct Config {
    uint64_t norm_lo    = 0;
    uint64_t norm_hi    = 0;
    std::string output  = "";
    uint32_t batch_size = 5u * 1000u * 1000u;   // 5M candidates per batch (tuned: 8M hits 65536-block limit on Orin Nano)
    int block_size      = 128;                   // threads per block (tuned for register pressure)
};

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s --norm-lo N --norm-hi N --output FILE [--batch-size N] [--block-size N]\n"
        "\n"
        "  --norm-lo N      Lower bound of norm range (inclusive)\n"
        "  --norm-hi N      Upper bound of norm range (exclusive)\n"
        "  --output FILE    Output file path (text: a b norm per line)\n"
        "  --batch-size N   Candidates per GPU batch (default 8388608)\n"
        "  --block-size N   Threads per block: 128, 256, or 512 (default 128)\n",
        prog);
}

static Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--norm-lo") == 0 && i + 1 < argc) {
            cfg.norm_lo = strtoull(argv[++i], nullptr, 10);
        } else if (strcmp(argv[i], "--norm-hi") == 0 && i + 1 < argc) {
            cfg.norm_hi = strtoull(argv[++i], nullptr, 10);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            cfg.output = argv[++i];
        } else if (strcmp(argv[i], "--batch-size") == 0 && i + 1 < argc) {
            cfg.batch_size = (uint32_t)strtoul(argv[++i], nullptr, 10);
        } else if (strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) {
            cfg.block_size = (int)strtol(argv[++i], nullptr, 10);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        }
    }
    if (cfg.norm_hi == 0 || cfg.output.empty()) {
        print_usage(argv[0]);
        exit(1);
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Thermal management
// ---------------------------------------------------------------------------
static int read_gpu_temp() {
    // Jetson Orin Nano thermal zones via sysfs
    FILE* f = fopen("/sys/devices/virtual/thermal/thermal_zone1/temp", "r");
    if (!f) {
        f = fopen("/sys/devices/virtual/thermal/thermal_zone0/temp", "r");
    }
    if (!f) return -1;
    int temp = 0;
    if (fscanf(f, "%d", &temp) == 1) {
        fclose(f);
        return temp / 1000;  // millidegrees to degrees
    }
    fclose(f);
    return -1;
}

static void thermal_check() {
    int temp = read_gpu_temp();
    if (temp > 78) {
        fprintf(stderr, "[THERMAL] GPU at %d°C — pausing 30s to cool\n", temp);
        cudaDeviceSynchronize();
        struct timespec ts = {30, 0};
        nanosleep(&ts, nullptr);
    }
}

// ---------------------------------------------------------------------------
// CPU-side: generate inert primes (p ≡ 3 mod 4, norm = p²)
// ---------------------------------------------------------------------------
static void generate_inert_primes(uint64_t norm_lo, uint64_t norm_hi,
                                  std::vector<GaussianPrime>& out) {
    // Inert primes have norm p² where p ≡ 3 (mod 4).
    // We need p² in [norm_lo, norm_hi), so p in [ceil(sqrt(norm_lo)), floor(sqrt(norm_hi-1))].
    uint64_t p_lo = (norm_lo <= 1) ? 3 : isqrt64(norm_lo);
    // Make sure p_lo² >= norm_lo
    while (p_lo > 0 && p_lo * p_lo < norm_lo) p_lo++;
    // Start from at least 3
    if (p_lo < 3) p_lo = 3;
    // Align to odd
    if (p_lo % 2 == 0) p_lo++;
    // Align to ≡ 3 (mod 4)
    if (p_lo % 4 == 1) p_lo += 2;

    uint64_t p_hi_sq = norm_hi - 1;
    uint64_t p_hi = isqrt64(p_hi_sq);
    // p_hi is the largest p where p² < norm_hi

    for (uint64_t p = p_lo; p <= p_hi; p += 4) {
        if (p * p >= norm_hi) break;
        if (p * p < norm_lo) continue;
        // Must actually be prime
        if (is_prime(p)) {
            GaussianPrime gp;
            gp.a = (int32_t)p;
            gp.b = 0;
            gp.norm = p * p;
            out.push_back(gp);
        }
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    fprintf(stderr, "gm_cuda_primes — norm range [%lu, %lu)\n",
            (unsigned long)cfg.norm_lo, (unsigned long)cfg.norm_hi);

    // --- Device setup ---
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count == 0) {
        fprintf(stderr, "FATAL: No CUDA devices found (%s)\n", cudaGetErrorString(err));
        return 1;
    }

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    fprintf(stderr, "Device: %s (SM %d.%d, %d SMs, %.1f GB)\n",
            prop.name, prop.major, prop.minor,
            prop.multiProcessorCount,
            prop.totalGlobalMem / (1024.0 * 1024.0 * 1024.0));

    // --- Collect all results ---
    std::vector<GaussianPrime> all_primes;

    auto t_start = std::chrono::high_resolution_clock::now();

    // --- Category 1: Ramified prime (1+i)(1-i), norm 2 ---
    if (cfg.norm_lo <= 2 && cfg.norm_hi > 2) {
        GaussianPrime gp;
        gp.a = 1;
        gp.b = 1;
        gp.norm = 2;
        all_primes.push_back(gp);
    }

    // --- Category 3: Inert primes (CPU-side) ---
    generate_inert_primes(cfg.norm_lo, cfg.norm_hi, all_primes);

    // --- Category 2: Split primes (GPU-side) ---
    // Enumerate candidates n ≡ 1 (mod 4) in [norm_lo, norm_hi)
    // The kernel handles stride-of-4 enumeration starting from a base ≡ 1 mod 4.

    // Compute first candidate ≡ 1 (mod 4) >= max(5, norm_lo)
    uint64_t gpu_lo = (cfg.norm_lo < 5) ? 5 : cfg.norm_lo;
    // Align to ≡ 1 (mod 4)
    while (gpu_lo % 4 != 1) gpu_lo++;

    uint64_t gpu_hi = cfg.norm_hi;

    if (gpu_lo < gpu_hi) {
        // Estimate output buffer size using PNT: π(n) ≈ n/ln(n)
        // Split primes (p ≡ 1 mod 4) are roughly half of all primes.
        // Use 0.75x PNT as safety margin (actual split fraction ~0.5).
        // This keeps memory footprint manageable on Jetson's unified memory.
        // PNT estimate: split primes ≈ n/(2*ln(n)), add 20% margin
        double ln_hi = (gpu_hi > 10) ? log((double)gpu_hi) : 3.0;
        uint64_t estimated_primes = (uint64_t)((double)gpu_hi / ln_hi * 0.6) + 4096;
        if (estimated_primes > 20000000ULL) estimated_primes = 20000000ULL;
        uint32_t max_output = (uint32_t)estimated_primes;

        fprintf(stderr, "Output buffer: %u entries (%.1f MB)\n",
                max_output, (double)max_output * sizeof(GaussianPrime) / (1024.0 * 1024.0));
        fflush(stderr);

        // Allocate device buffers
        GaussianPrime* d_output = nullptr;
        uint32_t* d_count = nullptr;
        err = cudaMalloc(&d_output, (size_t)max_output * sizeof(GaussianPrime));
        if (err != cudaSuccess) {
            fprintf(stderr, "FATAL: cudaMalloc output failed: %s (requested %.1f MB)\n",
                    cudaGetErrorString(err),
                    (double)max_output * sizeof(GaussianPrime) / (1024.0 * 1024.0));
            return 1;
        }
        err = cudaMalloc(&d_count, sizeof(uint32_t));
        if (err != cudaSuccess) {
            fprintf(stderr, "FATAL: cudaMalloc count failed: %s\n", cudaGetErrorString(err));
            cudaFree(d_output);
            return 1;
        }
        fprintf(stderr, "CUDA allocation succeeded\n");
        fflush(stderr);

        // Process in batches
        uint64_t total_candidates = 0;
        uint64_t batch_start = gpu_lo;

        while (batch_start < gpu_hi) {
            // Each thread handles one candidate, stride of 4
            // batch_size candidates means batch covers batch_start to batch_start + batch_size*4
            uint64_t batch_end = batch_start + (uint64_t)cfg.batch_size * 4;
            if (batch_end > gpu_hi) batch_end = gpu_hi;

            // Reset output counter (async memset, faster than sync memcpy)
            cudaMemset(d_count, 0, sizeof(uint32_t));

            // Calculate number of threads needed
            uint64_t num_candidates = (batch_end - batch_start + 3) / 4;
            total_candidates += num_candidates;

            // Launch kernel
            int threads_per_block = cfg.block_size;
            int num_blocks = (int)((num_candidates + threads_per_block - 1) / threads_per_block);

            gaussian_prime_kernel<<<num_blocks, threads_per_block>>>(
                batch_start, batch_end, d_output, max_output, d_count
            );

            cudaDeviceSynchronize();
            err = cudaGetLastError();
            if (err != cudaSuccess) {
                fprintf(stderr, "Kernel error: %s\n", cudaGetErrorString(err));
                cudaFree(d_output);
                cudaFree(d_count);
                return 1;
            }

            // Read back count
            uint32_t count = 0;
            cudaMemcpy(&count, d_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);

            if (count > max_output) {
                fprintf(stderr, "WARNING: Output buffer overflow (%u > %u). Results truncated.\n",
                        count, max_output);
                count = max_output;
            }

            // Read back results
            if (count > 0) {
                size_t prev = all_primes.size();
                all_primes.resize(prev + count);
                cudaMemcpy(&all_primes[prev], d_output, count * sizeof(GaussianPrime),
                           cudaMemcpyDeviceToHost);
            }

            fprintf(stderr, "  batch [%lu, %lu): %u primes, total %zu\n",
                    (unsigned long)(batch_end - (uint64_t)cfg.batch_size * 4),
                    (unsigned long)batch_end, count, all_primes.size());
            fflush(stderr);

            batch_start = batch_end;

            // Thermal check between batches
            thermal_check();
        }

        cudaFree(d_output);
        cudaFree(d_count);

        fprintf(stderr, "GPU: processed %lu candidates in batches of %u\n",
                (unsigned long)total_candidates, cfg.batch_size);
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    // --- Sort by (norm, a, b) ---
    std::sort(all_primes.begin(), all_primes.end(),
              [](const GaussianPrime& x, const GaussianPrime& y) {
                  if (x.norm != y.norm) return x.norm < y.norm;
                  if (x.a != y.a) return x.a < y.a;
                  return x.b < y.b;
              });

    // --- Write output ---
    FILE* fout = nullptr;
    if (cfg.output == "/dev/null") {
        fout = fopen("/dev/null", "w");
    } else {
        fout = fopen(cfg.output.c_str(), "w");
    }
    if (!fout) {
        fprintf(stderr, "FATAL: Cannot open output file: %s\n", cfg.output.c_str());
        return 1;
    }

    for (const auto& gp : all_primes) {
        fprintf(fout, "%d %d %lu\n", gp.a, gp.b, (unsigned long)gp.norm);
    }
    fclose(fout);

    // --- Stats ---
    uint64_t total_range = cfg.norm_hi - cfg.norm_lo;
    double primes_per_sec = all_primes.size() / elapsed;
    double candidates_per_sec = (double)total_range / elapsed;

    fprintf(stderr, "\n=== Results ===\n");
    fprintf(stderr, "Norm range:      [%lu, %lu)\n",
            (unsigned long)cfg.norm_lo, (unsigned long)cfg.norm_hi);
    fprintf(stderr, "Primes found:    %zu\n", all_primes.size());
    fprintf(stderr, "Wall time:       %.3f s\n", elapsed);
    fprintf(stderr, "Primes/sec:      %.0f\n", primes_per_sec);
    fprintf(stderr, "Candidates/sec:  %.0f\n", candidates_per_sec);

    int temp = read_gpu_temp();
    if (temp >= 0) {
        fprintf(stderr, "GPU temperature: %d°C\n", temp);
    }

    return 0;
}
