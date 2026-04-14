// campaign.cpp — Campaign runner v7, C++ + CUDA tile paths
//
// Orchestrates: grid construction → sieve init → tower-by-tower tile processing
// → compositor ingestion → incremental spanning check → verdict.
//
// Two tile paths:
//   Default (C++): process tiles one tower at a time via libtile.a
//   --cuda:        shell out to tile_kernel_multi in GPU bursts, tower j=0 still C++
//
// Links against:
//   libcompositor.a  (grid, compositor, tileop_parse)
//   libtile.a        (process_tile, sieve, encode, union_find)
//
// Type conflict note:
//   tiles-compositor/include/types.h and tile-cpp/include/constants.h both define
//   TILE_SIDE, TILEOP_SIZE, OVERFLOW_SENTINEL, EMPTY_OFFSET with identical values.
//   Including both causes a redefinition error. We solve this by including only
//   compositor headers and forward-declaring the tile-cpp types/functions we need.
//   The linker resolves symbols from libtile.a.

#include "compositor.h"
#include "grid.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// Forward declarations for tile-cpp types and functions.
// These mirror tile-cpp/include/{types.h, sieve.h, constants.h, process_tile.h}
// but avoid including those headers (which would collide with compositor types.h).
// ---------------------------------------------------------------------------

// From tile-cpp/include/constants.h — K_SQ propagated via -DK_SQ_VAL=N
#ifndef K_SQ_VAL
#define K_SQ_VAL 40
#endif
static constexpr int32_t TILE_CPP_K_SQ = K_SQ_VAL;
static constexpr int TILE_CPP_SPLIT_PRIMES_COUNT = 609;
static constexpr int TILE_CPP_INERT_PRIMES_COUNT = 619;
// static constexpr uint32_t TILE_CPP_SIEVE_LIMIT = 10000;  // available if needed

// From tile-cpp/include/types.h
struct TileCoord {
    int64_t a_lo;
    int64_t b_lo;
};

struct TileOp {
    uint8_t bytes[TILEOP_SIZE];  // TILEOP_SIZE = 256, from compositor types.h
};

struct PhaseTimings {
    int64_t sieve_ns;
    int64_t compact_ns;
    int64_t union_find_ns;
    int64_t face_extract_ns;
    int64_t prune_encode_ns;
    int64_t total_ns;
};

struct TileResult {
    TileOp   tileop;
    uint32_t prime_count;
    uint32_t group_count;
    uint32_t ports_before_pruning;
    uint32_t ports_after_pruning;
};

// From tile-cpp/include/sieve.h
struct SieveTables {
    uint32_t split_table[TILE_CPP_SPLIT_PRIMES_COUNT];
    uint16_t inert_primes[TILE_CPP_INERT_PRIMES_COUNT];
    int split_count;
    int inert_count;
};

extern "C++" {
    bool init_sieve_tables(SieveTables& tables);
    TileResult process_tile(const TileCoord& coord, const SieveTables& tables,
                            PhaseTimings* timings = nullptr);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

enum class OutputMode {
    VERDICT_ONLY,
    DUMP,
    DUMP_WITH_STATS,
};

const char* verdict_string(CompositorResult::Verdict v) {
    return v == CompositorResult::SPANNING ? "SPANNING" : "MOAT";
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s R [--k-sq N] [--mode verdict_only|dump|dump_with_stats] "
        "[--progress-interval N] [--cuda|--cuda-stream] [--cuda-binary PATH] [--burst-size N] "
        "[--no-early-exit]\n", argv0);
}

bool file_exists(const char* path) {
    struct stat st {};
    return ::stat(path, &st) == 0;
}

std::string make_temp_path(const char* prefix) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/%s_XXXXXX.bin", prefix);
    // mkstemp needs mutable, and modifies in place
    // We'll use a simpler pattern: pid + a counter
    static int counter = 0;
    std::snprintf(buf, sizeof(buf), "/tmp/%s_%d_%d.bin", prefix, static_cast<int>(::getpid()), counter++);
    return std::string(buf);
}

std::string resolve_cuda_binary(const std::string& explicit_path) {
    if (!explicit_path.empty()) {
        if (file_exists(explicit_path.c_str())) {
            return explicit_path;
        }
        std::fprintf(stderr, "error: CUDA binary not found: %s\n", explicit_path.c_str());
        return {};
    }
    // Check ./tile_kernel_multi
    if (file_exists("./tile_kernel_multi")) {
        return "./tile_kernel_multi";
    }
    // Check PATH via which
    FILE* fp = ::popen("which tile_kernel_multi 2>/dev/null", "r");
    if (fp) {
        char path_buf[512];
        if (std::fgets(path_buf, sizeof(path_buf), fp)) {
            ::pclose(fp);
            // Strip trailing newline
            size_t len = std::strlen(path_buf);
            while (len > 0 && (path_buf[len - 1] == '\n' || path_buf[len - 1] == '\r')) {
                path_buf[--len] = '\0';
            }
            if (len > 0 && file_exists(path_buf)) {
                return std::string(path_buf);
            }
        } else {
            ::pclose(fp);
        }
    }
    std::fprintf(stderr, "error: CUDA binary 'tile_kernel_multi' not found in PATH or current directory\n");
    return {};
}

