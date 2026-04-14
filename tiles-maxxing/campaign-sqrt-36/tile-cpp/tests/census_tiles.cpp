#include "compact.h"
#include "encode.h"
#include "face_extract.h"
#include "prune.h"
#include "sieve.h"
#include "union_find.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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
    int       off_I;
    int       off_L;
    int       off_R;
    int       payload_bytes_used;
    int       payload_slack;
    int       multi_face_groups;
    int       max_group_ports;
    int       has_all_4_faces;
    int       h1_256_lr_ports;
    int       lr_ports;
    int       overflow_reason_group_cap;
    int       overflow_reason_budget;
    int       overflow_reason_h1;
    const char* status;
};

struct TileCensusMetrics {
    TileResult result;
    FaceData   pruned_face_data;
    bool       encode_group_cap_overflow;
    bool       encode_budget_overflow;
    bool       encode_h1_failure;
    int        face_ports[NUM_FACES];
    int        payload_bytes_used;
    int        payload_slack;
    int        multi_face_groups;
    int        max_group_ports;
    int        has_all_4_faces;
    int        h1_256_lr_ports;
    int        lr_ports;
};

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
    std::printf("  %-16s min=%-6u  mean=%-10.1f  median=%-8.1f  p95=%-8.1f  p99=%-8.1f  max=%-6u\n",
                label,
                vec_min(v),
                vec_mean(v),
                vec_median(v),
                vec_percentile(v, 95.0),
                vec_percentile(v, 99.0),
                vec_max(v));
}

void print_face_stat_line(const char* label, std::vector<int>& v) {
    std::printf("    %-14s min=%-4d  mean=%-8.1f  median=%-6.1f  p95=%-6.1f  p99=%-6.1f  max=%-4d\n",
                label,
                vec_min(v),
                vec_mean(v),
                vec_median(v),
                vec_percentile(v, 95.0),
                vec_percentile(v, 99.0),
                vec_max(v));
}

int payload_bytes_from_face_counts(const int face_ports[NUM_FACES]) {
    return face_ports[FACE_O] + face_ports[FACE_I] + 2 * face_ports[FACE_L] + 2 * face_ports[FACE_R];
}

