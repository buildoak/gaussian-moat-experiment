#include "compositor.h"
#include "grid.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {

constexpr std::size_t kHeaderBytes = 4;
constexpr std::size_t kRecordBytes = 148;
constexpr std::size_t kTileOpOffset = 20;
constexpr std::size_t kTowerBytes =
    static_cast<std::size_t>(TILES_PER_TOWER) * kRecordBytes;

using Clock = std::chrono::steady_clock;

const char* verdict_string(CompositorResult::Verdict verdict) {
    return verdict == CompositorResult::SPANNING ? "SPANNING" : "MOAT";
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s <binary_path> <R_value> [--max-towers N] [--progress-interval N]\n",
                 argv0);
}

bool parse_positive_i64(const char* text, int64_t& value) {
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed <= 0) {
        return false;
    }
    value = static_cast<int64_t>(parsed);
    return true;
}

bool parse_positive_u64(const char* text, uint64_t& value) {
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

std::string basename_of(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash == nullptr ? std::string(path) : std::string(slash + 1);
}

bool read_tile_count(std::FILE* file, uint32_t& n_tiles) {
    std::array<uint8_t, kHeaderBytes> header{};
    if (!seek_to(file, 0) || !read_exact(file, header.data(), header.size())) {
        return false;
    }
    n_tiles = read_u32_le(header.data());
    return true;
}

bool load_coords_from_first_tile(std::FILE* file,
                                 uint32_t tower_count,
                                 std::vector<int64_t>& coords) {
    coords.assign(static_cast<std::size_t>(tower_count) *
                      static_cast<std::size_t>(TILES_PER_TOWER) * 2U,
                  0);

    std::array<uint8_t, kRecordBytes> record{};
    for (uint32_t tower = 0; tower < tower_count; ++tower) {
        const std::uint64_t offset =
            kHeaderBytes + static_cast<std::uint64_t>(tower) * kTowerBytes;
        if (!seek_to(file, offset) || !read_exact(file, record.data(), record.size())) {
            return false;
        }

        const std::size_t coord_offset =
            static_cast<std::size_t>(tower) * static_cast<std::size_t>(TILES_PER_TOWER) * 2U;
        coords[coord_offset] = read_i64_le(record.data());
        coords[coord_offset + 1U] = read_i64_le(record.data() + 8);
    }

    return true;
}

bool read_tower_tileops(std::FILE* file,
                        uint32_t tower_idx,
                        std::array<uint8_t, kTowerBytes>& raw_records,
                        std::array<uint8_t, TILES_PER_TOWER * TILEOP_SIZE>& tower_tileops) {
    const std::uint64_t offset =
        kHeaderBytes + static_cast<std::uint64_t>(tower_idx) * kTowerBytes;
    if (!seek_to(file, offset) || !read_exact(file, raw_records.data(), raw_records.size())) {
        return false;
    }

    for (int row = 0; row < TILES_PER_TOWER; ++row) {
        const std::size_t record_offset =
            static_cast<std::size_t>(row) * kRecordBytes + kTileOpOffset;
        std::memcpy(tower_tileops.data() + static_cast<std::size_t>(row) * TILEOP_SIZE,
                    raw_records.data() + record_offset,
                    TILEOP_SIZE);
    }

    return true;
}

uint32_t count_groups_in_tower(const Grid& grid, uint32_t tower_idx, const uint8_t* tower_tileops) {
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

double peak_rss_mb() {
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* binary_path = argv[1];
    int64_t R = 0;
    if (!parse_positive_i64(argv[2], R)) {
        print_usage(argv[0]);
        return 1;
    }

    uint64_t max_towers = 0;
    uint64_t progress_interval = 1000;
    bool has_max_towers = false;

    for (int argi = 3; argi < argc; ++argi) {
        if (std::strcmp(argv[argi], "--max-towers") == 0) {
            if (argi + 1 >= argc || !parse_positive_u64(argv[argi + 1], max_towers)) {
                print_usage(argv[0]);
                return 1;
            }
            has_max_towers = true;
            ++argi;
            continue;
        }
        if (std::strcmp(argv[argi], "--progress-interval") == 0) {
            if (argi + 1 >= argc || !parse_positive_u64(argv[argi + 1], progress_interval)) {
                print_usage(argv[0]);
                return 1;
            }
            ++argi;
            continue;
        }

        print_usage(argv[0]);
        return 1;
    }

    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::fopen(binary_path, "rb"),
                                                            &std::fclose);
    if (!file) {
        std::fprintf(stderr, "failed to open %s\n", binary_path);
        return 1;
    }

    uint32_t n_tiles = 0;
    if (!read_tile_count(file.get(), n_tiles)) {
        std::fprintf(stderr, "failed to read header from %s\n", binary_path);
        return 1;
    }
    if (n_tiles == 0U || (n_tiles % static_cast<uint32_t>(TILES_PER_TOWER)) != 0U) {
        std::fprintf(stderr, "invalid tile count: %" PRIu32 "\n", n_tiles);
        return 1;
    }

    const uint32_t total_towers = n_tiles / static_cast<uint32_t>(TILES_PER_TOWER);
    const uint32_t towers_to_process = has_max_towers
                                           ? static_cast<uint32_t>(
                                                 std::min<uint64_t>(max_towers, total_towers))
                                           : total_towers;

    std::vector<int64_t> coords;
    if (!load_coords_from_first_tile(file.get(), total_towers, coords)) {
        std::fprintf(stderr, "failed during first-pass coordinate extraction\n");
        return 1;
    }

    Grid grid{};
    compute_grid_from_coords(R, coords.data(), n_tiles, grid);
    if (grid.num_towers != static_cast<int>(total_towers)) {
        std::fprintf(stderr, "grid reconstruction failed: towers=%d expected=%" PRIu32 "\n",
                     grid.num_towers,
                     total_towers);
        return 1;
    }

    std::printf("=== Gaussian Moat Compositor Campaign ===\n");
    std::printf("binary: %s\n", basename_of(binary_path).c_str());
    std::printf("R: %" PRId64 "\n", R);
    std::printf("towers: %" PRIu32 "\n", towers_to_process);
    std::printf("tiles: %" PRIu32 "\n\n", n_tiles);

    const Clock::time_point wall_start = Clock::now();

    Compositor compositor;
    compositor.init(grid);

    std::array<uint8_t, kTowerBytes> raw_records{};
    std::array<uint8_t, TILES_PER_TOWER * TILEOP_SIZE> tower_tileops{};
    uint64_t total_groups_seen = 0;

    for (uint32_t tower = 0; tower < towers_to_process; ++tower) {
        if (!read_tower_tileops(file.get(), tower, raw_records, tower_tileops)) {
            std::fprintf(stderr, "failed to read tower %" PRIu32 "\n", tower);
            return 1;
        }

        const uint32_t tower_groups =
            count_groups_in_tower(grid, tower, tower_tileops.data());
        total_groups_seen += static_cast<uint64_t>(tower_groups);
        compositor.ingest_tower(static_cast<int32_t>(tower), tower_tileops.data(), nullptr);

        if (((tower + 1U) % static_cast<uint32_t>(progress_interval)) == 0U ||
            tower + 1U == towers_to_process) {
            const double pct =
                100.0 * static_cast<double>(tower + 1U) / static_cast<double>(towers_to_process);
            const double avg =
                static_cast<double>(total_groups_seen) /
                (static_cast<double>(tower + 1U) * static_cast<double>(TILES_PER_TOWER));
            std::printf("  tower %" PRIu32 "/%" PRIu32 " (%.1f%%): groups=%" PRIu32
                        ", total_groups=%" PRIu64 ", avg=%.1f/tile\n",
                        tower + 1U,
                        towers_to_process,
                        pct,
                        tower_groups,
                        total_groups_seen,
                        avg);
            std::fflush(stdout);
        }
    }

    const CompositorResult result = compositor.finalize();
    const Clock::time_point wall_end = Clock::now();
    const double wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();
    const double avg_groups_per_tile =
        static_cast<double>(result.total_groups) /
        static_cast<double>(static_cast<uint64_t>(towers_to_process) *
                            static_cast<uint64_t>(TILES_PER_TOWER));

    std::printf("\n=== RESULT ===\n");
    std::printf("verdict: %s\n", verdict_string(result.verdict));
    std::printf("total_groups: %" PRIu32 "\n", result.total_groups);
    std::printf("inner_roots: %" PRIu32 "\n", result.inner_root_count);
    std::printf("outer_roots: %" PRIu32 "\n", result.outer_root_count);
    std::printf("wall_time: %.3fs\n", wall_seconds);
    std::printf("peak_rss: %.0f MB\n", peak_rss_mb());
    std::printf("groups/tile_avg: %.1f\n", avg_groups_per_tile);

    return 0;
}