// Write burst_index.bin for a batch of towers.
// Format: uint32_t num_towers, uint32_t total_tiles, uint32_t tiles_per_tower[num_towers]
bool write_burst_index(const char* path, const std::vector<uint32_t>& tpt,
                       uint32_t num_towers_in_burst, uint32_t total_tiles_in_burst) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = true;
    ok = ok && std::fwrite(&num_towers_in_burst, sizeof(uint32_t), 1, f) == 1;
    ok = ok && std::fwrite(&total_tiles_in_burst, sizeof(uint32_t), 1, f) == 1;
    ok = ok && std::fwrite(tpt.data(), sizeof(uint32_t), num_towers_in_burst, f) == num_towers_in_burst;
    std::fclose(f);
    return ok;
}

// Write coords.bin for a batch of tiles.
// Format: uint32_t num_tiles, then num_tiles * (int64_t a_lo, int64_t b_lo)
bool write_coords_file(const char* path, const std::vector<TileCoord>& coords, uint32_t num_tiles) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = true;
    ok = ok && std::fwrite(&num_tiles, sizeof(uint32_t), 1, f) == 1;
    for (uint32_t i = 0; i < num_tiles && ok; ++i) {
        ok = ok && std::fwrite(&coords[i].a_lo, sizeof(int64_t), 1, f) == 1;
        ok = ok && std::fwrite(&coords[i].b_lo, sizeof(int64_t), 1, f) == 1;
    }
    std::fclose(f);
    return ok;
}

// Read raw TileOp output from CUDA campaign — just total_tiles * 256 bytes.
bool read_raw_tileops(const char* path, uint8_t* buf, uint32_t total_tiles) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    const size_t expected = static_cast<size_t>(total_tiles) * TILEOP_SIZE;

    // BUG-3 FIX: Verify file size matches expected before reading.
    // An oversized file indicates a CUDA kernel bug or wrong tile count.
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    if (file_size < 0 || static_cast<size_t>(file_size) != expected) {
        std::fprintf(stderr, "ERROR: output file size %ld != expected %zu for %s\n",
                     file_size, expected, path);
        std::fclose(f);
        return false;
    }
    std::fseek(f, 0, SEEK_SET);

    size_t got = std::fread(buf, 1, expected, f);
    std::fclose(f);
    return got == expected;
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

bool parse_positive_u32(const char* text, uint32_t& value) {
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0UL || parsed > UINT32_MAX) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
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

// ---------------------------------------------------------------------------
// Persistent CUDA subprocess with bidirectional pipes.
//
// Protocol (binary, little-endian, over stdin/stdout):
//   Campaign → CUDA (per burst):
//     uint32_t num_tiles
//     uint32_t burst_idx_bytes   (size of burst_index payload)
//     uint32_t coords_bytes      (size of coords payload)
//     [burst_idx data: uint32_t num_towers, uint32_t total_tiles, uint32_t tpt[N]]
//     [coords data: uint32_t num_tiles, TileCoord[N]]
//
//   CUDA → Campaign (per burst):
//     uint32_t num_tiles
//     uint32_t output_bytes
//     [raw TileOp data: num_tiles * 256 bytes]
//
//   Termination: close write pipe → CUDA sees EOF → exits.
// ---------------------------------------------------------------------------

struct CudaStreamProcess {
    pid_t pid = -1;
    int write_fd = -1;   // campaign writes burst data here (CUDA's stdin)
    int read_fd = -1;    // campaign reads results here (CUDA's stdout)

    bool alive() const { return pid > 0; }

    // Launch the CUDA binary in stream mode.
    // Returns true on success.
    bool launch(const std::string& cuda_binary) {
        int pipe_to_cuda[2] = {-1, -1};    // [0]=read(child), [1]=write(parent)
        int pipe_from_cuda[2] = {-1, -1};  // [0]=read(parent), [1]=write(child)

        if (::pipe(pipe_to_cuda) != 0 || ::pipe(pipe_from_cuda) != 0) {
            std::fprintf(stderr, "error: pipe() failed: %s\n", std::strerror(errno));
            return false;
        }

        pid = ::fork();
        if (pid < 0) {
            std::fprintf(stderr, "error: fork() failed: %s\n", std::strerror(errno));
            ::close(pipe_to_cuda[0]); ::close(pipe_to_cuda[1]);
            ::close(pipe_from_cuda[0]); ::close(pipe_from_cuda[1]);
            return false;
        }

        if (pid == 0) {
            // --- Child process: becomes the CUDA binary ---
            // stdin = read end of pipe_to_cuda
            ::dup2(pipe_to_cuda[0], STDIN_FILENO);
            // stdout = write end of pipe_from_cuda
            ::dup2(pipe_from_cuda[1], STDOUT_FILENO);
            // Close all unused FDs
            ::close(pipe_to_cuda[0]);
            ::close(pipe_to_cuda[1]);
            ::close(pipe_from_cuda[0]);
            ::close(pipe_from_cuda[1]);

            ::execl(cuda_binary.c_str(), cuda_binary.c_str(), "stream", nullptr);
            // If execl returns, it failed
            std::fprintf(stderr, "error: execl(%s) failed: %s\n",
                         cuda_binary.c_str(), std::strerror(errno));
            ::_exit(127);
        }

        // --- Parent process ---
        // Close unused ends
        ::close(pipe_to_cuda[0]);
        ::close(pipe_from_cuda[1]);

        write_fd = pipe_to_cuda[1];
        read_fd = pipe_from_cuda[0];

        return true;
    }

