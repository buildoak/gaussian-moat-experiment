// main.cu — Production Gaussian prime generator
//
// Enumerates all Gaussian primes with norms in [norm_lo, norm_hi).
// Three categories:
//   1. Ramified prime: (1,1) with norm 2  (always if range includes 2)
//   2. Split primes: p ≡ 1 (mod 4), decompose via Cornacchia → (a,b,p)
//   3. Inert primes: p ≡ 3 (mod 4), emit (p,0,p²)
//
// Two modes:
//   --mode mr      Miller-Rabin kernel (legacy, per-candidate primality test)
//   --mode sieve   Segmented sieve kernel (default, bulk sieve + Cornacchia)

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
#include "sieve_base.cuh"
#include "sieve_kernel.cuh"

// Declared in kernel.cu (MR kernel)
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
    std::string mode    = "sieve";   // "sieve" (default) or "mr"
    uint32_t batch_size = 5u * 1000u * 1000u;   // 5M candidates per batch (MR mode)
    int block_size      = 128;                   // threads per block (MR mode)
};

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s --norm-lo N --norm-hi N --output FILE [--mode sieve|mr] [--batch-size N] [--block-size N]\n"
        "\n"
        "  --norm-lo N      Lower bound of norm range (inclusive)\n"
        "  --norm-hi N      Upper bound of norm range (exclusive)\n"
        "  --output FILE    Output file path (text: a b norm per line)\n"
        "  --mode MODE      'sieve' (default, fast) or 'mr' (legacy Miller-Rabin)\n"
        "  --batch-size N   Candidates per GPU batch [MR mode] (default 5000000)\n"
        "  --block-size N   Threads per block [MR mode]: 128, 256, or 512 (default 128)\n",
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
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            cfg.mode = argv[++i];
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
    if (cfg.mode != "sieve" && cfg.mode != "mr") {
        fprintf(stderr, "Unknown mode: %s (use 'sieve' or 'mr')\n", cfg.mode.c_str());
        exit(1);
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Thermal management
// ---------------------------------------------------------------------------
static int read_gpu_temp() {
    FILE* f = fopen("/sys/devices/virtual/thermal/thermal_zone1/temp", "r");
    if (!f) {
        f = fopen("/sys/devices/virtual/thermal/thermal_zone0/temp", "r");
    }
    if (!f) return -1;
    int temp = 0;
    if (fscanf(f, "%d", &temp) == 1) {
        fclose(f);
        return temp / 1000;
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
// Used only in MR mode. In sieve mode, the Cornacchia dispatch kernel
// handles inert primes from the sieve's prime output.
// ---------------------------------------------------------------------------
static void generate_inert_primes(uint64_t norm_lo, uint64_t norm_hi,
                                  std::vector<GaussianPrime>& out) {
    uint64_t p_lo = (norm_lo <= 1) ? 3 : isqrt64(norm_lo);
    while (p_lo > 0 && p_lo * p_lo < norm_lo) p_lo++;
    if (p_lo < 3) p_lo = 3;
    if (p_lo % 2 == 0) p_lo++;
    if (p_lo % 4 == 1) p_lo += 2;

    uint64_t p_hi_sq = norm_hi - 1;
    uint64_t p_hi = isqrt64(p_hi_sq);

    for (uint64_t p = p_lo; p <= p_hi; p += 4) {
        if (p * p >= norm_hi) break;
        if (p * p < norm_lo) continue;
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
// Sieve mode: segmented sieve → Cornacchia dispatch
// ---------------------------------------------------------------------------
static void run_sieve_mode(const Config& cfg,
                           std::vector<GaussianPrime>& all_primes) {
    // 1. Generate base primes via CPU sieve
    uint64_t sqrt_hi = (uint64_t)sqrt((double)cfg.norm_hi) + 2;
    fprintf(stderr, "[sieve] Generating base primes up to %lu...\n", (unsigned long)sqrt_hi);
    auto t_base = std::chrono::high_resolution_clock::now();

    auto base_primes = simple_sieve_cpu(sqrt_hi);

    auto t_base_end = std::chrono::high_resolution_clock::now();
    double base_elapsed = std::chrono::duration<double>(t_base_end - t_base).count();
    fprintf(stderr, "[sieve] %zu base primes in %.3f s\n", base_primes.size(), base_elapsed);

    // Partition into tiny / small / large
    uint32_t tiny_count = partition_primes(base_primes, TINY_THRESHOLD);
    uint32_t small_count = partition_primes(base_primes, SEGMENT_SPAN);
    fprintf(stderr, "[sieve] tiny=%u, small=%u, large=%zu\n",
            tiny_count, small_count, base_primes.size() - small_count);

    // 2. Upload base primes to GPU
    uint32_t* d_base_primes = nullptr;
    cudaError_t err = cudaMalloc(&d_base_primes, base_primes.size() * sizeof(uint32_t));
    if (err != cudaSuccess) {
        fprintf(stderr, "FATAL: cudaMalloc base primes: %s\n", cudaGetErrorString(err));
        exit(1);
    }
    cudaMemcpy(d_base_primes, base_primes.data(),
               base_primes.size() * sizeof(uint32_t), cudaMemcpyHostToDevice);

    // 3. Allocate sieve output buffer
    // Size based on per-batch capacity.
    // Each batch covers segments_per_batch * SEGMENT_SPAN norms.
    // PNT: primes per batch ~ batch_span / ln(batch_span). 1.5x safety margin.
    uint32_t segments_per_batch = 10000;
    uint64_t batch_norm_span = (uint64_t)segments_per_batch * SEGMENT_SPAN;
    // Cap batch span to actual range for buffer estimation
    uint64_t effective_span = (cfg.norm_hi - cfg.norm_lo < batch_norm_span)
                            ? (cfg.norm_hi - cfg.norm_lo) : batch_norm_span;
    double ln_batch = log((double)(effective_span > 10 ? effective_span : 10));
    uint64_t est_primes = (uint64_t)((double)effective_span / ln_batch * 1.5) + 65536;
    if (est_primes > 120000000ULL) est_primes = 120000000ULL;
    uint32_t max_sieve_output = (uint32_t)est_primes;

    uint64_t* d_sieve_out = nullptr;
    uint32_t* d_sieve_count = nullptr;
    err = cudaMalloc(&d_sieve_out, (size_t)max_sieve_output * sizeof(uint64_t));
    if (err != cudaSuccess) {
        fprintf(stderr, "FATAL: cudaMalloc sieve output: %s (%.1f MB)\n",
                cudaGetErrorString(err),
                (double)max_sieve_output * sizeof(uint64_t) / (1024.0 * 1024.0));
        exit(1);
    }
    cudaMalloc(&d_sieve_count, sizeof(uint32_t));

    // 4. Allocate Cornacchia output buffer
    GaussianPrime* d_gp_out = nullptr;
    uint32_t* d_gp_count = nullptr;
    cudaMalloc(&d_gp_out, (size_t)max_sieve_output * sizeof(GaussianPrime));
    cudaMalloc(&d_gp_count, sizeof(uint32_t));

    fprintf(stderr, "[sieve] Sieve buffer: %u entries (%.1f MB), GP buffer: %.1f MB\n",
            max_sieve_output,
            (double)max_sieve_output * sizeof(uint64_t) / (1024.0 * 1024.0),
            (double)max_sieve_output * sizeof(GaussianPrime) / (1024.0 * 1024.0));

    // 5. Process in macro-batches to fit in output buffer
    // Each batch covers segments_per_batch * SEGMENT_SPAN norms

    uint64_t current_lo = cfg.norm_lo;
    // Even-align
    if (current_lo % 2 != 0 && current_lo > 0) current_lo--;

    uint64_t total_sieve_primes = 0;

    while (current_lo < cfg.norm_hi) {
        uint64_t current_hi = current_lo + batch_norm_span;
        if (current_hi > cfg.norm_hi) current_hi = cfg.norm_hi;

        uint64_t aligned_lo = current_lo & ~1ULL;
        uint64_t total_span = current_hi - aligned_lo;
        uint32_t num_segs = (uint32_t)((total_span + SEGMENT_SPAN - 1) / SEGMENT_SPAN);

        // Reset sieve counter
        cudaMemset(d_sieve_count, 0, sizeof(uint32_t));

        // Launch sieve kernel
        int grid = (num_segs < 1024) ? (int)num_segs : 1024;
        if (grid < 1) grid = 1;

        segmented_sieve_kernel<<<grid, THREADS_PER_BLK>>>(
            current_lo, current_hi,
            d_base_primes, (uint32_t)base_primes.size(),
            tiny_count, small_count,
            nullptr, nullptr,  // No bucketed large primes (Phase 2C handles them in-kernel)
            d_sieve_out, max_sieve_output, d_sieve_count
        );
        cudaDeviceSynchronize();
        err = cudaGetLastError();
        if (err != cudaSuccess) {
            fprintf(stderr, "Sieve kernel error: %s\n", cudaGetErrorString(err));
            break;
        }

        uint32_t sieve_count = 0;
        cudaMemcpy(&sieve_count, d_sieve_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);

        if (sieve_count > max_sieve_output) {
            fprintf(stderr, "WARNING: Sieve buffer overflow (%u > %u)\n", sieve_count, max_sieve_output);
            sieve_count = max_sieve_output;
        }

        // Launch Cornacchia dispatch kernel
        if (sieve_count > 0) {
            cudaMemset(d_gp_count, 0, sizeof(uint32_t));

            int corn_grid = (sieve_count + 255) / 256;
            cornacchia_dispatch_kernel<<<corn_grid, 256>>>(
                d_sieve_out, sieve_count,
                d_gp_out, max_sieve_output, d_gp_count,
                cfg.norm_lo, cfg.norm_hi
            );
            cudaDeviceSynchronize();
            err = cudaGetLastError();
            if (err != cudaSuccess) {
                fprintf(stderr, "Cornacchia kernel error: %s\n", cudaGetErrorString(err));
                break;
            }

            uint32_t gp_count = 0;
            cudaMemcpy(&gp_count, d_gp_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);

            if (gp_count > 0) {
                size_t prev = all_primes.size();
                all_primes.resize(prev + gp_count);
                cudaMemcpy(&all_primes[prev], d_gp_out, gp_count * sizeof(GaussianPrime),
                           cudaMemcpyDeviceToHost);
            }

            total_sieve_primes += sieve_count;

            fprintf(stderr, "  batch [%lu, %lu): sieve=%u, gp=%u, total=%zu\n",
                    (unsigned long)current_lo, (unsigned long)current_hi,
                    sieve_count, gp_count, all_primes.size());
        }

        current_lo = current_hi;
        thermal_check();
    }

    fprintf(stderr, "[sieve] Total raw primes from sieve: %lu\n",
            (unsigned long)total_sieve_primes);

    // Cleanup
    cudaFree(d_base_primes);
    cudaFree(d_sieve_out);
    cudaFree(d_sieve_count);
    cudaFree(d_gp_out);
    cudaFree(d_gp_count);
}

// ---------------------------------------------------------------------------
// MR mode: Miller-Rabin kernel (legacy)
// ---------------------------------------------------------------------------
static void run_mr_mode(const Config& cfg,
                        std::vector<GaussianPrime>& all_primes) {
    // Inert primes (CPU-side)
    generate_inert_primes(cfg.norm_lo, cfg.norm_hi, all_primes);

    // Split primes (GPU-side)
    uint64_t gpu_lo = (cfg.norm_lo < 5) ? 5 : cfg.norm_lo;
    while (gpu_lo % 4 != 1) gpu_lo++;
    uint64_t gpu_hi = cfg.norm_hi;

    if (gpu_lo < gpu_hi) {
        double ln_hi = (gpu_hi > 10) ? log((double)gpu_hi) : 3.0;
        uint64_t estimated_primes = (uint64_t)((double)gpu_hi / ln_hi * 0.6) + 4096;
        if (estimated_primes > 20000000ULL) estimated_primes = 20000000ULL;
        uint32_t max_output = (uint32_t)estimated_primes;

        fprintf(stderr, "Output buffer: %u entries (%.1f MB)\n",
                max_output, (double)max_output * sizeof(GaussianPrime) / (1024.0 * 1024.0));

        GaussianPrime* d_output = nullptr;
        uint32_t* d_count = nullptr;
        cudaError_t err = cudaMalloc(&d_output, (size_t)max_output * sizeof(GaussianPrime));
        if (err != cudaSuccess) {
            fprintf(stderr, "FATAL: cudaMalloc: %s\n", cudaGetErrorString(err));
            exit(1);
        }
        cudaMalloc(&d_count, sizeof(uint32_t));

        uint64_t total_candidates = 0;
        uint64_t batch_start = gpu_lo;

        while (batch_start < gpu_hi) {
            uint64_t batch_end = batch_start + (uint64_t)cfg.batch_size * 4;
            if (batch_end > gpu_hi) batch_end = gpu_hi;

            cudaMemset(d_count, 0, sizeof(uint32_t));

            uint64_t num_candidates = (batch_end - batch_start + 3) / 4;
            total_candidates += num_candidates;

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
                exit(1);
            }

            uint32_t count = 0;
            cudaMemcpy(&count, d_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);

            if (count > max_output) {
                fprintf(stderr, "WARNING: Output buffer overflow (%u > %u)\n", count, max_output);
                count = max_output;
            }

            if (count > 0) {
                size_t prev = all_primes.size();
                all_primes.resize(prev + count);
                cudaMemcpy(&all_primes[prev], d_output, count * sizeof(GaussianPrime),
                           cudaMemcpyDeviceToHost);
            }

            fprintf(stderr, "  batch [%lu, %lu): %u primes, total %zu\n",
                    (unsigned long)(batch_end - (uint64_t)cfg.batch_size * 4),
                    (unsigned long)batch_end, count, all_primes.size());

            batch_start = batch_end;
            thermal_check();
        }

        cudaFree(d_output);
        cudaFree(d_count);

        fprintf(stderr, "GPU: processed %lu candidates in batches of %u\n",
                (unsigned long)total_candidates, cfg.batch_size);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    fprintf(stderr, "gm_cuda_primes — mode=%s, norm range [%lu, %lu)\n",
            cfg.mode.c_str(),
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

    // --- Run selected mode ---
    if (cfg.mode == "sieve") {
        run_sieve_mode(cfg, all_primes);
    } else {
        run_mr_mode(cfg, all_primes);
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
    fprintf(stderr, "Mode:            %s\n", cfg.mode.c_str());
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
