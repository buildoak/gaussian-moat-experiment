#include "gpu_constants.cuh"
#include "gpu_types.cuh"

#include <cuda_runtime.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <vector>

__global__ void process_tiles_kernel(const TileCoord* coords,
                                     TileOp* output,
                                     uint32_t* prime_counts,
#ifdef PROFILE_PHASES
                                     PhaseTimingGPU* phase_timings,
#endif
                                     int num_tiles);

void upload_sieve_tables(const SieveTablesBarrett& tables);
void upload_backward_offsets(const int8_t* bk_dr, const int8_t* bk_dc, int count);
void upload_constants();
size_t tile_kernel_shared_bytes();

namespace {

constexpr uint32_t SIEVE_LIMIT = 10000U;

inline void check_cuda(cudaError_t status, const char* what) {
    if (status != cudaSuccess) {
        std::fprintf(stderr, "%s failed: %s\n", what, cudaGetErrorString(status));
        std::abort();
    }
}

uint64_t mulmod_small(uint64_t a, uint64_t b, uint64_t m) {
    return (a * b) % m;
}

uint64_t fast_sqrt_neg1(uint64_t p) {
    for (uint64_t x = 1; x < p; ++x) {
        if (mulmod_small(x, x, p) == (p - 1ULL)) {
            return x;
        }
    }
    return std::numeric_limits<uint64_t>::max();
}

uint32_t barrett_host_mod(uint32_t x, uint32_t p, uint32_t mu) {
    uint32_t q = static_cast<uint32_t>((static_cast<uint64_t>(x) * mu) >> 32);
    uint32_t r = x - q * p;
    if (r >= p) r -= p;
    return r;
}

bool init_sieve_tables_host(SieveTablesBarrett& tables) {
    std::memset(&tables, 0, sizeof(tables));

    uint8_t is_prime_table[SIEVE_LIMIT + 1];
    std::memset(is_prime_table, 1, sizeof(is_prime_table));
    is_prime_table[0] = 0U;
    is_prime_table[1] = 0U;

    for (uint32_t p = 2U; p * p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) {
            continue;
        }
        for (uint32_t multiple = p * p; multiple <= SIEVE_LIMIT; multiple += p) {
            is_prime_table[multiple] = 0U;
        }
    }

    for (uint32_t p = 2U; p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) {
            continue;
        }

        const uint32_t mu = static_cast<uint32_t>((1ULL << 32) / static_cast<uint64_t>(p));

        if ((p & 3U) == 1U) {
            const uint64_t root_raw = fast_sqrt_neg1(static_cast<uint64_t>(p));
            if (root_raw == std::numeric_limits<uint64_t>::max()) {
                return false;
            }

            uint64_t root = root_raw;
            const uint64_t neg_root = static_cast<uint64_t>(p) - root;
            if (neg_root < root) {
                root = neg_root;
            }

            if (mulmod_small(root, root, static_cast<uint64_t>(p)) != static_cast<uint64_t>(p - 1U)) {
                return false;
            }
            if (tables.split_count >= SPLIT_PRIMES_COUNT) {
                return false;
            }

            // Validate Barrett for this prime
            const uint32_t root32 = static_cast<uint32_t>(root);
            if (barrett_host_mod(root32 * root32, p, mu) != p - 1U) {
                std::fprintf(stderr, "Barrett validation failed for split prime %u\n", p);
                return false;
            }

            tables.split_table[tables.split_count++] = SplitPrimeBarrettGPU{
                static_cast<uint16_t>(p),
                static_cast<uint16_t>(root),
                mu
            };
        } else if ((p & 3U) == 3U) {
            if (tables.inert_count >= INERT_PRIMES_COUNT) {
                return false;
            }
            tables.inert_primes[tables.inert_count++] = InertPrimeBarrettGPU{
                static_cast<uint16_t>(p),
                0,  // pad
                mu
            };
        }
    }

    return tables.split_count == SPLIT_PRIMES_COUNT &&
           tables.inert_count == INERT_PRIMES_COUNT;
}

