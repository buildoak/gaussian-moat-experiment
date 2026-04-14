#include "compositor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr std::size_t kCudaHeaderBytes = 4;
constexpr std::size_t kCudaRecordBytes = 148;
constexpr std::size_t kCudaTileOpOffset = 20;
constexpr std::size_t kTowerBytes =
    static_cast<std::size_t>(TILES_PER_TOWER) * kCudaRecordBytes;

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

const char* verdict_string(CompositorResult::Verdict verdict) {
    return verdict == CompositorResult::SPANNING ? "SPANNING" : "MOAT";
}

void print_usage(const char* argv0) {
    std::fprintf(stderr, "Usage: %s <cuda_binary> <R> <max_towers>\n", argv0);
}

bool parse_int64_arg(const char* text, int64_t& value) {
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed <= 0) {
        return false;
    }
    value = static_cast<int64_t>(parsed);
    return true;
}

bool parse_uint64_arg(const char* text, uint64_t& value) {
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0ULL) {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool read_exact(std::FILE* file, void* dst, std::size_t bytes) {
    return std::fread(dst, 1, bytes, file) == bytes;
}

bool seek_to(std::FILE* file, std::uint64_t offset) {
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
}

bool skip_bytes(std::FILE* file, std::uint64_t offset) {
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_CUR) == 0;
}

