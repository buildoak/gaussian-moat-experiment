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
#include <thrust/sort.h>
#include <thrust/execution_policy.h>

#include "types.h"
#include "device_config.cuh"
#include "modular_arith.cuh"
#include "miller_rabin.cuh"
#include "cornacchia.cuh"
#include "sieve_base.cuh"
#include "sieve_kernel.cuh"
#include "gprf_writer.cuh"

struct GPrimeComparator {
    __host__ __device__
    bool operator()(const GaussianPrime& x, const GaussianPrime& y) const {
        if (x.norm != y.norm) return x.norm < y.norm;
        if (x.a != y.a) return x.a < y.a;
        return x.b < y.b;
    }
};

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
    uint64_t k_squared  = 0;                     // step parameter for GPRF header
    bool use_stdout     = false;                 // write raw GPRF records to stdout
};

static void print_usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s --norm-lo N --norm-hi N (--output FILE | --stdout) [--mode sieve|mr] [--batch-size N] [--block-size N] [--k-squared N]\n"
        "\n"
        "  --norm-lo N      Lower bound of norm range (inclusive)\n"
        "  --norm-hi N      Upper bound of norm range (exclusive)\n"
        "  --output FILE    Output file path (text or .gprf binary)\n"
        "  --stdout         Write raw GPRF records (16-byte: a,b,norm LE) to stdout\n"
        "  --mode MODE      'sieve' (default, fast) or 'mr' (legacy Miller-Rabin)\n"
        "  --batch-size N   Candidates per GPU batch [MR mode] (default 5000000)\n"
        "  --block-size N   Threads per block [MR mode]: 128, 256, or 512 (default 128)\n"
        "  --k-squared N    Step parameter k^2 for GPRF header metadata (default 0)\n",
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
        } else if (strcmp(argv[i], "--k-squared") == 0 && i + 1 < argc) {
            cfg.k_squared = strtoull(argv[++i], nullptr, 10);
        } else if (strcmp(argv[i], "--stdout") == 0) {
            cfg.use_stdout = true;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        }
    }
    if (cfg.norm_hi == 0 || (cfg.output.empty() && !cfg.use_stdout)) {
        print_usage(argv[0]);
        exit(1);
    }
    if (cfg.use_stdout && !cfg.output.empty()) {
        fprintf(stderr, "error: --stdout and --output are mutually exclusive\n");
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
#if THERMAL_CHECK_ENABLED
    int temp = read_gpu_temp();
    if (temp > 78) {
        fprintf(stderr, "[THERMAL] GPU at %d°C — pausing 30s to cool\n", temp);
        cudaDeviceSynchronize();
        struct timespec ts = {30, 0};
        nanosleep(&ts, nullptr);
    }
#endif
}

