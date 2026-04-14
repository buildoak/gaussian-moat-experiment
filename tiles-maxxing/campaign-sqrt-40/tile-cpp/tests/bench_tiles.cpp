#include "process_tile.h"
#include "encode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include <dispatch/dispatch.h>

namespace {

constexpr double R_CENTER = 850'000'000.0;
constexpr double R_WIDTH  = 8192.0;
constexpr int    TILE_ALIGN = 256;

struct BenchSample {
    PhaseTimings phases;
    double       total_us;
};

// Generate a tile coordinate in the first octant annulus at R~850M
TileCoord random_tile(std::mt19937_64& rng) {
    std::uniform_real_distribution<double> angle_dist(0.0, M_PI / 4.0);
    std::uniform_real_distribution<double> radius_dist(R_CENTER, R_CENTER + R_WIDTH);

    const double theta = angle_dist(rng);
    const double r     = radius_dist(rng);
    const double a     = r * std::cos(theta);
    const double b     = r * std::sin(theta);

    int64_t a_lo = static_cast<int64_t>(std::floor(a / TILE_ALIGN)) * TILE_ALIGN;
    int64_t b_lo = static_cast<int64_t>(std::floor(b / TILE_ALIGN)) * TILE_ALIGN;

    // Ensure first octant: a_lo >= b_lo >= 0
    if (b_lo < 0) b_lo = 0;
    if (a_lo < b_lo) std::swap(a_lo, b_lo);

    return TileCoord{a_lo, b_lo};
}

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

double median(std::vector<double>& v) {
    return percentile(v, 50.0);
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += x;
    return sum / static_cast<double>(v.size());
}

void print_phase_stats(const std::vector<BenchSample>& samples, int tile_count) {
    // Extract per-phase vectors (in microseconds)
    std::vector<double> sieve_us(tile_count), compact_us(tile_count),
                        uf_us(tile_count), face_us(tile_count),
                        prune_us(tile_count), total_us(tile_count);

    for (int i = 0; i < tile_count; ++i) {
        const auto& p = samples[i].phases;
        sieve_us[i]   = static_cast<double>(p.sieve_ns) / 1000.0;
        compact_us[i] = static_cast<double>(p.compact_ns) / 1000.0;
        uf_us[i]      = static_cast<double>(p.union_find_ns) / 1000.0;
        face_us[i]    = static_cast<double>(p.face_extract_ns) / 1000.0;
        prune_us[i]   = static_cast<double>(p.prune_encode_ns) / 1000.0;
        total_us[i]   = samples[i].total_us;
    }

    // Compute stats
    struct PhaseStat {
        const char* name;
        double mean_us, median_us, p99_us;
        double pct;  // percentage of total
    };

    const double total_mean = mean(total_us);

    auto make_stat = [&](const char* name, std::vector<double>& v) -> PhaseStat {
        PhaseStat s;
        s.name      = name;
        s.mean_us   = mean(v);
        s.median_us = median(v);
        s.p99_us    = percentile(v, 99.0);
        s.pct       = (total_mean > 0.0) ? (s.mean_us / total_mean * 100.0) : 0.0;
        return s;
    };

    PhaseStat stats[] = {
        make_stat("Sieve",          sieve_us),
        make_stat("Compact",        compact_us),
        make_stat("Union-Find",     uf_us),
        make_stat("Face Extract",   face_us),
        make_stat("Prune+Encode",   prune_us),
    };

    // Print per-phase table
    std::printf("%-16s %10s %10s %10s %8s\n",
                "Phase", "Mean(us)", "Med(us)", "P99(us)", "Pct(%)");
    std::printf("%-16s %10s %10s %10s %8s\n",
                "----------------", "----------", "----------", "----------", "--------");

    for (const auto& s : stats) {
        std::printf("%-16s %10.1f %10.1f %10.1f %7.1f%%\n",
                    s.name, s.mean_us, s.median_us, s.p99_us, s.pct);
    }

    // Print total row
    std::printf("%-16s %10s %10s %10s %8s\n",
                "----------------", "----------", "----------", "----------", "--------");
    std::printf("%-16s %10.1f %10.1f %10.1f %7s\n",
                "TOTAL",
                mean(total_us),
                median(total_us),
                percentile(total_us, 99.0),
                "100.0%");

    // Print total tile stats
    std::printf("\n--- Total per-tile (us) ---\n");
    std::printf("  mean:   %10.1f\n", mean(total_us));
    std::printf("  median: %10.1f\n", median(total_us));
    std::printf("  p99:    %10.1f\n", percentile(total_us, 99.0));

    auto sorted_total = total_us;
    std::sort(sorted_total.begin(), sorted_total.end());
    std::printf("  min:    %10.1f\n", sorted_total.front());
    std::printf("  max:    %10.1f\n", sorted_total.back());
}

void print_tileop_stats(const std::vector<TileResult>& results) {
    int empty_count = 0;
    int overflow_count = 0;
    int live_count = 0;
    double payload_sum = 0.0;

    for (const TileResult& result : results) {
        const TileOpLayout layout = parse_tileop_v2(result.tileop);
        if (!layout.is_valid) {
            continue;
        }
        if (layout.is_overflow) {
            ++overflow_count;
            continue;
        }
        if (layout.is_empty) {
            ++empty_count;
        } else {
            ++live_count;
        }
        payload_sum += static_cast<double>(layout.payload_bytes_used);
    }

    const int normal_tiles = empty_count + live_count;
    const double denom = results.empty() ? 1.0 : static_cast<double>(results.size());
    const double avg_payload = normal_tiles > 0 ? payload_sum / static_cast<double>(normal_tiles) : 0.0;

    std::printf("\n--- TileOp Summary ---\n");
    std::printf("  empty tiles:    %10d (%.2f%%)\n", empty_count, 100.0 * empty_count / denom);
    std::printf("  overflow tiles: %10d (%.2f%%)\n", overflow_count, 100.0 * overflow_count / denom);
    std::printf("  avg payload:    %10.1f bytes\n", avg_payload);
}

}  // namespace

