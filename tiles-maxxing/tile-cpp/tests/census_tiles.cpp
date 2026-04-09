#include "process_tile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include <dispatch/dispatch.h>

namespace {

constexpr int TILES_PER_TOWER = 32;

struct TileCensusRow {
    int       tower_j;
    int       tile_k;
    int64_t   a_lo;
    int64_t   b_lo;
    uint32_t  prime_count;
    uint32_t  group_count;
    uint32_t  ports_before;
    uint32_t  ports_after;
    int       face_ports[NUM_FACES];  // I, O, L, R
    bool      overflow;
};

// Count non-zero bytes in a 16-byte face slot
int count_face_ports(const uint8_t* face_slot) {
    int count = 0;
    for (int i = 0; i < PORTS_PER_FACE; ++i) {
        if (face_slot[i] != 0) ++count;
    }
    return count;
}

// Decode per-face port counts from TileOp bytes
// Layout: bytes[0..15]=Face I, [16..31]=Face O, [32..47]=Face L, [48..63]=Face R
void decode_face_ports(const TileOp& op, int face_ports[NUM_FACES]) {
    face_ports[FACE_I] = count_face_ports(&op.bytes[0]);
    face_ports[FACE_O] = count_face_ports(&op.bytes[16]);
    face_ports[FACE_L] = count_face_ports(&op.bytes[32]);
    face_ports[FACE_R] = count_face_ports(&op.bytes[48]);
}

template<typename T>
double vec_mean(const std::vector<T>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& x : v) sum += static_cast<double>(x);
    return sum / static_cast<double>(v.size());
}

template<typename T>
double vec_percentile(std::vector<T> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return static_cast<double>(v[lo]) * (1.0 - frac) + static_cast<double>(v[hi]) * frac;
}

template<typename T>
double vec_median(std::vector<T>& v) {
    return vec_percentile(v, 50.0);
}

template<typename T>
T vec_min(const std::vector<T>& v) {
    return *std::min_element(v.begin(), v.end());
}

template<typename T>
T vec_max(const std::vector<T>& v) {
    return *std::max_element(v.begin(), v.end());
}

void print_stat_line(const char* label, std::vector<uint32_t>& v) {
    std::printf("  %-16s min=%-6u  mean=%-10.1f  median=%-8.1f  max=%-6u  p99=%-8.1f\n",
                label,
                vec_min(v),
                vec_mean(v),
                vec_median(v),
                vec_max(v),
                vec_percentile(v, 99.0));
}

void print_face_stat_line(const char* label, std::vector<int>& v) {
    std::printf("    %-14s min=%-4d  mean=%-8.1f  max=%-4d  p99=%-6.1f\n",
                label,
                vec_min(v),
                vec_mean(v),
                vec_max(v),
                vec_percentile(v, 99.0));
}

void mkdirs(const char* path) {
    // Simple recursive mkdir
    char tmp[4096];
    std::strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

}  // namespace