uint32_t read_u32_le(const uint8_t* data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

int64_t read_i64_le(const uint8_t* data) {
    int64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool read_tile_count(std::FILE* file, uint32_t& n_tiles) {
    std::array<uint8_t, kCudaHeaderBytes> header{};
    if (!seek_to(file, 0) || !read_exact(file, header.data(), header.size())) {
        return false;
    }
    n_tiles = read_u32_le(header.data());
    return true;
}

bool load_first_tile_coords(std::FILE* file,
                            uint32_t tower_count,
                            std::vector<int64_t>& coords) {
    coords.clear();
    coords.reserve(static_cast<std::size_t>(tower_count) * 2U);

    std::array<uint8_t, kCudaRecordBytes> record{};
    if (!seek_to(file, kCudaHeaderBytes)) {
        return false;
    }

    constexpr std::size_t kSkipPerTower =
        static_cast<std::size_t>(TILES_PER_TOWER - 1) * kCudaRecordBytes;

    for (uint32_t tower = 0; tower < tower_count; ++tower) {
        if (!read_exact(file, record.data(), record.size())) {
            return false;
        }

        coords.push_back(read_i64_le(record.data() + 0));
        coords.push_back(read_i64_le(record.data() + 8));

        if ((tower + 1U) < tower_count && !skip_bytes(file, kSkipPerTower)) {
            return false;
        }
    }

    return true;
}

void build_grid_from_first_tile_coords(int64_t R,
                                       const std::vector<int64_t>& coords,
                                       Grid& grid) {
    if ((coords.size() % 2U) != 0U || coords.empty()) {
        std::fprintf(stderr, "invalid coordinate buffer: %zu values\n", coords.size());
        std::exit(1);
    }

    grid.R = R;
    grid.S = TILE_SIDE;
    grid.W = RADIAL_DEPTH;
    grid.base_y.clear();
    grid.delta.clear();
    grid.tiles_per_tower.clear();
    grid.tower_offset.clear();
    grid.total_tiles = 0;
    grid.base_y.reserve(coords.size() / 2U);

    const std::size_t tower_count = coords.size() / 2U;
    for (std::size_t tower = 0; tower < tower_count; ++tower) {
        const int64_t a_lo = coords[tower * 2U];
        const int64_t b_lo = coords[tower * 2U + 1U];
        const int64_t expected_a = static_cast<int64_t>(tower) * TILE_SIDE;
        if (a_lo != expected_a) {
            std::fprintf(stderr,
                         "grid coordinate mismatch at tower %zu: a_lo=%lld expected=%lld\n",
                         tower,
                         static_cast<long long>(a_lo),
                         static_cast<long long>(expected_a));
            std::exit(1);
        }
        grid.base_y.push_back(b_lo);
    }

    grid.num_towers = static_cast<int>(grid.base_y.size());
    // Uniform tower height for this test path
    grid.tiles_per_tower.assign(static_cast<std::size_t>(grid.num_towers),
                                static_cast<uint32_t>(TILES_PER_TOWER));
    grid.tower_offset.resize(static_cast<std::size_t>(grid.num_towers));
    for (int j = 0; j < grid.num_towers; ++j) {
        grid.tower_offset[static_cast<std::size_t>(j)] =
            static_cast<uint64_t>(j) * static_cast<uint64_t>(TILES_PER_TOWER);
    }
    grid.total_tiles = static_cast<uint64_t>(grid.num_towers) *
                       static_cast<uint64_t>(TILES_PER_TOWER);
    if (grid.num_towers > 1) {
        grid.delta.reserve(static_cast<std::size_t>(grid.num_towers - 1));
        for (int tower = 0; tower < grid.num_towers - 1; ++tower) {
            const int64_t delta = grid.base_y[static_cast<std::size_t>(tower)] -
                                  grid.base_y[static_cast<std::size_t>(tower + 1)];
            if (delta < 0) {
                std::fprintf(stderr,
                             "grid delta underflow at tower %d: %lld -> %lld\n",
                             tower,
                             static_cast<long long>(grid.base_y[static_cast<std::size_t>(tower)]),
                             static_cast<long long>(grid.base_y[static_cast<std::size_t>(tower + 1)]));
                std::exit(1);
            }
            grid.delta.push_back(delta);
        }
    }
}

bool read_tower_tileops(std::FILE* file,
                        std::array<uint8_t, kTowerBytes>& raw_records,
                        std::array<uint8_t, TILES_PER_TOWER * TILEOP_SIZE>& tower_tileops,
                        uint32_t tower_idx) {
    if (!read_exact(file, raw_records.data(), raw_records.size())) {
        std::fprintf(stderr, "failed to read tower %u from input\n", tower_idx);
        return false;
    }

    for (int row = 0; row < TILES_PER_TOWER; ++row) {
        const std::size_t record_offset =
            static_cast<std::size_t>(row) * kCudaRecordBytes + kCudaTileOpOffset;
        const uint8_t* record_tileop = raw_records.data() + record_offset;
        uint8_t* out_tileop =
            tower_tileops.data() + static_cast<std::size_t>(row) * TILEOP_SIZE;

        if (is_overflow(record_tileop)) {
            std::fprintf(stderr,
                         "overflow tile encountered at tower %u row %d; integration test expects none\n",
                         tower_idx,
                         row);
            return false;
        }

        std::memcpy(out_tileop, record_tileop, TILEOP_SIZE);
    }

    return true;
}

uint32_t count_groups_in_tower(const Grid& grid,
                               uint32_t tower_idx,
                               const uint8_t* tower_tileops) {
    uint32_t tower_groups = 0;
    for (int row = 0; row < TILES_PER_TOWER; ++row) {
        const uint8_t* tile =
            tower_tileops + static_cast<std::size_t>(row) * TILEOP_SIZE;
        if (is_tile_dead(grid, static_cast<int32_t>(tower_idx), row) || is_dead(tile)) {
            continue;
        }
        tower_groups += static_cast<uint32_t>(max_group_label(tile));
    }
    return tower_groups;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    int64_t R = 0;
    uint64_t max_towers = 0;
    if (!parse_int64_arg(argv[2], R) || !parse_uint64_arg(argv[3], max_towers)) {
        print_usage(argv[0]);
        return 1;
    }

    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::fopen(argv[1], "rb"),
                                                            &std::fclose);
    if (!file) {
        std::fprintf(stderr, "failed to open %s\n", argv[1]);
        return 1;
    }

    const Clock::time_point wall_start = Clock::now();

    uint32_t n_tiles = 0;
    if (!read_tile_count(file.get(), n_tiles)) {
        std::fprintf(stderr, "failed to read CUDA header from %s\n", argv[1]);
        return 1;
    }
    if ((n_tiles % static_cast<uint32_t>(TILES_PER_TOWER)) != 0U) {
        std::fprintf(stderr, "invalid tile count: %u is not divisible by %d\n", n_tiles,
                     TILES_PER_TOWER);
        return 1;
    }

    const uint32_t tower_count = n_tiles / static_cast<uint32_t>(TILES_PER_TOWER);
    std::vector<int64_t> coords;
    if (!load_first_tile_coords(file.get(), tower_count, coords)) {
        std::fprintf(stderr, "failed during first-pass coordinate extraction\n");
        return 1;
    }

    Grid grid{};
    build_grid_from_first_tile_coords(R, coords, grid);
    if (grid.num_towers != static_cast<int>(tower_count) || grid.base_y.empty()) {
        std::fprintf(stderr, "grid reconstruction failed: towers=%d expected=%u\n",
                     grid.num_towers,
                     tower_count);
        return 1;
    }

    std::printf("grid: R=%lld, towers=%u, base_y[0]=%lld, base_y[last]=%lld\n",
                static_cast<long long>(R),
                tower_count,
                static_cast<long long>(grid.base_y.front()),
                static_cast<long long>(grid.base_y.back()));

    const uint32_t towers_to_process =
        static_cast<uint32_t>(std::min<uint64_t>(max_towers, tower_count));
    std::printf("processing %u towers...\n", towers_to_process);

    if (!seek_to(file.get(), kCudaHeaderBytes)) {
        std::fprintf(stderr, "failed to rewind for tower ingestion\n");
        return 1;
    }

    Compositor compositor;
    compositor.init(grid);

    std::array<uint8_t, kTowerBytes> raw_records{};
    std::array<uint8_t, TILES_PER_TOWER * TILEOP_SIZE> tower_tileops{};
    uint64_t total_groups_seen = 0;

    for (uint32_t tower = 0; tower < towers_to_process; ++tower) {
        if (!read_tower_tileops(file.get(), raw_records, tower_tileops, tower)) {
            return 1;
        }

        const uint32_t tower_groups =
            count_groups_in_tower(grid, tower, tower_tileops.data());
        total_groups_seen += static_cast<uint64_t>(tower_groups);
        compositor.ingest_tower(static_cast<int32_t>(tower), tower_tileops.data());

        if (tower == 0 || tower + 1U == towers_to_process ||
            (tower + 1U) % 100000U == 0U) {
            const double pct = 100.0 * static_cast<double>(tower + 1U) /
                               static_cast<double>(towers_to_process);
            const double avg_groups = static_cast<double>(total_groups_seen) /
                                      static_cast<double>((static_cast<uint64_t>(tower) + 1ULL) *
                                                          static_cast<uint64_t>(TILES_PER_TOWER));
            std::printf("  tower %u/%u (%.1f%%): groups=%u, total=%llu, avg=%.1f/tile\n",
                        tower, towers_to_process, pct,
                        tower_groups,
                        static_cast<unsigned long long>(total_groups_seen),
                        avg_groups);
            std::fflush(stdout);
        }
    }

    const CompositorResult result = compositor.finalize();
    if (result.total_groups == 0U) {
        std::fprintf(stderr, "invariant failed: total_groups == 0\n");
        return 1;
    }
    if (result.inner_root_count == 0U) {
        std::fprintf(stderr, "invariant failed: inner_root_count == 0\n");
        return 1;
    }
    if (static_cast<uint64_t>(result.total_groups) != total_groups_seen) {
        std::fprintf(stderr,
                     "group count mismatch: finalize=%u estimated=%llu\n",
                     result.total_groups,
                     static_cast<unsigned long long>(total_groups_seen));
        return 1;
    }

    const Clock::time_point wall_end = Clock::now();
    std::printf("result: %s, total_groups=%u, inner_roots=%u, outer_roots=%u\n",
                verdict_string(result.verdict),
                result.total_groups,
                result.inner_root_count,
                result.outer_root_count);
    std::printf("wall: %.3fms\n", elapsed_ms(wall_start, wall_end));
    return 0;
}
