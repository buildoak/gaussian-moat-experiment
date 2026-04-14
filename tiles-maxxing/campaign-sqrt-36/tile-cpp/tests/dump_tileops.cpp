// dump_tileops: Generate test tile coordinates across diverse zones, process
// through the C++ pipeline, and write two binary files:
//   1. coords.bin  — CUDA-compatible input (uint32 header + [a_lo, b_lo] per tile)
//   2. cpp_tileops.bin — C++ reference output (uint32 header + [a_lo, b_lo, prime_count, tileop[256]] per tile)
//
// Usage: dump_tileops <R> <output_dir> [--towers N]
//   Default: R=860000000, towers=32 (=1024 tiles), output_dir required.
//
// Zone strategy (4 zones, each gets towers/4 towers):
//   inner:   towers 0..N/4         (near real axis)
//   mid:     towers 50000..50000+N/4 (former overflow hot zone)
//   outer:   towers 150000..150000+N/4
//   diagonal: towers near 45-degree line

#include "process_tile.h"
#include "sieve.h"
#include "union_find.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr int TILES_PER_TOWER = 32;
constexpr int DEFAULT_TOWERS = 32;

struct Zone {
    const char* name;
    int start_tower;
};

// Compute the base b_lo for a tower at a_lo under radius R.
int64_t tower_b_lo_base(int64_t a_lo, int64_t R) {
    const double R_d = static_cast<double>(R);
    const double a_d = static_cast<double>(a_lo);
    const double inner = R_d * R_d - a_d * a_d;
    if (inner <= 0.0) return -1;
    const int64_t b_raw = static_cast<int64_t>(std::sqrt(inner));
    return (b_raw / TILE_SIDE) * TILE_SIDE;
}

// Find the tower index closest to the 45-degree line (a_lo ~ b_lo).
int find_diagonal_tower(int64_t R) {
    // At 45 degrees, a = b = R / sqrt(2)
    const double target = static_cast<double>(R) / std::sqrt(2.0);
    return static_cast<int>(target / static_cast<double>(TILE_SIDE));
}