int main(int argc, char** argv) {
    int64_t R = 860'000'000LL;
    int num_towers = 3125;
    bool multithreaded = false;

    // Parse CLI args
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mt") == 0) {
            multithreaded = true;
        } else {
            if (positional == 0) {
                R = std::atoll(argv[i]);
            } else if (positional == 1) {
                num_towers = std::atoi(argv[i]);
            }
            ++positional;
        }
    }

    const int total_tiles = num_towers * TILES_PER_TOWER;
    const unsigned hw_threads = std::thread::hardware_concurrency();

    std::printf("=== Tile Census ===\n");
    std::printf("R: %lld, Towers: %d, Tiles: %d, Mode: %s, HW threads: %u\n\n",
                static_cast<long long>(R), num_towers, total_tiles,
                multithreaded ? "multithreaded (GCD)" : "single-threaded",
                hw_threads);

    // Init sieve tables
    SieveTables tables;
    if (!init_sieve_tables(tables)) {
        std::fprintf(stderr, "error: init_sieve_tables failed\n");
        return 1;
    }

    // Generate tile coordinates
    const double R_d = static_cast<double>(R);
    std::vector<TileCoord> coords(total_tiles);
    std::vector<int> tower_indices(total_tiles);
    std::vector<int> tile_indices(total_tiles);

    int idx = 0;
    for (int j = 0; j < num_towers; ++j) {
        const int64_t a_lo = static_cast<int64_t>(j) * TILE_SIDE;
        const double a_lo_d = static_cast<double>(a_lo);

        // b_lo_base = floor(sqrt(R^2 - a_lo^2)) rounded down to multiple of TILE_SIDE
        const double inner = R_d * R_d - a_lo_d * a_lo_d;
        int64_t b_raw;
        if (inner <= 0.0) {
            b_raw = 0;
        } else {
            b_raw = static_cast<int64_t>(std::sqrt(inner));
        }
        const int64_t b_lo_base = (b_raw / TILE_SIDE) * TILE_SIDE;

        // Stop condition check: past 45-degree line
        if (a_lo >= b_lo_base && j > 0) {
            std::printf("Note: tower %d has a_lo=%lld >= b_lo_base=%lld (past 45 deg). "
                        "Truncating to %d towers.\n",
                        j, static_cast<long long>(a_lo),
                        static_cast<long long>(b_lo_base), j);
            // Resize to actual count
            const int actual_tiles = j * TILES_PER_TOWER;
            coords.resize(actual_tiles);
            tower_indices.resize(actual_tiles);
            tile_indices.resize(actual_tiles);
            num_towers = j;
            break;
        }

        for (int k = 0; k < TILES_PER_TOWER; ++k) {
            coords[idx] = TileCoord{a_lo, b_lo_base + static_cast<int64_t>(k) * TILE_SIDE};
            tower_indices[idx] = j;
            tile_indices[idx] = k;
            ++idx;
        }
    }

    const int actual_total = static_cast<int>(coords.size());
    std::printf("Generated %d tile coordinates across %d towers.\n\n", actual_total, num_towers);

    // Process tiles
    std::vector<TileResult> results(actual_total);

    const auto wall_start = std::chrono::steady_clock::now();

    if (multithreaded) {
        TileResult*      results_ptr = results.data();
        const TileCoord* coords_ptr  = coords.data();

        dispatch_apply(static_cast<size_t>(actual_total),
                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                       ^(size_t i) {
            results_ptr[i] = process_tile(coords_ptr[i], tables, nullptr);
        });
    } else {
        for (int i = 0; i < actual_total; ++i) {
            results[i] = process_tile(coords[i], tables, nullptr);
        }
    }

    const auto wall_end = std::chrono::steady_clock::now();
    const double wall_sec = std::chrono::duration_cast<std::chrono::microseconds>(
        wall_end - wall_start).count() / 1.0e6;

    // Build census rows and decode face ports
    std::vector<TileCensusRow> rows(actual_total);
    for (int i = 0; i < actual_total; ++i) {
        auto& row = rows[i];
        row.tower_j      = tower_indices[i];
        row.tile_k        = tile_indices[i];
        row.a_lo          = coords[i].a_lo;
        row.b_lo          = coords[i].b_lo;
        row.prime_count   = results[i].prime_count;
        row.group_count   = results[i].group_count;
        row.ports_before  = results[i].ports_before_pruning;
        row.ports_after   = results[i].ports_after_pruning;
        row.overflow      = (results[i].tileop.bytes[0] == OVERFLOW_SENTINEL);

        if (row.overflow) {
            // Face decode not meaningful for poisoned tiles
            row.face_ports[0] = row.face_ports[1] = row.face_ports[2] = row.face_ports[3] = -1;
        } else {
            decode_face_ports(results[i].tileop, row.face_ports);
        }
    }

    // Write CSV
    char csv_path[4096];
    std::snprintf(csv_path, sizeof(csv_path),
                  "/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/census_output/census_R%lld_T%d.csv",
                  static_cast<long long>(R), num_towers);

    // Ensure output directory exists
    mkdirs("/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/census_output");

    FILE* csv = std::fopen(csv_path, "w");
    if (!csv) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", csv_path);
        return 1;
    }

    std::fprintf(csv, "tower_j,tile_k,a_lo,b_lo,prime_count,group_count,ports_before,ports_after,face_I_ports,face_O_ports,face_L_ports,face_R_ports,overflow\n");
    for (int i = 0; i < actual_total; ++i) {
        const auto& r = rows[i];
        std::fprintf(csv, "%d,%d,%lld,%lld,%u,%u,%u,%u,%d,%d,%d,%d,%d\n",
                     r.tower_j, r.tile_k,
                     static_cast<long long>(r.a_lo),
                     static_cast<long long>(r.b_lo),
                     r.prime_count, r.group_count,
                     r.ports_before, r.ports_after,
                     r.face_ports[FACE_I], r.face_ports[FACE_O],
                     r.face_ports[FACE_L], r.face_ports[FACE_R],
                     r.overflow ? 1 : 0);
    }
    std::fclose(csv);
    std::printf("CSV written to: %s\n\n", csv_path);

    // Compute summary statistics
    std::vector<uint32_t> prime_counts, group_counts, ports_befores, ports_afters;
    std::vector<int> face_I, face_O, face_L, face_R;
    int overflow_count = 0;

    for (int i = 0; i < actual_total; ++i) {
        const auto& r = rows[i];
        prime_counts.push_back(r.prime_count);
        group_counts.push_back(r.group_count);
        ports_befores.push_back(r.ports_before);
        ports_afters.push_back(r.ports_after);

        if (r.overflow) {
            ++overflow_count;
        } else {
            face_I.push_back(r.face_ports[FACE_I]);
            face_O.push_back(r.face_ports[FACE_O]);
            face_L.push_back(r.face_ports[FACE_L]);
            face_R.push_back(r.face_ports[FACE_R]);
        }
    }

    // Print summary
    const double throughput = static_cast<double>(actual_total) / wall_sec;

    std::printf("=== Census Summary ===\n");
    std::printf("R: %lld, Towers: %d, Tiles: %d\n",
                static_cast<long long>(R), num_towers, actual_total);
    std::printf("Wall clock: %.1fs\n", wall_sec);
    std::printf("Throughput: %.0f tiles/sec\n\n", throughput);

    print_stat_line("Prime count:", prime_counts);
    print_stat_line("Group count:", group_counts);
    print_stat_line("Ports before:", ports_befores);
    print_stat_line("Ports after:", ports_afters);

    std::printf("\nPer-face ports after pruning (non-overflow tiles only, n=%d):\n",
                static_cast<int>(face_I.size()));
    print_face_stat_line("Face I:", face_I);
    print_face_stat_line("Face O:", face_O);
    print_face_stat_line("Face L:", face_L);
    print_face_stat_line("Face R:", face_R);

    std::printf("\nOverflow tiles: %d / %d (%.2f%%)\n\n",
                overflow_count, actual_total,
                100.0 * static_cast<double>(overflow_count) / static_cast<double>(actual_total));

    // Histogram: group count distribution
    {
        auto sorted = group_counts;
        std::sort(sorted.begin(), sorted.end());
        uint32_t max_gc = sorted.back();

        std::printf("Histogram -- group count distribution:\n");
        std::vector<int> hist(max_gc + 1, 0);
        for (auto gc : group_counts) hist[gc]++;
        for (uint32_t g = 0; g <= max_gc; ++g) {
            if (hist[g] > 0) {
                std::printf("  %3u: %d\n", g, hist[g]);
            }
        }
    }

    // Histogram: max face ports (across 4 faces) per non-overflow tile
    {
        std::printf("\nHistogram -- max face ports (across 4 faces, non-overflow only):\n");
        std::vector<int> max_face_ports;
        for (int i = 0; i < actual_total; ++i) {
            if (rows[i].overflow) continue;
            int mx = 0;
            for (int f = 0; f < NUM_FACES; ++f) {
                mx = std::max(mx, rows[i].face_ports[f]);
            }
            max_face_ports.push_back(mx);
        }

        if (!max_face_ports.empty()) {
            int hist_max = *std::max_element(max_face_ports.begin(), max_face_ports.end());
            hist_max = std::max(hist_max, PORTS_PER_FACE);  // always show up to 16
            std::vector<int> hist(hist_max + 1, 0);
            for (int v : max_face_ports) hist[v]++;
            for (int g = 0; g <= hist_max; ++g) {
                if (g == PORTS_PER_FACE) {
                    std::printf("  %3d: %d  <-- would overflow\n", g, hist[g]);
                } else {
                    std::printf("  %3d: %d\n", g, hist[g]);
                }
            }
        }
    }

    return 0;
}