#ifdef PROFILE_PHASES
struct TimingSeriesStats {
    int64_t min_cycles;
    int64_t max_cycles;
    double mean_cycles;
    double median_cycles;
};

TimingSeriesStats compute_timing_stats(const std::vector<int64_t>& samples) {
    if (samples.empty()) {
        return TimingSeriesStats{0, 0, 0.0, 0.0};
    }

    std::vector<int64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    long double sum = 0.0;
    for (const int64_t sample : samples) {
        sum += static_cast<long double>(sample);
    }

    const size_t mid = sorted.size() / 2;
    const double median = (sorted.size() % 2 == 0)
        ? (static_cast<double>(sorted[mid - 1]) + static_cast<double>(sorted[mid])) / 2.0
        : static_cast<double>(sorted[mid]);

    return TimingSeriesStats{
        sorted.front(),
        sorted.back(),
        static_cast<double>(sum / static_cast<long double>(samples.size())),
        median,
    };
}

void print_phase_report(const std::vector<PhaseTimingGPU>& timings) {
    struct PhaseColumn {
        const char* label;
        int64_t PhaseTimingGPU::*member;
    };

    static constexpr PhaseColumn kColumns[] = {
        {"phase1a", &PhaseTimingGPU::phase1a_cycles},
        {"phase1b", &PhaseTimingGPU::phase1b_cycles},
        {"phase1c", &PhaseTimingGPU::phase1c_cycles},
        {"phase2", &PhaseTimingGPU::phase2_cycles},
        {"phase3", &PhaseTimingGPU::phase3_cycles},
        {"phase45", &PhaseTimingGPU::phase45_cycles},
        {"total", &PhaseTimingGPU::total_cycles},
    };

    long double total_cycle_sum = 0.0;
    for (const PhaseTimingGPU& timing : timings) {
        total_cycle_sum += static_cast<long double>(timing.total_cycles);
    }

    std::printf("\n--- phase timing (cycles) ---\n");
    for (const PhaseColumn& column : kColumns) {
        std::vector<int64_t> samples;
        samples.reserve(timings.size());
        long double phase_sum = 0.0;
        for (const PhaseTimingGPU& timing : timings) {
            const int64_t value = timing.*(column.member);
            samples.push_back(value);
            phase_sum += static_cast<long double>(value);
        }

        const TimingSeriesStats stats = compute_timing_stats(samples);
        const bool is_total = std::strcmp(column.label, "total") == 0;
        const double pct_total = (is_total || total_cycle_sum == 0.0)
            ? 100.0
            : static_cast<double>((phase_sum / total_cycle_sum) * 100.0);
        std::printf("%-7s min=%lld max=%lld mean=%.1f median=%.1f pct_total=%.2f%%\n",
                    column.label,
                    static_cast<long long>(stats.min_cycles),
                    static_cast<long long>(stats.max_cycles),
                    stats.mean_cycles,
                    stats.median_cycles,
                    pct_total);
    }
}
#endif

struct BackwardOffsetsHost {
    int8_t dr[NUM_BACKWARD_OFFSETS];
    int8_t dc[NUM_BACKWARD_OFFSETS];
    int count;
};

void init_backward_offsets_host(BackwardOffsetsHost& offsets) {
    int count = 0;

    for (int dr = -COLLAR; dr <= 0; ++dr) {
        for (int dc = -COLLAR; dc <= COLLAR; ++dc) {
            if ((dr > 0) || (dr == 0 && dc >= 0)) {
                continue;
            }
            if ((dr * dr + dc * dc) > K_SQ) {
                continue;
            }

            if (count >= NUM_BACKWARD_OFFSETS) {
                std::fprintf(stderr, "backward offset overflow\n");
                std::abort();
            }
            offsets.dr[count] = static_cast<int8_t>(dr);
            offsets.dc[count] = static_cast<int8_t>(dc);
            ++count;
        }
    }

    offsets.count = count;
    if (offsets.count != NUM_BACKWARD_OFFSETS) {
        std::fprintf(stderr, "backward offset count mismatch: expected %d got %d\n",
                     NUM_BACKWARD_OFFSETS, offsets.count);
        std::abort();
    }
}

