#include "process_tile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

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

}  // namespace

int main(int argc, char** argv) {
    const int tile_count = (argc > 1) ? std::atoi(argv[1]) : 500;
    if (tile_count <= 0) {
        std::fprintf(stderr, "error: tile count must be positive\n");
        return 2;
    }

    std::printf("=== tile-cpp benchmark ===\n");
    std::printf("tiles: %d | R: %.0f | W: %.0f | build: -O2\n\n",
                tile_count, R_CENTER, R_WIDTH);

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

    // Run benchmark
    std::vector<BenchSample> samples(tile_count);
    const auto bench_start = std::chrono::steady_clock::now();

    for (int i = 0; i < tile_count; ++i) {
        PhaseTimings pt{};
        process_tile(coords[i], tables, &pt);
        samples[i].phases   = pt;
        samples[i].total_us = static_cast<double>(pt.total_ns) / 1000.0;
    }

    const auto bench_end = std::chrono::steady_clock::now();
    const double bench_wall_ms = std::chrono::duration_cast<std::chrono::microseconds>(
        bench_end - bench_start).count() / 1000.0;

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

    std::printf("\nWall clock: %.1f ms for %d tiles (%.1f tiles/sec)\n",
                bench_wall_ms, tile_count,
                static_cast<double>(tile_count) / (bench_wall_ms / 1000.0));

    return 0;
}