int main(int argc, char** argv) {
    int tile_count = 500;
    bool multithreaded = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mt") == 0) {
            multithreaded = true;
        } else {
            tile_count = std::atoi(argv[i]);
        }
    }

    if (tile_count <= 0) {
        std::fprintf(stderr, "error: tile count must be positive\n");
        return 2;
    }

    const unsigned hw_threads = std::thread::hardware_concurrency();

    std::printf("=== tile-cpp benchmark ===\n");
    std::printf("tiles: %d | R: %.0f | W: %.0f | mode: %s | hw_threads: %u\n\n",
                tile_count, R_CENTER, R_WIDTH,
                multithreaded ? "multithreaded (GCD)" : "single-threaded",
                hw_threads);

    // Init sieve tables
    SieveTables tables;
    if (!init_sieve_tables(tables)) {
        std::fprintf(stderr, "error: init_sieve_tables failed\n");
        return 1;
    }

    // Generate tile coordinates
    std::mt19937_64 rng(42);  // fixed seed for reproducibility
    std::vector<TileCoord> coords(tile_count);
    for (int i = 0; i < tile_count; ++i) {
        coords[i] = random_tile(rng);
    }

    // Pre-allocate results and timings
    std::vector<TileResult> results(tile_count);
    std::vector<BenchSample> samples(tile_count);

    if (multithreaded) {
        // --- Multithreaded path: GCD dispatch_apply ---

        // Pre-allocate per-tile timing storage
        std::vector<PhaseTimings> all_timings(tile_count);

        // Raw pointers for block captures (blocks capture by const copy;
        // pointers let us write through to the underlying vectors)
        PhaseTimings* timings_ptr = all_timings.data();
        TileResult*   results_ptr = results.data();
        const TileCoord* coords_ptr = coords.data();

        const auto wall_start = std::chrono::steady_clock::now();
        dispatch_apply(static_cast<size_t>(tile_count),
                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                       ^(size_t i) {
            timings_ptr[i] = {};
            results_ptr[i] = process_tile(coords_ptr[i], tables, &timings_ptr[i]);
        });
        const auto wall_end = std::chrono::steady_clock::now();

        const double wall_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            wall_end - wall_start).count() / 1000.0;

        // Fill samples from timing data
        for (int i = 0; i < tile_count; ++i) {
            samples[i].phases   = all_timings[i];
            samples[i].total_us = static_cast<double>(all_timings[i].total_ns) / 1000.0;
        }

        // Print per-phase stats (measures individual tile cost, not parallelism)
        print_phase_stats(samples, tile_count);
        print_tileop_stats(results);

        // Print wall clock / throughput
        const double tiles_per_sec = static_cast<double>(tile_count) / (wall_ms / 1000.0);
        std::printf("\n--- Multithreaded Summary ---\n");
        std::printf("  Wall clock:     %10.1f ms\n", wall_ms);
        std::printf("  Throughput:     %10.0f tiles/sec\n", tiles_per_sec);
        std::printf("  HW threads:     %10u\n", hw_threads);

        // Compute speedup vs single-threaded baseline
        // Use per-tile mean * tile_count as the ideal single-threaded wall time
        double sum_tile_us = 0.0;
        for (int i = 0; i < tile_count; ++i) {
            sum_tile_us += samples[i].total_us;
        }
        const double st_equivalent_ms = sum_tile_us / 1000.0;
        const double speedup = st_equivalent_ms / wall_ms;
        std::printf("  ST equivalent:  %10.1f ms (sum of per-tile times)\n", st_equivalent_ms);
        std::printf("  Speedup:        %10.2fx\n", speedup);

    } else {
        // --- Single-threaded path (original behavior) ---

        const auto bench_start = std::chrono::steady_clock::now();

        for (int i = 0; i < tile_count; ++i) {
            PhaseTimings pt{};
            results[i] = process_tile(coords[i], tables, &pt);
            samples[i].phases   = pt;
            samples[i].total_us = static_cast<double>(pt.total_ns) / 1000.0;
        }

        const auto bench_end = std::chrono::steady_clock::now();
        const double bench_wall_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            bench_end - bench_start).count() / 1000.0;

        // Print per-phase stats
        print_phase_stats(samples, tile_count);
        print_tileop_stats(results);

        std::printf("\nWall clock: %.1f ms for %d tiles (%.1f tiles/sec)\n",
                    bench_wall_ms, tile_count,
                    static_cast<double>(tile_count) / (bench_wall_ms / 1000.0));
    }

    return 0;
}