// ---------------------------------------------------------------------------
// CPU-side: generate inert primes (p ≡ 3 mod 4, norm = p²)
// Used in MR mode and in sieve mode (after split-prime GPU processing).
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
// Sieve mode: segmented sieve (split primes only) → Cornacchia dispatch
// + CPU inert-prime generation.
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
    // PNT: primes per batch ~ batch_span / ln(batch_span), scaled for p == 1 (mod 4) output.
    uint32_t segments_per_batch = SEGMENTS_PER_BATCH_BASE;
    uint64_t batch_norm_span = (uint64_t)segments_per_batch * SEGMENT_SPAN;
    // Cap batch span to actual range for buffer estimation
    uint64_t effective_span = (cfg.norm_hi - cfg.norm_lo < batch_norm_span)
                            ? (cfg.norm_hi - cfg.norm_lo) : batch_norm_span;
    // Use actual norm position for density estimate, not just batch span
    double ln_norm = log((double)(cfg.norm_lo > 10 ? cfg.norm_lo + effective_span : effective_span));
    uint64_t est_primes = (uint64_t)((double)effective_span / ln_norm * 0.8) + 65536;
    // Buffer cap: 160M entries (~3.8GB total for sieve+GP buffers). Safe for 24GB+ VRAM.
    if (est_primes > 160000000ULL) est_primes = 160000000ULL;
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

    // For streaming stdout mode: temp host buffer + running total
    std::vector<GaussianPrime> host_batch_buf;
    uint64_t total_gp_primes = 0;

    uint64_t total_sieve_primes = 0;

    while (current_lo < cfg.norm_hi) {
        uint64_t current_hi = current_lo + batch_norm_span;
        if (current_hi > cfg.norm_hi) current_hi = cfg.norm_hi;

        uint64_t aligned_lo = current_lo & ~1ULL;
        uint64_t total_span = current_hi - aligned_lo;
        uint32_t num_segs = (uint32_t)((total_span + SEGMENT_SPAN - 1) / SEGMENT_SPAN);

        uint64_t* d_bucket_hits = nullptr;
        uint64_t* d_bucket_offsets = nullptr;

        // Build large-prime buckets for this batch.
        // Two-pass: count hits per segment, then fill bit positions.
        // Cache 'first' values across batches to avoid repeated modular reductions.
        if (small_count < base_primes.size() && num_segs > 0u) {
            std::vector<uint32_t> seg_hit_counts(num_segs, 0u);
            const uint64_t start_odd = aligned_lo + 1u;

            // Pass 1: count hits per segment.
            for (uint32_t i = small_count; i < base_primes.size(); ++i) {
                const uint32_t p = base_primes[i];
                if ((p & 1u) == 0u) {
                    continue;
                }

                const uint64_t p64 = static_cast<uint64_t>(p);
                uint64_t first = 0u;
                const uint64_t p2 = p64 * p64;
                if (p2 >= start_odd) {
                    first = p2;
                } else {
                    const uint64_t rem = start_odd % p64;
                    first = (rem == 0u) ? start_odd : (start_odd + (p64 - rem));
                }
                if ((first & 1u) == 0u) {
                    first += p64;
                }

                const uint64_t step = p64 << 1u;
                for (; first < current_hi; first += step) {
                    const uint64_t s64 = (first - aligned_lo) / SEGMENT_SPAN;
                    if (s64 >= num_segs) {
                        break;
                    }
                    seg_hit_counts[static_cast<uint32_t>(s64)]++;
                }
            }

            std::vector<uint64_t> host_bucket_offsets(num_segs + 1u, 0u);
            uint64_t total_hits64 = 0u;
            for (uint32_t s = 0; s < num_segs; ++s) {
                total_hits64 += static_cast<uint64_t>(seg_hit_counts[s]);
                host_bucket_offsets[s + 1u] = total_hits64;
            }

            const uint64_t total_hits = total_hits64;
            std::vector<uint64_t> host_bucket_hits(total_hits > 0u ? total_hits : 1u, 0u);
            std::vector<uint32_t> fill_pos(num_segs, 0u);

            // Pass 2: fill bit positions.
            for (uint32_t i = small_count; i < base_primes.size(); ++i) {
                const uint32_t p = base_primes[i];
                if ((p & 1u) == 0u) {
                    continue;
                }

                const uint64_t p64 = static_cast<uint64_t>(p);
                uint64_t first = 0u;
                const uint64_t p2 = p64 * p64;
                if (p2 >= start_odd) {
                    first = p2;
                } else {
                    const uint64_t rem = start_odd % p64;
                    first = (rem == 0u) ? start_odd : (start_odd + (p64 - rem));
                }
                if ((first & 1u) == 0u) {
                    first += p64;
                }

                const uint64_t step = p64 << 1u;
                for (; first < current_hi; first += step) {
                    const uint64_t s64 = (first - aligned_lo) / SEGMENT_SPAN;
                    if (s64 >= num_segs) {
                        break;
                    }
                    const uint32_t s = static_cast<uint32_t>(s64);
                    const uint64_t seg_lo = aligned_lo + static_cast<uint64_t>(s) * SEGMENT_SPAN;
                    const uint32_t bit = static_cast<uint32_t>((first - seg_lo - 1u) >> 1u);
                    const uint64_t slot = host_bucket_offsets[s] + fill_pos[s];
                    host_bucket_hits[slot] = static_cast<uint64_t>(bit);
                    fill_pos[s]++;
                }
            }

            uint64_t* d_bucket_hits_mut = nullptr;
            uint64_t* d_bucket_offsets_mut = nullptr;
            const size_t bucket_hits_bytes = static_cast<size_t>(host_bucket_hits.size()) * sizeof(uint64_t);
            const size_t bucket_offsets_bytes = static_cast<size_t>(host_bucket_offsets.size()) * sizeof(uint64_t);

            cudaError_t bucket_err = cudaMalloc(&d_bucket_offsets_mut, bucket_offsets_bytes);
            if (bucket_err == cudaSuccess) {
                bucket_err = cudaMemcpy(d_bucket_offsets_mut, host_bucket_offsets.data(),
                                        bucket_offsets_bytes, cudaMemcpyHostToDevice);
            }
            if (bucket_err == cudaSuccess) {
                bucket_err = cudaMalloc(&d_bucket_hits_mut, bucket_hits_bytes);
            }
            if (bucket_err == cudaSuccess) {
                bucket_err = cudaMemcpy(d_bucket_hits_mut, host_bucket_hits.data(),
                                        bucket_hits_bytes, cudaMemcpyHostToDevice);
            }

            if (bucket_err == cudaSuccess) {
                d_bucket_hits = d_bucket_hits_mut;
                d_bucket_offsets = d_bucket_offsets_mut;
            } else {
                if (d_bucket_offsets_mut != nullptr) cudaFree(d_bucket_offsets_mut);
                if (d_bucket_hits_mut != nullptr) cudaFree(d_bucket_hits_mut);
                fprintf(stderr, "WARNING: bucket upload failed, using fallback Phase 2C (%s)\n",
                        cudaGetErrorString(bucket_err));
            }
        }

        // Reset sieve counter
        cudaMemset(d_sieve_count, 0, sizeof(uint32_t));

        // Launch sieve kernel
        int grid = (num_segs < GRID_CAP) ? (int)num_segs : GRID_CAP;
        if (grid < 1) grid = 1;

        segmented_sieve_kernel<<<grid, THREADS_PER_BLK>>>(
            current_lo, current_hi,
            d_base_primes, (uint32_t)base_primes.size(),
            tiny_count, small_count,
            d_bucket_hits, d_bucket_offsets,
            d_sieve_out, max_sieve_output, d_sieve_count
        );
        cudaDeviceSynchronize();
        err = cudaGetLastError();
        if (d_bucket_hits != nullptr) cudaFree(d_bucket_hits);
        if (d_bucket_offsets != nullptr) cudaFree(d_bucket_offsets);
        if (err != cudaSuccess) {
            fprintf(stderr, "Sieve kernel error: %s\n", cudaGetErrorString(err));
            break;
        }

        uint32_t sieve_count = 0;
        cudaMemcpy(&sieve_count, d_sieve_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);

        if (sieve_count > max_sieve_output) {
            fprintf(stderr,
                "FATAL: sieve output overflow — %u primes exceed buffer capacity %u. "
                "Increase EST_PRIME_FACTOR or reduce batch size.\n",
                sieve_count, max_sieve_output);
            exit(1);
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

            if (gp_count > max_sieve_output) {
                fprintf(stderr,
                    "FATAL: Cornacchia output overflow — %u primes exceed buffer capacity %u. "
                    "Increase EST_PRIME_FACTOR or reduce batch size.\n",
                    gp_count, max_sieve_output);
                exit(1);
            }

            if (gp_count > 0) {
                if (cfg.use_stdout) {
                    // Streaming: copy to temp buffer and write directly to stdout
                    host_batch_buf.resize(gp_count);
                    cudaMemcpy(host_batch_buf.data(), d_gp_out, gp_count * sizeof(GaussianPrime),
                               cudaMemcpyDeviceToHost);
                    for (const auto& gp : host_batch_buf) {
                        fwrite(&gp.a, sizeof(gp.a), 1, stdout);
                        fwrite(&gp.b, sizeof(gp.b), 1, stdout);
                        fwrite(&gp.norm, sizeof(gp.norm), 1, stdout);
                    }
                    fflush(stdout);
                } else {
                    size_t prev = all_primes.size();
                    all_primes.resize(prev + gp_count);
                    cudaMemcpy(&all_primes[prev], d_gp_out, gp_count * sizeof(GaussianPrime),
                               cudaMemcpyDeviceToHost);
                }
                total_gp_primes += gp_count;
            }

            total_sieve_primes += sieve_count;

            fprintf(stderr, "  batch [%lu, %lu): sieve=%u, gp=%u, total=%lu\n",
                    (unsigned long)current_lo, (unsigned long)current_hi,
                    sieve_count, gp_count, (unsigned long)total_gp_primes);
        }

        current_lo = current_hi;
        thermal_check();
    }

    fprintf(stderr, "[sieve] Total raw primes from sieve: %lu\n",
            (unsigned long)total_sieve_primes);

    // Add inert primes p == 3 (mod 4), norm = p^2, on CPU.
    if (cfg.use_stdout) {
        // Stream inert primes directly to stdout
        std::vector<GaussianPrime> inert_buf;
        generate_inert_primes(cfg.norm_lo, cfg.norm_hi, inert_buf);
        for (const auto& gp : inert_buf) {
            fwrite(&gp.a, sizeof(gp.a), 1, stdout);
            fwrite(&gp.b, sizeof(gp.b), 1, stdout);
            fwrite(&gp.norm, sizeof(gp.norm), 1, stdout);
        }
        fflush(stdout);
        total_gp_primes += inert_buf.size();
        fprintf(stderr, "[sieve] Streamed %lu inert primes\n", (unsigned long)inert_buf.size());
    } else {
        generate_inert_primes(cfg.norm_lo, cfg.norm_hi, all_primes);
    }

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
                fprintf(stderr,
                    "FATAL: MR output overflow — %u primes exceed buffer capacity %u. "
                    "Increase EST_PRIME_FACTOR or reduce batch size.\n",
                    count, max_output);
                exit(1);
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
        if (cfg.use_stdout) {
            fwrite(&gp.a, sizeof(gp.a), 1, stdout);
            fwrite(&gp.b, sizeof(gp.b), 1, stdout);
            fwrite(&gp.norm, sizeof(gp.norm), 1, stdout);
            fflush(stdout);
        }
        all_primes.push_back(gp);
    }

    // --- Run selected mode ---
    if (cfg.mode == "sieve") {
        run_sieve_mode(cfg, all_primes);
    } else {
        run_mr_mode(cfg, all_primes);
    }

    // --- Sort by (norm, a, b) on GPU ---
    // Skip sort in stdout mode — primes were already streamed per-batch.
    if (!cfg.use_stdout)
    {
        size_t n = all_primes.size();
        bool sort_ok = false;

#if USE_MANAGED_MEMORY
        // Jetson path: unified memory, no explicit copies needed
        GaussianPrime* managed = nullptr;
        cudaError_t merr = cudaMallocManaged(&managed, n * sizeof(GaussianPrime));
        if (merr == cudaSuccess && managed != nullptr) {
            memcpy(managed, all_primes.data(), n * sizeof(GaussianPrime));
            cudaDeviceSynchronize();
            thrust::sort(thrust::device, managed, managed + n, GPrimeComparator());
            cudaDeviceSynchronize();
            memcpy(all_primes.data(), managed, n * sizeof(GaussianPrime));
            cudaFree(managed);
            sort_ok = true;
        }
#else
        // Discrete GPU path (A100 etc): explicit cudaMalloc + cudaMemcpy
        GaussianPrime* d_sort = nullptr;
        cudaError_t merr = cudaMalloc(&d_sort, n * sizeof(GaussianPrime));
        if (merr == cudaSuccess && d_sort != nullptr) {
            cudaMemcpy(d_sort, all_primes.data(), n * sizeof(GaussianPrime),
                       cudaMemcpyHostToDevice);
            thrust::sort(thrust::device, d_sort, d_sort + n, GPrimeComparator());
            cudaDeviceSynchronize();
            cudaMemcpy(all_primes.data(), d_sort, n * sizeof(GaussianPrime),
                       cudaMemcpyDeviceToHost);
            cudaFree(d_sort);
            sort_ok = true;
        }
#endif

        if (!sort_ok) {
            // Fallback: CPU sort if GPU alloc fails
            std::sort(all_primes.begin(), all_primes.end(),
                      [](const GaussianPrime& x, const GaussianPrime& y) {
                          if (x.norm != y.norm) return x.norm < y.norm;
                          if (x.a != y.a) return x.a < y.a;
                          return x.b < y.b;
                      });
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t_end - t_start).count();

    // --- Write output ---
    if (cfg.use_stdout) {
        // Already streamed per-batch in run_sieve_mode / run_mr_mode.
        // Nothing to write here.
    } else {
        bool use_gprf = (cfg.output.size() >= 5 &&
                         cfg.output.compare(cfg.output.size() - 5, 5, ".gprf") == 0);
        if (use_gprf) {
            PrimeFileWriter writer(cfg.output.c_str(), cfg.norm_hi, cfg.k_squared);
            for (const auto& gp : all_primes) {
                writer.write(gp.a, gp.b, gp.norm);
            }
            writer.finalize();
        } else {
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
        }
    }

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