std::vector<TileCoord> default_tiles() {
    return {
        TileCoord{600000000LL, 600000000LL},
        TileCoord{699999744LL, 400000000LL},
    };
}

// Generate N tile coordinates on a grid near ~45 degrees at R=860M.
// Starts at (608000000, 608000000) — both divisible by 256, at radius ~860M.
// Lays out tiles in a square grid pattern, advancing b_lo first then a_lo.
std::vector<TileCoord> generate_bench_tiles(int n) {
    std::vector<TileCoord> coords;
    coords.reserve(n);

    constexpr int64_t A_ORIGIN = 608000000LL;  // divisible by 256
    constexpr int64_t B_ORIGIN = 608000000LL;

    // Grid width: sqrt(n) tiles wide, rest fill next rows
    const int grid_w = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));

    for (int i = 0; i < n; ++i) {
        const int col = i % grid_w;
        const int row = i / grid_w;
        coords.push_back(TileCoord{
            A_ORIGIN + static_cast<int64_t>(row) * TILE_SIDE,
            B_ORIGIN + static_cast<int64_t>(col) * TILE_SIDE,
        });
    }
    return coords;
}

void print_device_info() {
    int device = 0;
    check_cuda(cudaGetDevice(&device), "cudaGetDevice");

    cudaDeviceProp prop{};
    check_cuda(cudaGetDeviceProperties(&prop, device), "cudaGetDeviceProperties");

    std::printf("device: %s\n", prop.name);
    std::printf("  SMs: %d\n", prop.multiProcessorCount);
    std::printf("  max threads/SM: %d\n", prop.maxThreadsPerMultiProcessor);
    std::printf("  max shared mem/SM: %zu bytes\n", prop.sharedMemPerMultiprocessor);
    std::printf("  max shared mem/block: %zu bytes\n", prop.sharedMemPerBlock);
    std::printf("  max shared mem/block (optin): %zu bytes\n", prop.sharedMemPerBlockOptin);
    std::printf("  clock rate: %d MHz\n", prop.clockRate / 1000);
    std::printf("  memory clock: %d MHz\n", prop.memoryClockRate / 1000);
}

void print_kernel_occupancy(size_t shared_bytes) {
    int num_blocks = 0;
    cudaError_t occ_err = cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &num_blocks, process_tiles_kernel, BLOCK_THREADS, shared_bytes);
    if (occ_err == cudaSuccess) {
        std::printf("  max active blocks/SM: %d\n", num_blocks);
        std::printf("  occupancy: %d/%d threads per SM (%.1f%%)\n",
                    num_blocks * BLOCK_THREADS, 1536,
                    100.0 * static_cast<double>(num_blocks * BLOCK_THREADS) / 1536.0);
    } else {
        std::printf("  occupancy query failed: %s\n", cudaGetErrorString(occ_err));
    }

    cudaFuncAttributes attr{};
    cudaError_t attr_err = cudaFuncGetAttributes(&attr, process_tiles_kernel);
    if (attr_err == cudaSuccess) {
        std::printf("  registers/thread: %d\n", attr.numRegs);
        std::printf("  static shared mem: %zu bytes\n", attr.sharedSizeBytes);
        std::printf("  const mem: %zu bytes\n", attr.constSizeBytes);
    } else {
        std::printf("  func attributes query failed: %s\n", cudaGetErrorString(attr_err));
    }
}

enum class Mode { TEST, BENCH };

struct Args {
    Mode mode;
    int tile_count;
};