    // Send a complete burst to the CUDA subprocess.
    // burst_idx: [uint32_t num_towers, uint32_t total_tiles, uint32_t tpt[N]]
    // coords:    [uint32_t num_tiles, TileCoord[N]]
    bool send_burst(const uint8_t* burst_idx_data, uint32_t burst_idx_bytes,
                    const uint8_t* coords_data, uint32_t coords_bytes,
                    uint32_t num_tiles) {
        // Write header: num_tiles, burst_idx_bytes, coords_bytes
        uint32_t header[3] = {num_tiles, burst_idx_bytes, coords_bytes};
        if (!write_all(header, sizeof(header))) return false;
        if (!write_all(burst_idx_data, burst_idx_bytes)) return false;
        if (!write_all(coords_data, coords_bytes)) return false;
        return true;
    }

    // Read burst result from the CUDA subprocess.
    // Returns the number of tiles in the response, or 0 on error.
    // output_buf must be large enough for num_tiles * TILEOP_SIZE bytes.
    uint32_t recv_burst(uint8_t* output_buf) {
        uint32_t resp_header[2] = {0, 0};  // num_tiles, output_bytes
        if (!read_all(resp_header, sizeof(resp_header))) return 0;

        const uint32_t resp_tiles = resp_header[0];
        const uint32_t resp_bytes = resp_header[1];
        const uint32_t expected_bytes = resp_tiles * TILEOP_SIZE;

        if (resp_bytes != expected_bytes) {
            std::fprintf(stderr, "error: CUDA stream response size mismatch: "
                         "got %u bytes, expected %u for %u tiles\n",
                         resp_bytes, expected_bytes, resp_tiles);
            return 0;
        }

        if (!read_all(output_buf, resp_bytes)) return 0;
        return resp_tiles;
    }

