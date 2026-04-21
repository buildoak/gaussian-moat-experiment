// cpp_dump: Process tiles through the C++ reference pipeline and dump TileOps + prime counts
// to a binary file for cross-validation against the CUDA kernel.
//
// Usage:
//   ./cpp_dump [-c coords.bin] [-o output.bin] [-n N]
//
//   -c coords.bin   Read tile coordinates from binary file (little-endian):
//                     uint32_t num_tiles
//                     num_tiles * { int64_t a_lo, int64_t b_lo }
//                   If omitted, falls back to hardcoded test coords.
//   -o output.bin   Output file (default: cpp_tileops.bin)
//   -n N            Number of tiles when using hardcoded coords (default: 1000)
//
// Output format (binary, little-endian):
//   uint32_t num_tiles
//   For each tile:
//     int64_t a_lo, b_lo          (16 bytes)
//     uint32_t prime_count         (4 bytes)
//     uint8_t tileop[128]          (128 bytes)
//   Total per tile: 148 bytes

#include "test_coords.h"

// tile-cpp headers
#include "process_tile.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Load coordinates from a binary file produced by gen_coords.py.
// Format: uint32_t num_tiles, then num_tiles * {int64_t a_lo, int64_t b_lo}.
// Returns false on error.
static bool load_coords_from_file(const char* path, std::vector<TileCoord>& out) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        std::fprintf(stderr, "error: cannot open coords file %s\n", path);
        return false;
    }

    uint32_t n = 0;
    if (std::fread(&n, sizeof(uint32_t), 1, fp) != 1) {
        std::fprintf(stderr, "error: failed to read num_tiles from %s\n", path);
        std::fclose(fp);
        return false;
    }

    out.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (std::fread(&out[i].a_lo, sizeof(int64_t), 1, fp) != 1 ||
            std::fread(&out[i].b_lo, sizeof(int64_t), 1, fp) != 1) {
            std::fprintf(stderr, "error: unexpected EOF reading coord %u from %s\n", i, path);
            std::fclose(fp);
            return false;
        }
    }

    std::fclose(fp);
    return true;
}

int main(int argc, char** argv) {
    int num_tiles = 1000;
    const char* output_path = "cpp_tileops.bin";
    const char* coords_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            num_tiles = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            coords_path = argv[++i];
        }
    }

    // Build coordinate list: from file or hardcoded generator
    std::vector<TileCoord> coords;
    if (coords_path) {
        if (!load_coords_from_file(coords_path, coords)) {
            return 1;
        }
        num_tiles = static_cast<int>(coords.size());
        std::fprintf(stderr, "cpp_dump: loaded %d tiles from %s\n", num_tiles, coords_path);
    } else {
        auto test_coords = generate_test_coords(num_tiles);
        coords.resize(num_tiles);
        for (int i = 0; i < num_tiles; ++i) {
            coords[i].a_lo = test_coords[i].a_lo;
            coords[i].b_lo = test_coords[i].b_lo;
        }
        std::fprintf(stderr, "cpp_dump: using %d hardcoded test coords\n", num_tiles);
    }

    // Init sieve tables
    SieveTables tables;
    if (!init_sieve_tables(tables)) {
        std::fprintf(stderr, "error: init_sieve_tables failed\n");
        return 1;
    }

    // Open output file
    FILE* fp = std::fopen(output_path, "wb");
    if (!fp) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", output_path);
        return 1;
    }

    // Write header
    uint32_t n = static_cast<uint32_t>(num_tiles);
    std::fwrite(&n, sizeof(uint32_t), 1, fp);

    auto t_start = std::chrono::steady_clock::now();

    for (int i = 0; i < num_tiles; ++i) {
        TileResult result = process_tile(coords[i], tables);

        // Write: a_lo, b_lo, prime_count, tileop
        std::fwrite(&coords[i].a_lo, sizeof(int64_t), 1, fp);
        std::fwrite(&coords[i].b_lo, sizeof(int64_t), 1, fp);
        std::fwrite(&result.prime_count, sizeof(uint32_t), 1, fp);
        std::fwrite(result.tileop.bytes, 1, 128, fp);

        if ((i + 1) % 1000 == 0) {
            std::fprintf(stderr, "  Processed %d/%d tiles...\n", i + 1, num_tiles);
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count() / 1000.0;

    std::fclose(fp);
    std::fprintf(stderr, "cpp_dump: wrote %d tiles to %s in %.1f ms (%.2f ms/tile)\n",
                 num_tiles, output_path, elapsed_ms, elapsed_ms / num_tiles);

    return 0;
}