Args parse_args(int argc, char** argv) {
    if (argc < 2) {
        return Args{Mode::TEST, 2};
    }
    if (std::strcmp(argv[1], "test") == 0) {
        return Args{Mode::TEST, 2};
    }
    const int n = std::atoi(argv[1]);
    if (n <= 0) {
        std::fprintf(stderr, "usage: %s [test | <tile_count>]\n", argv[0]);
        std::fprintf(stderr, "  test          — 2-tile smoke test (default)\n");
        std::fprintf(stderr, "  <tile_count>  — benchmark N tiles\n");
        std::exit(1);
    }
    return Args{Mode::BENCH, n};
}

void run_test() {
    const size_t shared_bytes = tile_kernel_shared_bytes();
    std::vector<TileCoord> coords = default_tiles();
    const int num_tiles = static_cast<int>(coords.size());

    TileCoord* d_coords = nullptr;
    TileOp* d_output = nullptr;
    uint32_t* d_prime_counts = nullptr;
#ifdef PROFILE_PHASES
    PhaseTimingGPU* d_phase_timings = nullptr;
#endif

    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_coords), sizeof(TileCoord) * coords.size()),
               "cudaMalloc(d_coords)");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_output), sizeof(TileOp) * coords.size()),
               "cudaMalloc(d_output)");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_prime_counts),
                          sizeof(uint32_t) * coords.size()),
               "cudaMalloc(d_prime_counts)");
#ifdef PROFILE_PHASES
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_phase_timings),
                          sizeof(PhaseTimingGPU) * coords.size()),
               "cudaMalloc(d_phase_timings)");