    // Close write pipe (signals EOF to child) and wait for exit.
    int shutdown() {
        if (write_fd >= 0) {
            ::close(write_fd);
            write_fd = -1;
        }
        int status = 0;
        if (pid > 0) {
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        if (read_fd >= 0) {
            ::close(read_fd);
            read_fd = -1;
        }
        return status;
    }

    // Kill and reap if still alive (for error paths).
    void kill_and_reap() {
        if (write_fd >= 0) { ::close(write_fd); write_fd = -1; }
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            ::waitpid(pid, nullptr, 0);
            pid = -1;
        }
        if (read_fd >= 0) { ::close(read_fd); read_fd = -1; }
    }

private:
    bool write_all(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t remaining = len;
        while (remaining > 0) {
            ssize_t n = ::write(write_fd, p, remaining);
            if (n <= 0) {
                std::fprintf(stderr, "error: write to CUDA subprocess failed: %s\n",
                             n == 0 ? "EOF" : std::strerror(errno));
                return false;
            }
            p += n;
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }

    bool read_all(void* data, size_t len) {
        uint8_t* p = static_cast<uint8_t*>(data);
        size_t remaining = len;
        while (remaining > 0) {
            ssize_t n = ::read(read_fd, p, remaining);
            if (n <= 0) {
                std::fprintf(stderr, "error: read from CUDA subprocess failed: %s\n",
                             n == 0 ? "EOF" : std::strerror(errno));
                return false;
            }
            p += n;
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // --- CLI parse ---
    int64_t R = 0;
    if (!parse_positive_i64(argv[1], R)) {
        std::fprintf(stderr, "error: R must be a positive integer\n");
        print_usage(argv[0]);
        return 1;
    }

    uint32_t k_sq = 40;
    OutputMode mode = OutputMode::VERDICT_ONLY;
    uint32_t progress_interval = 1000;
    bool use_cuda = false;
    bool use_cuda_stream = false;
    bool no_early_exit = false;
    std::string cuda_binary_path;
    uint32_t burst_size = 28000;

    for (int argi = 2; argi < argc; ++argi) {
        if (std::strcmp(argv[argi], "--k-sq") == 0) {
            if (argi + 1 >= argc || !parse_positive_u32(argv[argi + 1], k_sq)) {
                std::fprintf(stderr, "error: --k-sq requires a positive integer\n");
                return 1;
            }
            ++argi;
            continue;
        }
        if (std::strcmp(argv[argi], "--mode") == 0) {
            if (argi + 1 >= argc) {
                std::fprintf(stderr, "error: --mode requires a value\n");
                return 1;
            }
            ++argi;
            if (std::strcmp(argv[argi], "verdict_only") == 0) {
                mode = OutputMode::VERDICT_ONLY;
            } else if (std::strcmp(argv[argi], "dump") == 0) {
                mode = OutputMode::DUMP;
            } else if (std::strcmp(argv[argi], "dump_with_stats") == 0) {
                mode = OutputMode::DUMP_WITH_STATS;
            } else {
                std::fprintf(stderr, "error: unknown mode '%s'\n", argv[argi]);
                return 1;
            }
            continue;
        }
        if (std::strcmp(argv[argi], "--progress-interval") == 0) {
            if (argi + 1 >= argc || !parse_positive_u32(argv[argi + 1], progress_interval)) {
                std::fprintf(stderr, "error: --progress-interval requires a positive integer\n");
                return 1;
            }
            ++argi;
            continue;
        }
        if (std::strcmp(argv[argi], "--cuda") == 0) {
            use_cuda = true;
            continue;
        }
        if (std::strcmp(argv[argi], "--cuda-stream") == 0) {
            use_cuda = true;
            use_cuda_stream = true;
            continue;
        }
        if (std::strcmp(argv[argi], "--cuda-binary") == 0) {
            if (argi + 1 >= argc) {
                std::fprintf(stderr, "error: --cuda-binary requires a path\n");
                return 1;
            }
            ++argi;
            cuda_binary_path = argv[argi];
            continue;
        }
        if (std::strcmp(argv[argi], "--burst-size") == 0) {
            if (argi + 1 >= argc || !parse_positive_u32(argv[argi + 1], burst_size)) {
                std::fprintf(stderr, "error: --burst-size requires a positive integer\n");
                return 1;
            }
            ++argi;
            continue;
        }
        if (std::strcmp(argv[argi], "--no-early-exit") == 0) {
            no_early_exit = true;
            continue;
        }

        std::fprintf(stderr, "error: unknown option '%s'\n", argv[argi]);
        print_usage(argv[0]);
        return 1;
    }

    // --- Validate K_SQ against compiled tile-cpp constant ---
    if (k_sq != static_cast<uint32_t>(TILE_CPP_K_SQ)) {
        std::fprintf(stderr,
            "error: --k-sq %" PRIu32 " does not match compiled tile-cpp K_SQ = %d\n"
            "Recompile tile-cpp with the desired K_SQ value.\n",
            k_sq, TILE_CPP_K_SQ);
        return 1;
    }

    // --- Resolve CUDA binary if requested ---
    std::string resolved_cuda_binary;
    if (use_cuda) {
        resolved_cuda_binary = resolve_cuda_binary(cuda_binary_path);
        if (resolved_cuda_binary.empty()) {
            return 1;
        }
    }

    // --- Grid construction ---
    Grid grid{};
    compute_grid(R, grid);

    const int num_towers = grid.num_towers;
    const uint64_t total_tiles = grid.total_tiles;

    std::printf("=== Gaussian Moat Campaign (%s tile path) ===\n",
                use_cuda ? (use_cuda_stream ? "CUDA stream" : "CUDA") : "C++");
    std::printf("R:            %" PRId64 "\n", R);
    std::printf("K_SQ:         %" PRIu32 "\n", k_sq);
    std::printf("TILE_SIDE:    %d\n", TILE_SIDE);
    std::printf("num_towers:   %d\n", num_towers);
    std::printf("total_tiles:  %" PRIu64 "\n", total_tiles);
    if (use_cuda) {
        std::printf("cuda_binary:  %s\n", resolved_cuda_binary.c_str());
        std::printf("burst_size:   %" PRIu32 "\n", burst_size);
    }
    std::printf("\n");
    std::fflush(stdout);

    if (num_towers == 0) {
        std::fprintf(stderr, "error: grid has zero towers for R=%" PRId64 "\n", R);
        return 1;
    }

    // --- Sieve table init (needed for C++ path, and for tower j=0 in CUDA mode) ---
    SieveTables tables;
    if (!init_sieve_tables(tables)) {
        std::fprintf(stderr, "error: init_sieve_tables failed\n");
        return 1;
    }

    // --- Compositor init ---
    Compositor compositor;
    compositor.init(grid);
    compositor.set_burst_mode(true);  // enable incremental spanning check

    const Clock::time_point wall_start = Clock::now();

    bool spanning_found = false;
    int32_t spanning_tower = -1;
    uint64_t tiles_processed = 0;
    uint64_t overflow_count = 0;

    if (use_cuda) {
        // =====================================================================
        // CUDA burst path: process tower j=0 with C++ (spec requirement),
        // then remaining towers in GPU bursts.
        // =====================================================================

        // --- Tower j=0: C++ path (on-axis sieve correction per campaign_spec S6) ---
        if (num_towers > 0) {
            const uint32_t tpt0 = grid.tiles_per_tower[0];
            const std::size_t buf_size0 = static_cast<std::size_t>(tpt0) * TILEOP_SIZE;
            std::vector<uint8_t> tower_buf0(buf_size0, 0);

            for (uint32_t r = 0; r < tpt0; ++r) {
                const int64_t a_lo = 0;  // tower j=0
                const int64_t b_lo = grid.base_y[0] + static_cast<int64_t>(r) * TILE_SIDE;
                TileCoord coord{a_lo, b_lo};
                TileResult result = process_tile(coord, tables);
                std::memcpy(tower_buf0.data() + static_cast<std::size_t>(r) * TILEOP_SIZE,
                            result.tileop.bytes, TILEOP_SIZE);
                if (result.tileop.bytes[0] == OVERFLOW_SENTINEL) {
                    overflow_count++;
                    // BUG-2 FIX: replace overflow with empty TileOp
                    std::fprintf(stderr,
                        "warning: overflow tile at tower 0 row %u replaced with empty\n", r);
                    const std::size_t toff = static_cast<std::size_t>(r) * TILEOP_SIZE;
                    std::memset(tower_buf0.data() + toff, 0, TILEOP_SIZE);
                    // Dead-tile signature: [EMPTY_OFFSET, EMPTY_OFFSET, EMPTY_OFFSET, 0]
                    tower_buf0[toff]     = EMPTY_OFFSET;
                    tower_buf0[toff + 1] = EMPTY_OFFSET;
                    tower_buf0[toff + 2] = EMPTY_OFFSET;
                }
            }

            compositor.ingest_tower(0, tower_buf0.data(), nullptr);
            tiles_processed += tpt0;

            if (compositor.check_spanning_incremental()) {
                compositor.collect_outer_boundary(0);
                spanning_found = true;
                spanning_tower = 0;
            }

            std::printf("  tower 1/%d (C++ j=0) elapsed %.1fs\n",
                        num_towers,
                        std::chrono::duration<double>(Clock::now() - wall_start).count());
            std::fflush(stdout);
        }

        // --- Remaining towers via CUDA bursts ---
        if ((!spanning_found || no_early_exit) && num_towers > 1) {
            const int32_t first_cuda_tower = 1;
            const int32_t remaining_towers = num_towers - first_cuda_tower;

          if (use_cuda_stream) {
            // =============================================================
            // CUDA stream path: persistent subprocess with pipe protocol.
            // One process launch, CUDA context initialized once, bursts
            // streamed over stdin/stdout.
            // =============================================================
            CudaStreamProcess cuda_proc;
            if (!cuda_proc.launch(resolved_cuda_binary)) {
                std::fprintf(stderr, "error: failed to launch CUDA stream subprocess\n");
                return 1;
            }
            std::fprintf(stderr, "cuda-stream: launched pid %d\n",
                         static_cast<int>(cuda_proc.pid));

            for (int32_t burst_start = 0;
                 burst_start < remaining_towers && (!spanning_found || no_early_exit);
                 burst_start += static_cast<int32_t>(burst_size)) {

                const int32_t towers_in_burst = std::min(
                    static_cast<int32_t>(burst_size),
                    remaining_towers - burst_start);

                // Build tiles_per_tower and coords for this burst
                std::vector<uint32_t> burst_tpt(static_cast<size_t>(towers_in_burst));
                uint32_t burst_total_tiles = 0;
                for (int32_t bi = 0; bi < towers_in_burst; ++bi) {
                    const int32_t j = first_cuda_tower + burst_start + bi;
                    burst_tpt[static_cast<size_t>(bi)] =
                        grid.tiles_per_tower[static_cast<std::size_t>(j)];
                    burst_total_tiles += burst_tpt[static_cast<size_t>(bi)];
                }

                // Build coords array (tower-major order)
                std::vector<TileCoord> burst_coords;
                burst_coords.reserve(burst_total_tiles);
                for (int32_t bi = 0; bi < towers_in_burst; ++bi) {
                    const int32_t j = first_cuda_tower + burst_start + bi;
                    const uint32_t tpt = burst_tpt[static_cast<size_t>(bi)];
                    for (uint32_t r = 0; r < tpt; ++r) {
                        const int64_t a_lo = static_cast<int64_t>(j) * TILE_SIDE;
                        const int64_t b_lo = grid.base_y[static_cast<std::size_t>(j)]
                                             + static_cast<int64_t>(r) * TILE_SIDE;
                        burst_coords.push_back(TileCoord{a_lo, b_lo});
                    }
                }

                // Serialize burst_index in memory (same binary format as file)
                const uint32_t num_towers_u32 = static_cast<uint32_t>(towers_in_burst);
                const uint32_t burst_idx_bytes = static_cast<uint32_t>(
                    2 * sizeof(uint32_t) + static_cast<size_t>(towers_in_burst) * sizeof(uint32_t));
                std::vector<uint8_t> burst_idx_buf(burst_idx_bytes);
                {
                    uint8_t* p = burst_idx_buf.data();
                    std::memcpy(p, &num_towers_u32, sizeof(uint32_t)); p += sizeof(uint32_t);
                    std::memcpy(p, &burst_total_tiles, sizeof(uint32_t)); p += sizeof(uint32_t);
                    std::memcpy(p, burst_tpt.data(),
                                static_cast<size_t>(towers_in_burst) * sizeof(uint32_t));
                }

                // Serialize coords in memory (same binary format as file)
                const uint32_t coords_bytes = static_cast<uint32_t>(
                    sizeof(uint32_t) + static_cast<size_t>(burst_total_tiles) * sizeof(TileCoord));
                std::vector<uint8_t> coords_buf(coords_bytes);
                {
                    uint8_t* p = coords_buf.data();
                    std::memcpy(p, &burst_total_tiles, sizeof(uint32_t)); p += sizeof(uint32_t);
                    std::memcpy(p, burst_coords.data(),
                                static_cast<size_t>(burst_total_tiles) * sizeof(TileCoord));
                }

                // Send burst to subprocess
                if (!cuda_proc.send_burst(burst_idx_buf.data(), burst_idx_bytes,
                                          coords_buf.data(), coords_bytes,
                                          burst_total_tiles)) {
                    std::fprintf(stderr, "error: failed to send burst to CUDA stream subprocess\n");
                    cuda_proc.kill_and_reap();
                    return 1;
                }

                // Read results from subprocess
                const std::size_t output_alloc =
                    static_cast<std::size_t>(burst_total_tiles) * TILEOP_SIZE;
                std::vector<uint8_t> output_buf(output_alloc);

                const uint32_t resp_tiles = cuda_proc.recv_burst(output_buf.data());
                if (resp_tiles == 0 || resp_tiles != burst_total_tiles) {
                    std::fprintf(stderr, "error: CUDA stream burst response mismatch: "
                                 "expected %u tiles, got %u\n",
                                 burst_total_tiles, resp_tiles);
                    cuda_proc.kill_and_reap();
                    return 1;
                }

                // Feed compositor tower-by-tower from the output buffer
                uint32_t offset = 0;
                for (int32_t bi = 0; bi < towers_in_burst; ++bi) {
                    const int32_t j = first_cuda_tower + burst_start + bi;
                    const uint32_t tpt = burst_tpt[static_cast<size_t>(bi)];

                    // BUG-2 + BUG-5 FIX: overflow/malformed tile replacement
                    for (uint32_t r = 0; r < tpt; ++r) {
                        const std::size_t tile_off =
                            (static_cast<std::size_t>(offset) + r) * TILEOP_SIZE;
                        const uint8_t b0 = output_buf[tile_off];
                        const uint8_t b1 = output_buf[tile_off + 1];
                        const uint8_t b2 = output_buf[tile_off + 2];
                        bool replace = false;
                        const char* reason = nullptr;
                        if (b0 == OVERFLOW_SENTINEL) {
                            replace = true;
                            reason = "overflow";
                        } else if (b0 < TILEOP_HEADER_BYTES || b0 > b1 || b1 > b2
                                   || b2 > TILEOP_SIZE) {
                            replace = true;
                            reason = "malformed";
                        }
                        if (replace) {
                            overflow_count++;
                            std::fprintf(stderr,
                                "warning: %s tile at tower %d row %u "
                                "(CUDA stream) replaced with empty\n",
                                reason, static_cast<int>(j), r);
                            std::memset(output_buf.data() + tile_off, 0, TILEOP_SIZE);
                            output_buf[tile_off]     = EMPTY_OFFSET;
                            output_buf[tile_off + 1] = EMPTY_OFFSET;
                            output_buf[tile_off + 2] = EMPTY_OFFSET;
                        }
                    }

                    compositor.ingest_tower(j,
                        output_buf.data() + static_cast<std::size_t>(offset) * TILEOP_SIZE,
                        nullptr);
                    offset += tpt;
                    tiles_processed += tpt;

                    // Progress reporting
                    if (((j + 1) % static_cast<int32_t>(progress_interval)) == 0 ||
                        j + 1 == num_towers) {
                        const double elapsed =
                            std::chrono::duration<double>(Clock::now() - wall_start).count();
                        const double pct = 100.0 * static_cast<double>(j + 1)
                                           / static_cast<double>(num_towers);
                        std::printf("  tower %d/%d (%.1f%%) elapsed %.1fs\n",
                                    j + 1, num_towers, pct, elapsed);
                        std::fflush(stdout);
                    }

                    // Incremental spanning check
                    if (compositor.check_spanning_incremental()) {
                        if (!spanning_found) {
                            compositor.collect_outer_boundary(j);
                            spanning_found = true;
                            spanning_tower = j;
                        }
                        if (!no_early_exit) break;
                    }
                }
            }

            // Shut down the persistent subprocess
            int cuda_exit = cuda_proc.shutdown();
            if (cuda_exit != 0) {
                std::fprintf(stderr, "warning: CUDA stream subprocess exited with status %d\n",
                             cuda_exit);
            }

          } else {
            // =============================================================
            // Legacy CUDA burst path: file-based, one process per burst.
            // =============================================================
            for (int32_t burst_start = 0;
                 burst_start < remaining_towers && (!spanning_found || no_early_exit);
                 burst_start += static_cast<int32_t>(burst_size)) {

                const int32_t towers_in_burst = std::min(
                    static_cast<int32_t>(burst_size),
                    remaining_towers - burst_start);

                // Build tiles_per_tower and coords for this burst
                std::vector<uint32_t> burst_tpt(static_cast<size_t>(towers_in_burst));
                uint32_t burst_total_tiles = 0;
                for (int32_t bi = 0; bi < towers_in_burst; ++bi) {
                    const int32_t j = first_cuda_tower + burst_start + bi;
                    burst_tpt[static_cast<size_t>(bi)] =
                        grid.tiles_per_tower[static_cast<std::size_t>(j)];
                    burst_total_tiles += burst_tpt[static_cast<size_t>(bi)];
                }

                // Build coords array (tower-major order)
                std::vector<TileCoord> burst_coords;
                burst_coords.reserve(burst_total_tiles);
                for (int32_t bi = 0; bi < towers_in_burst; ++bi) {
                    const int32_t j = first_cuda_tower + burst_start + bi;
                    const uint32_t tpt = burst_tpt[static_cast<size_t>(bi)];
                    for (uint32_t r = 0; r < tpt; ++r) {
                        const int64_t a_lo = static_cast<int64_t>(j) * TILE_SIDE;
                        const int64_t b_lo = grid.base_y[static_cast<std::size_t>(j)]
                                             + static_cast<int64_t>(r) * TILE_SIDE;
                        burst_coords.push_back(TileCoord{a_lo, b_lo});
                    }
                }

                // Write temp files
                std::string burst_idx_path = make_temp_path("campaign_burst_idx");
                std::string coords_path = make_temp_path("campaign_coords");
                std::string output_path = make_temp_path("campaign_output");

                // BUG-4 FIX: RAII cleanup lambda ensures all temp files are
                // unlinked on every exit path (error returns, break, continue).
                // unlink on a non-existent path is harmless (returns -1, errno=ENOENT).
                auto cleanup_temps = [&]() {
                    ::unlink(burst_idx_path.c_str());
                    ::unlink(coords_path.c_str());
                    ::unlink(output_path.c_str());
                };

                if (!write_burst_index(burst_idx_path.c_str(), burst_tpt,
                                       static_cast<uint32_t>(towers_in_burst),
                                       burst_total_tiles)) {
                    std::fprintf(stderr, "error: failed to write burst index to %s\n",
                                 burst_idx_path.c_str());
                    cleanup_temps();
                    return 1;
                }
                if (!write_coords_file(coords_path.c_str(), burst_coords, burst_total_tiles)) {
                    std::fprintf(stderr, "error: failed to write coords to %s\n",
                                 coords_path.c_str());
                    cleanup_temps();
                    return 1;
                }

                // Shell out to CUDA binary
                char cmd_buf[2048];
                std::snprintf(cmd_buf, sizeof(cmd_buf), "%s campaign %s %s %s",
                              resolved_cuda_binary.c_str(),
                              burst_idx_path.c_str(),
                              coords_path.c_str(),
                              output_path.c_str());

                int exit_code = std::system(cmd_buf);
                if (exit_code != 0) {
                    std::fprintf(stderr, "error: CUDA binary exited with code %d\n"
                                         "  command: %s\n", exit_code, cmd_buf);
                    cleanup_temps();
                    return 1;
                }

                // Read output — burst_total_tiles * 256 bytes of raw TileOps
                const std::size_t output_bytes =
                    static_cast<std::size_t>(burst_total_tiles) * TILEOP_SIZE;
                std::vector<uint8_t> output_buf(output_bytes);

                if (!read_raw_tileops(output_path.c_str(), output_buf.data(), burst_total_tiles)) {
                    std::fprintf(stderr, "error: failed to read CUDA output from %s "
                                         "(expected %zu bytes)\n",
                                 output_path.c_str(), output_bytes);
                    cleanup_temps();
                    return 1;
                }

                // Clean up temp files
                cleanup_temps();

                // Feed compositor tower-by-tower from the output buffer
                uint32_t offset = 0;
                for (int32_t bi = 0; bi < towers_in_burst; ++bi) {
                    const int32_t j = first_cuda_tower + burst_start + bi;
                    const uint32_t tpt = burst_tpt[static_cast<size_t>(bi)];

                    // Check for overflow sentinels and malformed TileOps in this
                    // tower and replace with empty (BUG-2 + BUG-5 FIX).
                    // BUG-5: CUDA kernel at K_SQ=36 occasionally produces tiles
                    // where the offset triple (off_I, off_L, off_R) violates the
                    // ordering invariant off_I <= off_L <= off_R.  These are
                    // conservatively replaced with empty tiles.
                    for (uint32_t r = 0; r < tpt; ++r) {
                        const std::size_t tile_off =
                            (static_cast<std::size_t>(offset) + r) * TILEOP_SIZE;
                        const uint8_t b0 = output_buf[tile_off];
                        const uint8_t b1 = output_buf[tile_off + 1];
                        const uint8_t b2 = output_buf[tile_off + 2];
                        bool replace = false;
                        const char* reason = nullptr;
                        if (b0 == OVERFLOW_SENTINEL) {
                            replace = true;
                            reason = "overflow";
                        } else if (b0 < TILEOP_HEADER_BYTES || b0 > b1 || b1 > b2
                                   || b2 > TILEOP_SIZE) {
                            replace = true;
                            reason = "malformed";
                        }
                        if (replace) {
                            overflow_count++;
                            std::fprintf(stderr,
                                "warning: %s tile at tower %d row %u "
                                "(CUDA burst) replaced with empty\n",
                                reason, static_cast<int>(j), r);
                            std::memset(output_buf.data() + tile_off, 0, TILEOP_SIZE);
                            // Dead-tile signature: [EMPTY_OFFSET, EMPTY_OFFSET, EMPTY_OFFSET, 0]
                            output_buf[tile_off]     = EMPTY_OFFSET;
                            output_buf[tile_off + 1] = EMPTY_OFFSET;
                            output_buf[tile_off + 2] = EMPTY_OFFSET;
                        }
                    }

                    compositor.ingest_tower(j,
                        output_buf.data() + static_cast<std::size_t>(offset) * TILEOP_SIZE,
                        nullptr);
                    offset += tpt;
                    tiles_processed += tpt;

                    // Progress reporting
                    if (((j + 1) % static_cast<int32_t>(progress_interval)) == 0 ||
                        j + 1 == num_towers) {
                        const double elapsed =
                            std::chrono::duration<double>(Clock::now() - wall_start).count();
                        const double pct = 100.0 * static_cast<double>(j + 1)
                                           / static_cast<double>(num_towers);
                        std::printf("  tower %d/%d (%.1f%%) elapsed %.1fs\n",
                                    j + 1, num_towers, pct, elapsed);
                        std::fflush(stdout);
                    }

                    // Incremental spanning check
                    if (compositor.check_spanning_incremental()) {
                        if (!spanning_found) {
                            compositor.collect_outer_boundary(j);
                            spanning_found = true;
                            spanning_tower = j;
                        }
                        if (!no_early_exit) break;
                    }
                }
            }
          }  // use_cuda_stream vs legacy
        }
    } else {
        // =====================================================================
        // C++ tile path (original behavior, unchanged)
        // =====================================================================
        for (int32_t j = 0; j < num_towers; ++j) {
            const uint32_t tpt = grid.tiles_per_tower[static_cast<std::size_t>(j)];
            const std::size_t buf_size = static_cast<std::size_t>(tpt) * TILEOP_SIZE;

            // Allocate tower tileop buffer
            std::vector<uint8_t> tower_buf(buf_size, 0);

            bool tower_has_overflow = false;

            // Process each tile in the tower
            for (uint32_t r = 0; r < tpt; ++r) {
                const int64_t a_lo = static_cast<int64_t>(j) * TILE_SIDE;
                const int64_t b_lo = grid.base_y[static_cast<std::size_t>(j)]
                                     + static_cast<int64_t>(r) * TILE_SIDE;

                TileCoord coord{a_lo, b_lo};
                TileResult result = process_tile(coord, tables);

                // Copy tileop bytes into tower buffer
                std::memcpy(tower_buf.data() + static_cast<std::size_t>(r) * TILEOP_SIZE,
                            result.tileop.bytes, TILEOP_SIZE);

                // Check for overflow sentinel (first byte == 0xFF)
                if (result.tileop.bytes[0] == OVERFLOW_SENTINEL) {
                    tower_has_overflow = true;
                    overflow_count++;
                }
            }

            // --- Overflow handling ---
            // BUG-2 FIX: Replace overflow tiles with empty TileOps before
            // feeding to compositor. The compositor calls assert_not_overflow()
            // on every tile and aborts on overflow sentinels. Empty TileOps
            // (off_I = EMPTY_OFFSET) have no groups on any face and are handled
            // correctly as tiles with no connectivity.
            if (tower_has_overflow) {
                std::fprintf(stderr,
                    "warning: tower %d contains overflow tile(s) — "
                    "replacing with empty TileOps (extended reprocessing not yet implemented)\n",
                    static_cast<int>(j));
                for (uint32_t r = 0; r < tpt; ++r) {
                    if (tower_buf[static_cast<std::size_t>(r) * TILEOP_SIZE] == OVERFLOW_SENTINEL) {
                        const int64_t a_lo = static_cast<int64_t>(j) * TILE_SIDE;
                        const int64_t b_lo = grid.base_y[static_cast<std::size_t>(j)]
                                             + static_cast<int64_t>(r) * TILE_SIDE;
                        std::fprintf(stderr,
                            "  overflow tile at tower %d row %u (a=%" PRId64 " b=%" PRId64
                            ") replaced with empty\n",
                            static_cast<int>(j), r, a_lo, b_lo);
                        std::memset(tower_buf.data() + static_cast<std::size_t>(r) * TILEOP_SIZE,
                                    0, TILEOP_SIZE);
                        tower_buf[static_cast<std::size_t>(r) * TILEOP_SIZE] = EMPTY_OFFSET;
                    }
                }
            }

            // --- Feed to compositor ---
            compositor.ingest_tower(j, tower_buf.data(), nullptr);

            tiles_processed += tpt;

            // --- Progress reporting ---
            if (((j + 1) % static_cast<int32_t>(progress_interval)) == 0 ||
                j + 1 == num_towers) {
                const double elapsed =
                    std::chrono::duration<double>(Clock::now() - wall_start).count();
                const double pct = 100.0 * static_cast<double>(j + 1) / static_cast<double>(num_towers);
                std::printf("  tower %d/%d (%.1f%%) elapsed %.1fs\n",
                            j + 1, num_towers, pct, elapsed);
                std::fflush(stdout);
            }

            // --- Incremental spanning check ---
            if (compositor.check_spanning_incremental()) {
                if (!spanning_found) {
                    compositor.collect_outer_boundary(j);
                    spanning_found = true;
                    spanning_tower = j;
                }
                if (!no_early_exit) break;
            }
        }
    }

    // --- Finalize ---
    if (!spanning_found || no_early_exit) {
        // Collect outer boundary of the last tower
        compositor.collect_outer_boundary(static_cast<int32_t>(num_towers - 1));
    }
    const CompositorResult result = compositor.finalize();
    const Clock::time_point wall_end = Clock::now();
    const double wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();

    const int32_t towers_processed = (spanning_found && !no_early_exit) ? (spanning_tower + 1) : num_towers;

    // --- Output ---
    if (mode == OutputMode::DUMP || mode == OutputMode::DUMP_WITH_STATS) {
        // TODO: Implement dump and dump_with_stats output modes.
        // dump: write all TileOp bytes to a binary file for offline analysis.
        // dump_with_stats: same binary + per-tower statistics (group counts, etc).
        std::fprintf(stderr, "warning: mode dump/dump_with_stats not yet implemented, "
                             "falling back to verdict_only\n");
    }

    std::printf("\n=== RESULT ===\n");
    std::printf("{\n");
    std::printf("  \"R\": %" PRId64 ",\n", R);
    std::printf("  \"K_SQ\": %" PRIu32 ",\n", k_sq);
    std::printf("  \"verdict\": \"%s\",\n", verdict_string(result.verdict));
    std::printf("  \"num_towers\": %d,\n", num_towers);
    std::printf("  \"total_tiles\": %" PRIu64 ",\n", total_tiles);
    std::printf("  \"towers_processed\": %d,\n", towers_processed);
    std::printf("  \"tiles_processed\": %" PRIu64 ",\n", tiles_processed);
    std::printf("  \"wall_time_seconds\": %.3f,\n", wall_seconds);
    std::printf("  \"overflow_count\": %" PRIu64 ",\n", overflow_count);
    std::printf("  \"total_groups\": %" PRIu32 ",\n", result.total_groups);
    std::printf("  \"inner_roots\": %" PRIu32 ",\n", result.inner_root_count);
    std::printf("  \"outer_roots\": %" PRIu32 ",\n", result.outer_root_count);
    std::printf("  \"peak_rss_mb\": %.1f\n", peak_rss_mb());
    std::printf("}\n");

    // Exit code: 0 for SPANNING, 1 for MOAT
    return result.verdict == CompositorResult::SPANNING ? 0 : 1;
}