void mkdirs(const char* path) {
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
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <R> <output_dir> [--towers N]\n", argv[0]);
        std::fprintf(stderr, "  Generates coords.bin + cpp_tileops.bin in output_dir.\n");
        std::fprintf(stderr, "  Default: 32 towers (1024 tiles across 4 zones).\n");
        return 2;
    }

    const int64_t R = std::atoll(argv[1]);
    const char* output_dir = argv[2];
    int total_towers = DEFAULT_TOWERS;

    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--towers") == 0 && i + 1 < argc) {
            total_towers = std::atoi(argv[++i]);
        }
    }

    if (R <= 0 || total_towers < 4) {
        std::fprintf(stderr, "error: R must be positive and towers >= 4\n");
        return 2;
    }

    const int towers_per_zone = total_towers / 4;
    const int diagonal_tower = find_diagonal_tower(R);

    // Define zones
    Zone zones[4] = {
        {"inner",    0},
        {"mid",      50000},
        {"outer",    150000},
        {"diagonal", diagonal_tower - towers_per_zone / 2},
    };

    // Clamp diagonal start to non-negative
    if (zones[3].start_tower < 0) zones[3].start_tower = 0;

    std::printf("=== dump_tileops ===\n");
    std::printf("R: %lld, towers_per_zone: %d, total_towers: %d\n",
                static_cast<long long>(R), towers_per_zone, towers_per_zone * 4);

    // Generate coordinates across all zones
    struct TileEntry {
        int64_t a_lo;
        int64_t b_lo;
        int zone_idx;
        int tower_j;
        int tile_k;
    };

    std::vector<TileEntry> entries;
    entries.reserve(towers_per_zone * 4 * TILES_PER_TOWER);

    for (int z = 0; z < 4; ++z) {
        int generated = 0;
        for (int j = zones[z].start_tower; generated < towers_per_zone; ++j) {
            const int64_t a_lo = static_cast<int64_t>(j) * TILE_SIDE;
            const int64_t b_base = tower_b_lo_base(a_lo, R);
            if (b_base < 0) {
                // Past circle boundary, stop this zone
                std::printf("  zone '%s': stopped at tower %d (past circle boundary), got %d towers\n",
                            zones[z].name, j, generated);
                break;
            }

            // Check 45-degree boundary: skip if a_lo >= b_base (except for diagonal zone)
            if (z != 3 && a_lo >= b_base && j > 0) {
                std::printf("  zone '%s': stopped at tower %d (past 45-degree line), got %d towers\n",
                            zones[z].name, j, generated);
                break;
            }

            for (int k = 0; k < TILES_PER_TOWER; ++k) {
                TileEntry entry;
                entry.a_lo = a_lo;
                entry.b_lo = b_base + static_cast<int64_t>(k) * TILE_SIDE;
                entry.zone_idx = z;
                entry.tower_j = j;
                entry.tile_k = k;
                entries.push_back(entry);
            }
            ++generated;
        }
        std::printf("  zone '%s': start_tower=%d, towers=%d, tiles=%d\n",
                    zones[z].name, zones[z].start_tower, generated, generated * TILES_PER_TOWER);
    }

    const uint32_t num_tiles = static_cast<uint32_t>(entries.size());
    std::printf("\nTotal tiles: %u\n", num_tiles);

    if (num_tiles == 0) {
        std::fprintf(stderr, "error: no tiles generated\n");
        return 1;
    }

    // Init sieve tables
    SieveTables tables;
    if (!init_sieve_tables(tables)) {
        std::fprintf(stderr, "error: init_sieve_tables failed\n");
        return 1;
    }

    BackwardOffsets offsets;
    init_backward_offsets(offsets);

    // Process all tiles through C++ pipeline
    std::printf("Processing %u tiles through C++ pipeline...\n", num_tiles);
    const auto wall_start = std::chrono::steady_clock::now();

    struct TileOutput {
        int64_t a_lo;
        int64_t b_lo;
        uint32_t prime_count;
        TileOp tileop;
    };

    std::vector<TileOutput> outputs(num_tiles);

    for (uint32_t i = 0; i < num_tiles; ++i) {
        TileCoord coord{entries[i].a_lo, entries[i].b_lo};
        TileResult result = process_tile(coord, tables);
        outputs[i].a_lo = coord.a_lo;
        outputs[i].b_lo = coord.b_lo;
        outputs[i].prime_count = result.prime_count;
        outputs[i].tileop = result.tileop;
    }

    const auto wall_end = std::chrono::steady_clock::now();
    const double wall_sec = std::chrono::duration_cast<std::chrono::microseconds>(
        wall_end - wall_start).count() / 1.0e6;
    std::printf("Done in %.2fs (%.0f tiles/sec)\n\n", wall_sec,
                static_cast<double>(num_tiles) / wall_sec);

    // Write output files
    mkdirs(output_dir);

    // 1. coords.bin — CUDA input format
    //    [uint32_t num_tiles] [int64_t a_lo, int64_t b_lo] * num_tiles
    char coords_path[4096];
    std::snprintf(coords_path, sizeof(coords_path), "%s/coords.bin", output_dir);

    FILE* fcoords = std::fopen(coords_path, "wb");
    if (!fcoords) {
        std::fprintf(stderr, "error: cannot open %s\n", coords_path);
        return 1;
    }
    std::fwrite(&num_tiles, sizeof(uint32_t), 1, fcoords);
    for (uint32_t i = 0; i < num_tiles; ++i) {
        std::fwrite(&outputs[i].a_lo, sizeof(int64_t), 1, fcoords);
        std::fwrite(&outputs[i].b_lo, sizeof(int64_t), 1, fcoords);
    }
    std::fclose(fcoords);
    std::printf("Wrote CUDA coords:   %s (%u tiles, %zu bytes)\n",
                coords_path, num_tiles,
                static_cast<size_t>(sizeof(uint32_t) + num_tiles * 16));

    // 2. cpp_tileops.bin — C++ reference output
    //    Same record layout as CUDA dump output:
    //    [uint32_t num_tiles] [int64_t a_lo, int64_t b_lo, uint32_t prime_count, uint8_t tileop[256]] * num_tiles
    char cpp_path[4096];
    std::snprintf(cpp_path, sizeof(cpp_path), "%s/cpp_tileops.bin", output_dir);

    FILE* fcpp = std::fopen(cpp_path, "wb");
    if (!fcpp) {
        std::fprintf(stderr, "error: cannot open %s\n", cpp_path);
        return 1;
    }
    std::fwrite(&num_tiles, sizeof(uint32_t), 1, fcpp);
    for (uint32_t i = 0; i < num_tiles; ++i) {
        std::fwrite(&outputs[i].a_lo, sizeof(int64_t), 1, fcpp);
        std::fwrite(&outputs[i].b_lo, sizeof(int64_t), 1, fcpp);
        std::fwrite(&outputs[i].prime_count, sizeof(uint32_t), 1, fcpp);
        std::fwrite(outputs[i].tileop.bytes, 1, TILEOP_SIZE, fcpp);
    }
    std::fclose(fcpp);
    std::printf("Wrote C++ reference: %s (%u tiles, %zu bytes)\n",
                cpp_path, num_tiles,
                static_cast<size_t>(sizeof(uint32_t) + num_tiles * 276));

    // Summary
    int overflow_count = 0;
    int empty_count = 0;
    for (uint32_t i = 0; i < num_tiles; ++i) {
        if (outputs[i].tileop.bytes[0] == OVERFLOW_SENTINEL) {
            ++overflow_count;
        } else if (outputs[i].tileop.bytes[0] == EMPTY_OFFSET &&
                   outputs[i].tileop.bytes[1] == EMPTY_OFFSET &&
                   outputs[i].tileop.bytes[2] == EMPTY_OFFSET) {
            ++empty_count;
        }
    }
    std::printf("\nTileOp summary: normal=%u overflow=%d empty=%d\n",
                num_tiles - overflow_count - empty_count, overflow_count, empty_count);

    std::printf("\nNext step: run CUDA dump on the GPU machine:\n");
    std::printf("  ./tile_pipeline dump %s/coords.bin %s/cuda_tileops.bin\n",
                output_dir, output_dir);
    std::printf("Then compare:\n");
    std::printf("  python3 compare_tileops.py %s/cpp_tileops.bin %s/cuda_tileops.bin\n",
                output_dir, output_dir);

    return 0;
}