#endif

    check_cuda(cudaMemcpy(d_coords, coords.data(), sizeof(TileCoord) * coords.size(),
                          cudaMemcpyHostToDevice),
               "cudaMemcpy(d_coords)");

    check_cuda(cudaFuncSetAttribute(process_tiles_kernel,
                                    cudaFuncAttributeMaxDynamicSharedMemorySize,
                                    static_cast<int>(shared_bytes)),
               "cudaFuncSetAttribute(process_tiles_kernel)");

    const dim3 block(BLOCK_THREADS);
    const dim3 grid(static_cast<unsigned int>(num_tiles));

    cudaEvent_t start{};
    cudaEvent_t stop{};
    check_cuda(cudaEventCreate(&start), "cudaEventCreate(start)");
    check_cuda(cudaEventCreate(&stop), "cudaEventCreate(stop)");

    check_cuda(cudaEventRecord(start), "cudaEventRecord(start)");
    process_tiles_kernel<<<grid, block, shared_bytes>>>(
        d_coords, d_output, d_prime_counts,
#ifdef PROFILE_PHASES
        d_phase_timings,
#endif
        num_tiles);
    check_cuda(cudaEventRecord(stop), "cudaEventRecord(stop)");
    check_cuda(cudaGetLastError(), "process_tiles_kernel launch");
    check_cuda(cudaEventSynchronize(stop), "cudaEventSynchronize(stop)");

    float elapsed_ms = 0.0f;
    check_cuda(cudaEventElapsedTime(&elapsed_ms, start, stop), "cudaEventElapsedTime");

    std::vector<TileOp> output(coords.size());
    std::vector<uint32_t> prime_counts(coords.size(), 0U);
    check_cuda(cudaMemcpy(output.data(), d_output, sizeof(TileOp) * output.size(),
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy(output)");
    check_cuda(cudaMemcpy(prime_counts.data(), d_prime_counts,
                          sizeof(uint32_t) * prime_counts.size(),
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy(prime_counts)");
#ifdef PROFILE_PHASES
    std::vector<PhaseTimingGPU> phase_timings(coords.size());
    check_cuda(cudaMemcpy(phase_timings.data(), d_phase_timings,
                          sizeof(PhaseTimingGPU) * phase_timings.size(),
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy(phase_timings)");
#endif

    std::printf("=== SMOKE TEST ===\n");
    std::printf("processed %d tile(s) in %.3f ms (%.3f ms/tile)\n",
                num_tiles, elapsed_ms, elapsed_ms / static_cast<float>(num_tiles));
    std::printf("shared memory: %zu bytes\n", shared_bytes);

    for (int i = 0; i < num_tiles; ++i) {
        std::printf("tile[%d] coord=(%lld,%lld) prime_count=%u tileop[0..3]=%02x %02x %02x %02x\n",
                    i,
                    static_cast<long long>(coords[i].a_lo),
                    static_cast<long long>(coords[i].b_lo),
                    prime_counts[i],
                    output[i].bytes[0],
                    output[i].bytes[1],
                    output[i].bytes[2],
                    output[i].bytes[3]);
    }

#ifdef PROFILE_PHASES
    print_phase_report(phase_timings);
#endif

    cudaEventDestroy(stop);
    cudaEventDestroy(start);
#ifdef PROFILE_PHASES
    cudaFree(d_phase_timings);
#endif
    cudaFree(d_prime_counts);
    cudaFree(d_output);
    cudaFree(d_coords);
}

void run_bench(int tile_count) {
    const size_t shared_bytes = tile_kernel_shared_bytes();
    std::printf("=== BENCHMARK: %d tiles ===\n", tile_count);

    // --- Device info and occupancy ---
    print_device_info();
    std::printf("kernel config:\n");
    std::printf("  block threads: %d\n", BLOCK_THREADS);
    std::printf("  dynamic shared mem: %zu bytes\n", shared_bytes);
    print_kernel_occupancy(shared_bytes);

    // --- Generate tile coordinates ---
    std::vector<TileCoord> coords = generate_bench_tiles(tile_count);
    std::printf("tile grid: %d tiles, origin=(%lld,%lld)\n",
                tile_count,
                static_cast<long long>(coords[0].a_lo),
                static_cast<long long>(coords[0].b_lo));

    // --- Allocate device memory ---
    TileCoord* d_coords = nullptr;
    TileOp* d_output = nullptr;
    uint32_t* d_prime_counts = nullptr;
#ifdef PROFILE_PHASES
    PhaseTimingGPU* d_phase_timings = nullptr;
#endif

    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_coords),
                          sizeof(TileCoord) * tile_count),
               "cudaMalloc(d_coords)");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_output),
                          sizeof(TileOp) * tile_count),
               "cudaMalloc(d_output)");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_prime_counts),
                          sizeof(uint32_t) * tile_count),
               "cudaMalloc(d_prime_counts)");
#ifdef PROFILE_PHASES
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_phase_timings),
                          sizeof(PhaseTimingGPU) * tile_count),
               "cudaMalloc(d_phase_timings)");