TileCensusMetrics analyze_tile(const TileCoord& coord, const SieveTables& tables) {
    TileCensusMetrics metrics{};
    std::memset(&metrics, 0, sizeof(metrics));
    metrics.result.tileop = make_empty_tileop();
    metrics.payload_slack = TILEOP_PAYLOAD_BYTES;

    uint32_t bitmap[BITMAP_WORDS];
    uint32_t prefix[BITMAP_WORDS];
    uint32_t prime_pos[MAX_PRIMES];
    uint16_t parent[MAX_PRIMES];

    std::memset(bitmap, 0, sizeof(bitmap));
    sieve_tile(coord, tables, bitmap);

    const int bitmap_prime_count = compact_primes(bitmap, prefix, prime_pos);
    metrics.result.prime_count = static_cast<uint32_t>(bitmap_prime_count);  // match CUDA: all bitmap primes incl. collar

    if (bitmap_prime_count == 0) {
        return metrics;
    }

    build_components(bitmap, prefix, prime_pos, bitmap_prime_count, parent);
    const FaceData face_data = extract_faces(coord, bitmap, prefix, prime_pos, bitmap_prime_count, parent);
    metrics.result.ports_before_pruning = static_cast<uint32_t>(face_data.port_count);

    metrics.pruned_face_data = prune_dead_ends(face_data);
    metrics.result.ports_after_pruning = static_cast<uint32_t>(metrics.pruned_face_data.port_count);
    metrics.result.group_count = static_cast<uint32_t>(metrics.pruned_face_data.group_count);

    std::array<uint8_t, MAX_PORTS + 1> group_faces{};
    std::array<int, MAX_PORTS + 1> group_ports{};

    for (int i = 0; i < metrics.pruned_face_data.port_count; ++i) {
        const Port& port = metrics.pruned_face_data.ports[i];
        if (port.face >= 0 && port.face < NUM_FACES) {
            ++metrics.face_ports[port.face];
            if ((port.face == FACE_L || port.face == FACE_R) && port.h1 == TILE_SIDE) {
                ++metrics.h1_256_lr_ports;
            }
            if (port.face == FACE_L || port.face == FACE_R) {
                ++metrics.lr_ports;
            }
        }
        if (port.h1 > TILE_SIDE) {
            metrics.encode_h1_failure = true;
        }
        if (port.group > 0 && port.group <= MAX_PORTS) {
            group_faces[port.group] = static_cast<uint8_t>(group_faces[port.group] | (1U << port.face));
            ++group_ports[port.group];
        }
    }

    metrics.has_all_4_faces = 1;
    for (int face = 0; face < NUM_FACES; ++face) {
        if (metrics.face_ports[face] == 0) {
            metrics.has_all_4_faces = 0;
        }
    }

    for (int group = 1; group <= metrics.pruned_face_data.group_count && group <= MAX_PORTS; ++group) {
        if (group_ports[group] == 0) {
            continue;
        }
        metrics.max_group_ports = std::max(metrics.max_group_ports, group_ports[group]);

        int face_count = 0;
        uint8_t mask = group_faces[group];
        while (mask != 0) {
            face_count += static_cast<int>(mask & 1U);
            mask = static_cast<uint8_t>(mask >> 1);
        }
        if (face_count >= 2) {
            ++metrics.multi_face_groups;
        }
    }

    metrics.payload_bytes_used = payload_bytes_from_face_counts(metrics.face_ports);
    metrics.payload_slack = TILEOP_PAYLOAD_BYTES - metrics.payload_bytes_used;
    metrics.encode_group_cap_overflow = metrics.pruned_face_data.group_count > 127;
    metrics.encode_budget_overflow = metrics.payload_bytes_used > TILEOP_PAYLOAD_BYTES;

    metrics.result.tileop = encode_tileop(metrics.pruned_face_data);
    return metrics;
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
    int start_tower = 0;
    bool multithreaded = false;

    // Parse CLI args: R [num_towers] [start_tower] [--mt]
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mt") == 0) {
            multithreaded = true;
        } else {
            if (positional == 0) {
                R = std::atoll(argv[i]);
            } else if (positional == 1) {
                num_towers = std::atoi(argv[i]);
            } else if (positional == 2) {
                start_tower = std::atoi(argv[i]);
            }
            ++positional;
        }
    }

    const int total_tiles = num_towers * TILES_PER_TOWER;
    const unsigned hw_threads = std::thread::hardware_concurrency();

    std::printf("=== Tile Census ===\n");
    std::printf("R: %lld, Towers: %d (start=%d), Tiles: %d, Mode: %s, HW threads: %u\n\n",
                static_cast<long long>(R), num_towers, start_tower, total_tiles,
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
    for (int j = start_tower; j < start_tower + num_towers; ++j) {
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
            const int actual_tiles = (j - start_tower) * TILES_PER_TOWER;
            coords.resize(actual_tiles);
            tower_indices.resize(actual_tiles);
            tile_indices.resize(actual_tiles);
            num_towers = j - start_tower;
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
    std::vector<TileCensusMetrics> metrics(actual_total);

    const auto wall_start = std::chrono::steady_clock::now();

    if (multithreaded) {
        TileCensusMetrics* metrics_ptr = metrics.data();
        const TileCoord* coords_ptr  = coords.data();

        dispatch_apply(static_cast<size_t>(actual_total),
                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                       ^(size_t i) {
            metrics_ptr[i] = analyze_tile(coords_ptr[i], tables);
        });
    } else {
        for (int i = 0; i < actual_total; ++i) {
            metrics[i] = analyze_tile(coords[i], tables);
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
        row.prime_count   = metrics[i].result.prime_count;
        row.group_count   = metrics[i].result.group_count;
        row.ports_before  = metrics[i].result.ports_before_pruning;
        row.ports_after   = metrics[i].result.ports_after_pruning;
        row.multi_face_groups = metrics[i].multi_face_groups;
        row.max_group_ports = metrics[i].max_group_ports;
        row.has_all_4_faces = metrics[i].has_all_4_faces;
        row.h1_256_lr_ports = metrics[i].h1_256_lr_ports;
        row.lr_ports = metrics[i].lr_ports;
        row.overflow_reason_group_cap = metrics[i].encode_group_cap_overflow ? 1 : 0;
        row.overflow_reason_budget = metrics[i].encode_budget_overflow ? 1 : 0;
        row.overflow_reason_h1 = metrics[i].encode_h1_failure ? 1 : 0;
        const TileOpLayout layout = parse_tileop_v2(metrics[i].result.tileop);
        if (!layout.is_valid) {
            row.face_ports[0] = row.face_ports[1] = row.face_ports[2] = row.face_ports[3] = -1;
            row.off_I = row.off_L = row.off_R = -1;
            row.payload_bytes_used = -1;
            row.payload_slack = -1;
            row.status = "invalid";
        } else if (layout.is_overflow) {
            for (int face = 0; face < NUM_FACES; ++face) {
                row.face_ports[face] = metrics[i].face_ports[face];
            }
            row.off_I = row.off_L = row.off_R = -1;
            row.payload_bytes_used = metrics[i].payload_bytes_used;
            row.payload_slack = metrics[i].payload_slack;
            row.status = "overflow";
        } else {
            row.face_ports[FACE_I] = metrics[i].face_ports[FACE_I];
            row.face_ports[FACE_O] = metrics[i].face_ports[FACE_O];
            row.face_ports[FACE_L] = metrics[i].face_ports[FACE_L];
            row.face_ports[FACE_R] = metrics[i].face_ports[FACE_R];
            row.off_I = layout.off_I;
            row.off_L = layout.off_L;
            row.off_R = layout.off_R;
            row.payload_bytes_used = metrics[i].payload_bytes_used;
            row.payload_slack = metrics[i].payload_slack;
            row.status = layout.is_empty ? "empty" : "normal";
        }
    }

    // Write CSV
    char csv_path[4096];
    std::snprintf(csv_path, sizeof(csv_path),
                  "/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/census_output/census_R%lld_T%d_S%d.csv",
                  static_cast<long long>(R), num_towers, start_tower);

    // Ensure output directory exists
    mkdirs("/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-cpp/census_output");

    FILE* csv = std::fopen(csv_path, "w");
    if (!csv) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", csv_path);
        return 1;
    }

    std::fprintf(csv, "tower_j,tile_k,a_lo,b_lo,prime_count,group_count,ports_before,ports_after,off_I,off_L,off_R,face_I_ports,face_O_ports,face_L_ports,face_R_ports,payload_bytes_used,payload_slack,multi_face_groups,max_group_ports,has_all_4_faces,h1_256_lr_ports,lr_ports,overflow_reason_group_cap,overflow_reason_budget,overflow_reason_h1,status\n");
    for (int i = 0; i < actual_total; ++i) {
        const auto& r = rows[i];
        std::fprintf(csv, "%d,%d,%lld,%lld,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s\n",
                     r.tower_j, r.tile_k,
                     static_cast<long long>(r.a_lo),
                     static_cast<long long>(r.b_lo),
                     r.prime_count, r.group_count,
                     r.ports_before, r.ports_after,
                     r.off_I, r.off_L, r.off_R,
                     r.face_ports[FACE_I], r.face_ports[FACE_O],
                     r.face_ports[FACE_L], r.face_ports[FACE_R],
                     r.payload_bytes_used, r.payload_slack,
                     r.multi_face_groups, r.max_group_ports,
                     r.has_all_4_faces, r.h1_256_lr_ports, r.lr_ports,
                     r.overflow_reason_group_cap, r.overflow_reason_budget, r.overflow_reason_h1,
                     r.status);
    }
    std::fclose(csv);
    std::printf("CSV written to: %s\n\n", csv_path);

    // Compute summary statistics
    std::vector<uint32_t> prime_counts, group_counts, ports_befores, ports_afters;
    std::vector<int> face_I, face_O, face_L, face_R;
    std::vector<uint32_t> payload_used, payload_slacks, multi_face_groups, max_group_ports;
    std::vector<uint32_t> total_ports;
    int overflow_count = 0;
    int empty_count = 0;
    int invalid_count = 0;
    int all_4_faces_tiles = 0;
    int zero_ports_tiles = 0;
    int overflow_budget_count = 0;
    int overflow_group_cap_count = 0;
    int overflow_h1_count = 0;
    int tiles_with_h1_256 = 0;
    int64_t total_h1_256_ports = 0;
    int64_t total_lr_ports = 0;
    int max_payload_overflow = 0;
    std::vector<int> overflow_indices;

    for (int i = 0; i < actual_total; ++i) {
        const auto& r = rows[i];
        prime_counts.push_back(r.prime_count);
        group_counts.push_back(r.group_count);
        ports_befores.push_back(r.ports_before);
        ports_afters.push_back(r.ports_after);
        total_ports.push_back(static_cast<uint32_t>(r.ports_after));
        multi_face_groups.push_back(static_cast<uint32_t>(r.multi_face_groups));
        max_group_ports.push_back(static_cast<uint32_t>(r.max_group_ports));
        if (r.ports_after == 0) {
            ++zero_ports_tiles;
        }
        if (r.has_all_4_faces != 0) {
            ++all_4_faces_tiles;
        }
        if (r.h1_256_lr_ports > 0) {
            ++tiles_with_h1_256;
        }
        total_h1_256_ports += r.h1_256_lr_ports;
        total_lr_ports += r.lr_ports;

        if (std::strcmp(r.status, "overflow") == 0) {
            ++overflow_count;
            overflow_group_cap_count += r.overflow_reason_group_cap;
            overflow_budget_count += r.overflow_reason_budget;
            overflow_h1_count += r.overflow_reason_h1;
            max_payload_overflow = std::max(max_payload_overflow, r.payload_bytes_used);
            overflow_indices.push_back(i);
        } else if (std::strcmp(r.status, "empty") == 0) {
            ++empty_count;
            payload_used.push_back(static_cast<uint32_t>(r.payload_bytes_used));
            payload_slacks.push_back(static_cast<uint32_t>(r.payload_slack));
            face_I.push_back(r.face_ports[FACE_I]);
            face_O.push_back(r.face_ports[FACE_O]);
            face_L.push_back(r.face_ports[FACE_L]);
            face_R.push_back(r.face_ports[FACE_R]);
        } else if (std::strcmp(r.status, "invalid") == 0) {
            ++invalid_count;
        } else {
            payload_used.push_back(static_cast<uint32_t>(r.payload_bytes_used));
            payload_slacks.push_back(static_cast<uint32_t>(r.payload_slack));
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
    print_stat_line("Payload used:", payload_used);
    print_stat_line("Payload slack:", payload_slacks);
    print_stat_line("Multi-face grp:", multi_face_groups);
    print_stat_line("Max grp ports:", max_group_ports);

    std::printf("\nPer-face ports after pruning (non-overflow tiles only, n=%d):\n",
                static_cast<int>(face_I.size()));
    print_face_stat_line("Face I:", face_I);
    print_face_stat_line("Face O:", face_O);
    print_face_stat_line("Face L:", face_L);
    print_face_stat_line("Face R:", face_R);

    std::printf("\nStatus counts: normal=%d empty=%d overflow=%d invalid=%d\n",
                actual_total - empty_count - overflow_count - invalid_count,
                empty_count,
                overflow_count,
                invalid_count);
    std::printf("Overflow tiles: %d / %d (%.2f%%)\n\n",
                overflow_count, actual_total,
                100.0 * static_cast<double>(overflow_count) / static_cast<double>(actual_total));
    std::printf("Overflow causes: budget=%d group_cap=%d h1_failure=%d\n",
                overflow_budget_count, overflow_group_cap_count, overflow_h1_count);
    std::printf("Zero-port tiles: %d\n", zero_ports_tiles);
    std::printf("Tiles with all 4 faces populated: %d\n", all_4_faces_tiles);
    std::printf("Tiles with >=1 h1=256 L/R port: %d\n", tiles_with_h1_256);
    std::printf("Total h1=256 L/R ports: %lld / %lld (%.4f%% of L/R ports)\n\n",
                static_cast<long long>(total_h1_256_ports),
                static_cast<long long>(total_lr_ports),
                total_lr_ports == 0 ? 0.0
                                    : 100.0 * static_cast<double>(total_h1_256_ports) / static_cast<double>(total_lr_ports));

    if (!overflow_indices.empty()) {
        std::printf("Overflow tiles (first 20):\n");
        const int limit = std::min<int>(20, overflow_indices.size());
        for (int n = 0; n < limit; ++n) {
            const auto& r = rows[overflow_indices[n]];
            std::printf("  (%lld,%lld) ports=%u faces=[I:%d O:%d L:%d R:%d] groups=%u payload=%d slack=%d causes=%s%s%s\n",
                        static_cast<long long>(r.a_lo),
                        static_cast<long long>(r.b_lo),
                        r.ports_after,
                        r.face_ports[FACE_I], r.face_ports[FACE_O],
                        r.face_ports[FACE_L], r.face_ports[FACE_R],
                        r.group_count,
                        r.payload_bytes_used,
                        r.payload_slack,
                        r.overflow_reason_budget ? "budget " : "",
                        r.overflow_reason_group_cap ? "group_cap " : "",
                        r.overflow_reason_h1 ? "h1 " : "");
        }
        std::printf("\n");
    }

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
            if (std::strcmp(rows[i].status, "overflow") == 0 || std::strcmp(rows[i].status, "invalid") == 0) continue;
            int mx = 0;
            for (int f = 0; f < NUM_FACES; ++f) {
                mx = std::max(mx, rows[i].face_ports[f]);
            }
            max_face_ports.push_back(mx);
        }

        if (!max_face_ports.empty()) {
            int hist_max = *std::max_element(max_face_ports.begin(), max_face_ports.end());
            std::vector<int> hist(hist_max + 1, 0);
            for (int v : max_face_ports) hist[v]++;
            for (int g = 0; g <= hist_max; ++g) {
                std::printf("  %3d: %d\n", g, hist[g]);
            }
        }
    }

    return 0;
}