#endif

    check_cuda(cudaMemcpy(d_coords, coords.data(),
                          sizeof(TileCoord) * tile_count,
                          cudaMemcpyHostToDevice),
               "cudaMemcpy(d_coords)");

    check_cuda(cudaFuncSetAttribute(process_tiles_kernel,
                                    cudaFuncAttributeMaxDynamicSharedMemorySize,
                                    static_cast<int>(shared_bytes)),
               "cudaFuncSetAttribute(process_tiles_kernel)");

    const dim3 block(BLOCK_THREADS);

    // --- Warmup: 10 tiles, discard timing ---
    {
        const int warmup_tiles = (tile_count < 10) ? tile_count : 10;
        const dim3 warmup_grid(static_cast<unsigned int>(warmup_tiles));
        process_tiles_kernel<<<warmup_grid, block, shared_bytes>>>(
            d_coords, d_output, d_prime_counts,
#ifdef PROFILE_PHASES
            d_phase_timings,
#endif
            warmup_tiles);
        check_cuda(cudaGetLastError(), "warmup launch");
        check_cuda(cudaDeviceSynchronize(), "warmup sync");
        std::printf("warmup: %d tiles done\n", warmup_tiles);
    }

    // --- Benchmark pass ---
    cudaEvent_t start{};
    cudaEvent_t stop{};
    check_cuda(cudaEventCreate(&start), "cudaEventCreate(start)");
    check_cuda(cudaEventCreate(&stop), "cudaEventCreate(stop)");

    const dim3 grid(static_cast<unsigned int>(tile_count));

    check_cuda(cudaEventRecord(start), "cudaEventRecord(start)");
    process_tiles_kernel<<<grid, block, shared_bytes>>>(
        d_coords, d_output, d_prime_counts,
#ifdef PROFILE_PHASES
        d_phase_timings,
#endif
        tile_count);
    check_cuda(cudaEventRecord(stop), "cudaEventRecord(stop)");
    check_cuda(cudaGetLastError(), "benchmark launch");
    check_cuda(cudaEventSynchronize(stop), "cudaEventSynchronize(stop)");

    float elapsed_ms = 0.0f;
    check_cuda(cudaEventElapsedTime(&elapsed_ms, start, stop), "cudaEventElapsedTime");

    // --- Results ---
    const double ms_per_tile = static_cast<double>(elapsed_ms) / static_cast<double>(tile_count);
    const double tiles_per_sec = static_cast<double>(tile_count) / (static_cast<double>(elapsed_ms) / 1000.0);

    std::printf("\n--- results ---\n");
    std::printf("total:      %.3f ms\n", elapsed_ms);
    std::printf("tiles:      %d\n", tile_count);
    std::printf("ms/tile:    %.4f\n", ms_per_tile);
    std::printf("tiles/sec:  %.1f\n", tiles_per_sec);

    // --- Spot-check: read back a few tiles to confirm no overflow poison ---
    std::vector<uint32_t> prime_counts_host(tile_count, 0U);
    check_cuda(cudaMemcpy(prime_counts_host.data(), d_prime_counts,
                          sizeof(uint32_t) * tile_count,
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy(prime_counts)");

    int overflow_count = 0;
    // Check first output byte for overflow sentinel
    std::vector<TileOp> output_host(tile_count);
    check_cuda(cudaMemcpy(output_host.data(), d_output,
                          sizeof(TileOp) * tile_count,
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy(output)");

    for (int i = 0; i < tile_count; ++i) {
        if (output_host[i].bytes[0] == OVERFLOW_SENTINEL) {
            ++overflow_count;
        }
    }
    if (overflow_count > 0) {
        std::printf("WARNING: %d/%d tiles hit overflow sentinel\n", overflow_count, tile_count);
    } else {
        std::printf("all %d tiles completed without overflow\n", tile_count);
    }

#ifdef PROFILE_PHASES
    std::vector<PhaseTimingGPU> phase_timings(tile_count);
    check_cuda(cudaMemcpy(phase_timings.data(), d_phase_timings,
                          sizeof(PhaseTimingGPU) * phase_timings.size(),
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy(phase_timings)");
    print_phase_report(phase_timings);
#endif

    // --- Cleanup ---
    cudaEventDestroy(stop);
    cudaEventDestroy(start);
#ifdef PROFILE_PHASES
    cudaFree(d_phase_timings);
#endif
    cudaFree(d_prime_counts);
    cudaFree(d_output);
    cudaFree(d_coords);
}

}  // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    SieveTablesBarrett tables{};
    if (!init_sieve_tables_host(tables)) {
        std::fprintf(stderr, "failed to initialize sieve tables\n");
        return 1;
    }

    BackwardOffsetsHost offsets{};
    init_backward_offsets_host(offsets);

    upload_sieve_tables(tables);
    upload_backward_offsets(offsets.dr, offsets.dc, offsets.count);
    upload_constants();

    if (args.mode == Mode::TEST) {
        run_test();
    } else {
        run_bench(args.tile_count);
    }

    return 0;
}
