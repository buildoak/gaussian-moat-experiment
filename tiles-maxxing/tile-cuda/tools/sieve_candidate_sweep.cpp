// sieve_candidate_sweep.cpp
// Standalone tool: counts sieve survivors (candidates before MR) for tiles
// across the full operating range. Replicates the CUDA kernel's sieve logic
// exactly, without running Miller-Rabin.
//
// Usage: ./sieve_candidate_sweep [R] [num_towers] [tiles_per_tower]
// Defaults: R=860000000, num_towers=3383 (full octant), tiles_per_tower=32
//
// Build: c++ -O3 -march=native -std=c++17 -o sieve_candidate_sweep sieve_candidate_sweep.cpp
//
// Output: per-tile CSV on stdout, summary stats on stderr.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>

// --- Constants matching CUDA kernel ---
constexpr int32_t TILE_SIDE = 256;
constexpr int32_t COLLAR = 7;
constexpr int32_t TILE_POINTS = TILE_SIDE + 1;
constexpr int32_t SIDE_EXP = TILE_POINTS + 2 * COLLAR;  // 271
constexpr uint32_t SIEVE_LIMIT = 10000U;
constexpr int SPLIT_PRIMES_COUNT = 609;
constexpr int INERT_PRIMES_COUNT = 619;
constexpr int BITMAP_WORDS_PER_ROW = 9;
constexpr int LAST_WORD_VALID_BITS = 15;
constexpr uint32_t LAST_WORD_MASK = (1u << LAST_WORD_VALID_BITS) - 1u;
constexpr int ACTIVE_ROWS = 271;

struct TileCoord {
    int64_t a_lo;
    int64_t b_lo;
};

struct SieveTables {
    uint32_t split_table[SPLIT_PRIMES_COUNT];
    uint16_t inert_primes[INERT_PRIMES_COUNT];
    int split_count;
    int inert_count;
};

// --- Math utilities ---

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

bool init_sieve_tables(SieveTables& tables) {
    std::memset(&tables, 0, sizeof(tables));

    uint8_t is_prime_table[SIEVE_LIMIT + 1];
    std::memset(is_prime_table, 1, sizeof(is_prime_table));
    is_prime_table[0] = 0U;
    is_prime_table[1] = 0U;

    for (uint32_t p = 2U; p * p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) continue;
        for (uint32_t multiple = p * p; multiple <= SIEVE_LIMIT; multiple += p) {
            is_prime_table[multiple] = 0U;
        }
    }

    for (uint32_t p = 2U; p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) continue;

        if ((p & 3U) == 1U) {
            const uint64_t root_raw = fast_sqrt_neg1(static_cast<uint64_t>(p));
            if (root_raw == std::numeric_limits<uint64_t>::max()) return false;

            uint64_t root = root_raw;
            const uint64_t neg_root = static_cast<uint64_t>(p) - root;
            if (neg_root < root) root = neg_root;

            if (mulmod_small(root, root, static_cast<uint64_t>(p)) != static_cast<uint64_t>(p - 1U))
                return false;
            if (tables.split_count >= SPLIT_PRIMES_COUNT) return false;

            tables.split_table[tables.split_count++] =
                (static_cast<uint32_t>(root) << 16) | p;
        } else if ((p & 3U) == 3U) {
            if (tables.inert_count >= INERT_PRIMES_COUNT) return false;
            tables.inert_primes[tables.inert_count++] = static_cast<uint16_t>(p);
        }
    }

    return tables.split_count == SPLIT_PRIMES_COUNT &&
           tables.inert_count == INERT_PRIMES_COUNT;
}

// --- Sieve logic matching CUDA kernel exactly ---

inline int32_t euclidean_mod(int32_t value, uint32_t modulus) {
    const int32_t mod = static_cast<int32_t>(modulus);
    int32_t rem = value % mod;
    if (rem < 0) rem += mod;
    return rem;
}

inline void mark_residue_class_reg(uint32_t ws[BITMAP_WORDS_PER_ROW],
                                    int32_t b_start, uint32_t p, int32_t residue) {
    const int32_t b_mod = euclidean_mod(b_start, p);
    const int32_t first_col = euclidean_mod(residue - b_mod, p);
    for (int32_t col = first_col; col < SIDE_EXP; col += static_cast<int32_t>(p)) {
        ws[col >> 5] |= 1u << (col & 31);
    }
}

// Sieve one row, return survivor count. Matches gpu_sieve.cuh::sieve_row exactly.
uint32_t sieve_row_count(int32_t a, int32_t b_start, const SieveTables& tables) {
    uint32_t ws[BITMAP_WORDS_PER_ROW];

    // Step 1: parity elimination
    const uint32_t pattern = ((a ^ b_start) & 1) != 0 ? 0xAAAAAAAAu : 0x55555555u;
    for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
        ws[w] = pattern;
    }
    ws[BITMAP_WORDS_PER_ROW - 1] &= LAST_WORD_MASK;

    // Step 2: split primes
    for (int k = 0; k < tables.split_count; ++k) {
        const uint32_t packed = tables.split_table[k];
        const uint32_t p = packed & 0xFFFFu;
        const uint32_t root = packed >> 16;
        const int32_t residue = static_cast<int32_t>(
            (static_cast<int64_t>(euclidean_mod(a, p)) * static_cast<int64_t>(root)) %
            static_cast<int64_t>(p));
        mark_residue_class_reg(ws, b_start, p, residue);

        const int32_t neg_res = euclidean_mod(-residue, p);
        if (neg_res != residue) {
            mark_residue_class_reg(ws, b_start, p, neg_res);
        }
    }

    // Step 3: inert primes
    for (int k = 0; k < tables.inert_count; ++k) {
        const uint32_t p = static_cast<uint32_t>(tables.inert_primes[k]);
        if (euclidean_mod(a, p) == 0) {
            mark_residue_class_reg(ws, b_start, p, 0);
        }
    }

    // Count survivors: bits NOT set in ws
    uint32_t count = 0;
    for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
        uint32_t survivors = ~ws[w];
        if (w == (BITMAP_WORDS_PER_ROW - 1)) {
            survivors &= LAST_WORD_MASK;
        }
#if defined(__GNUC__) || defined(__clang__)
        count += static_cast<uint32_t>(__builtin_popcount(survivors));
#else
        while (survivors) { survivors &= survivors - 1; ++count; }
#endif
    }
    return count;
}

// Count total sieve candidates for a tile (sum across 271 rows).
// This is exactly what the CUDA kernel computes as total_cands.
uint32_t count_tile_candidates(const TileCoord& coord, const SieveTables& tables) {
    const int32_t a_start = static_cast<int32_t>(coord.a_lo - static_cast<int64_t>(COLLAR));
    const int32_t b_start = static_cast<int32_t>(coord.b_lo - static_cast<int64_t>(COLLAR));

    uint32_t total = 0;
    for (int row = 0; row < ACTIVE_ROWS; ++row) {
        const int32_t a = a_start + row;
        total += sieve_row_count(a, b_start, tables);
    }
    return total;
}

// --- Tile coordinate generation matching the grid spec ---

struct SweepResult {
    int64_t a_lo;
    int64_t b_lo;
    uint32_t candidates;
};

int main(int argc, char** argv) {
    int64_t R = 860'000'000LL;
    int num_towers = 0;  // 0 = full octant
    int tiles_per_tower = 32;
    bool sample_mode = false;
    int sample_count = 0;
    int64_t R_min = 0;
    int64_t R_max = 0;

    // Parse CLI
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--sample") == 0 && i + 1 < argc) {
            sample_mode = true;
            sample_count = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--range") == 0 && i + 2 < argc) {
            R_min = std::atoll(argv[++i]);
            R_max = std::atoll(argv[++i]);
        } else if (std::strcmp(argv[i], "--towers") == 0 && i + 1 < argc) {
            num_towers = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--tpt") == 0 && i + 1 < argc) {
            tiles_per_tower = std::atoi(argv[++i]);
        } else {
            R = std::atoll(argv[i]);
        }
    }

    // Init sieve tables
    SieveTables tables;
    if (!init_sieve_tables(tables)) {
        std::fprintf(stderr, "error: init_sieve_tables failed\n");
        return 1;
    }

    // Generate tile coordinates
    std::vector<TileCoord> coords;

    if (R_min > 0 && R_max > R_min) {
        // Range sweep: generate tiles at multiple radii
        std::fprintf(stderr, "=== Range sweep R=%lld to R=%lld ===\n",
                     static_cast<long long>(R_min), static_cast<long long>(R_max));

        // Sample at each radius: a few towers near 45 degrees (worst case for density)
        for (int64_t r = R_min; r <= R_max; r += 50'000'000LL) {
            const double R_d = static_cast<double>(r);
            // Near 45 degrees: a ~= b ~= R/sqrt(2)
            const int64_t a_base = static_cast<int64_t>(R_d / std::sqrt(2.0));
            const int64_t a_aligned = (a_base / TILE_SIDE) * TILE_SIDE;

            for (int dj = -5; dj <= 5; ++dj) {
                const int64_t a_lo = a_aligned + dj * TILE_SIDE;
                if (a_lo < 0) continue;

                const double inner = R_d * R_d - static_cast<double>(a_lo) * static_cast<double>(a_lo);
                if (inner <= 0.0) continue;
                int64_t b_raw = static_cast<int64_t>(std::sqrt(inner));
                int64_t b_lo_base = (b_raw / TILE_SIDE) * TILE_SIDE;

                for (int k = 0; k < tiles_per_tower; ++k) {
                    coords.push_back(TileCoord{a_lo, b_lo_base + static_cast<int64_t>(k) * TILE_SIDE});
                }
            }

            // Also near axis (a ~= 0): usually sparser but let's check
            for (int dj = 0; dj < 5; ++dj) {
                const int64_t a_lo = static_cast<int64_t>(dj) * TILE_SIDE;
                const double inner = R_d * R_d - static_cast<double>(a_lo) * static_cast<double>(a_lo);
                if (inner <= 0.0) continue;
                int64_t b_raw = static_cast<int64_t>(std::sqrt(inner));
                int64_t b_lo_base = (b_raw / TILE_SIDE) * TILE_SIDE;

                for (int k = 0; k < tiles_per_tower; ++k) {
                    coords.push_back(TileCoord{a_lo, b_lo_base + static_cast<int64_t>(k) * TILE_SIDE});
                }
            }
        }
    } else {
        // Single radius mode
        const double R_d = static_cast<double>(R);

        // Compute full octant tower count if not specified
        if (num_towers <= 0) {
            // Octant: towers from j=0 until base_x >= base_y (past 45 degrees)
            num_towers = static_cast<int>(R_d / (std::sqrt(2.0) * TILE_SIDE)) + 10;
        }

        std::fprintf(stderr, "=== Sieve candidate sweep ===\n");
        std::fprintf(stderr, "R=%lld, towers=%d, tiles_per_tower=%d\n",
                     static_cast<long long>(R), num_towers, tiles_per_tower);

        for (int j = 0; j < num_towers; ++j) {
            const int64_t a_lo = static_cast<int64_t>(j) * TILE_SIDE;
            const double a_lo_d = static_cast<double>(a_lo);
            const double inner = R_d * R_d - a_lo_d * a_lo_d;
            int64_t b_raw;
            if (inner <= 0.0) {
                b_raw = 0;
            } else {
                b_raw = static_cast<int64_t>(std::sqrt(inner));
            }
            const int64_t b_lo_base = (b_raw / TILE_SIDE) * TILE_SIDE;

            if (a_lo >= b_lo_base && j > 0) {
                std::fprintf(stderr, "Octant boundary at tower %d (a_lo=%lld >= b_lo_base=%lld)\n",
                             j, static_cast<long long>(a_lo), static_cast<long long>(b_lo_base));
                break;
            }

            for (int k = 0; k < tiles_per_tower; ++k) {
                coords.push_back(TileCoord{a_lo, b_lo_base + static_cast<int64_t>(k) * TILE_SIDE});
            }
        }
    }

    const int total_tiles = static_cast<int>(coords.size());
    std::fprintf(stderr, "Total tiles to sweep: %d\n", total_tiles);

    // Process tiles in parallel
    std::vector<SweepResult> results(total_tiles);
    std::atomic<int> progress{0};
    const unsigned hw_threads = std::thread::hardware_concurrency();
    const int num_threads = std::max(1U, hw_threads);

    auto start_time = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = t; i < total_tiles; i += num_threads) {
                results[i].a_lo = coords[i].a_lo;
                results[i].b_lo = coords[i].b_lo;
                results[i].candidates = count_tile_candidates(coords[i], tables);

                int p = progress.fetch_add(1) + 1;
                if (p % 10000 == 0) {
                    std::fprintf(stderr, "  %d / %d tiles (%.1f%%)\n",
                                 p, total_tiles, 100.0 * p / total_tiles);
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        end_time - start_time).count() / 1.0e6;

    // Compute statistics
    std::vector<uint32_t> all_counts(total_tiles);
    uint32_t global_max = 0;
    int64_t max_a = 0, max_b = 0;
    uint64_t sum = 0;

    for (int i = 0; i < total_tiles; ++i) {
        all_counts[i] = results[i].candidates;
        sum += results[i].candidates;
        if (results[i].candidates > global_max) {
            global_max = results[i].candidates;
            max_a = results[i].a_lo;
            max_b = results[i].b_lo;
        }
    }

    std::sort(all_counts.begin(), all_counts.end());

    double mean = static_cast<double>(sum) / total_tiles;
    uint32_t median = all_counts[total_tiles / 2];
    uint32_t min_val = all_counts[0];
    uint32_t p90 = all_counts[static_cast<size_t>(total_tiles * 0.90)];
    uint32_t p95 = all_counts[static_cast<size_t>(total_tiles * 0.95)];
    uint32_t p99 = all_counts[static_cast<size_t>(total_tiles * 0.99)];
    uint32_t p999 = all_counts[static_cast<size_t>(total_tiles * 0.999)];
    uint32_t p9999 = all_counts[std::min(static_cast<size_t>(total_tiles * 0.9999),
                                          static_cast<size_t>(total_tiles - 1))];

    // Print CSV header + data to stdout
    std::printf("a_lo,b_lo,candidates\n");
    for (int i = 0; i < total_tiles; ++i) {
        std::printf("%lld,%lld,%u\n",
                    static_cast<long long>(results[i].a_lo),
                    static_cast<long long>(results[i].b_lo),
                    results[i].candidates);
    }

    // Print summary to stderr
    std::fprintf(stderr, "\n=== Sieve Candidate Count Summary ===\n");
    std::fprintf(stderr, "Tiles processed: %d\n", total_tiles);
    std::fprintf(stderr, "Wall time: %.1fs (%.0f tiles/sec)\n",
                 elapsed, total_tiles / elapsed);
    std::fprintf(stderr, "Threads: %d\n\n", num_threads);
    std::fprintf(stderr, "  Min:      %u\n", min_val);
    std::fprintf(stderr, "  Mean:     %.1f\n", mean);
    std::fprintf(stderr, "  Median:   %u\n", median);
    std::fprintf(stderr, "  P90:      %u\n", p90);
    std::fprintf(stderr, "  P95:      %u\n", p95);
    std::fprintf(stderr, "  P99:      %u\n", p99);
    std::fprintf(stderr, "  P99.9:    %u\n", p999);
    std::fprintf(stderr, "  P99.99:   %u\n", p9999);
    std::fprintf(stderr, "  Max:      %u  at (%lld, %lld)\n",
                 global_max, static_cast<long long>(max_a), static_cast<long long>(max_b));

    // Count tiles exceeding various thresholds
    for (uint32_t threshold : {5000u, 5500u, 6000u, 6500u, 7000u, 7168u, 7500u, 8000u, 8192u}) {
        int count = 0;
        for (int i = 0; i < total_tiles; ++i) {
            if (results[i].candidates > threshold) ++count;
        }
        std::fprintf(stderr, "  Tiles > %u: %d (%.4f%%)\n",
                     threshold, count, 100.0 * count / total_tiles);
    }

    // Histogram: 500-wide bins
    std::fprintf(stderr, "\nHistogram (500-wide bins):\n");
    int max_bin = (global_max / 500 + 1) * 500;
    for (int lo = 0; lo < max_bin; lo += 500) {
        int count = 0;
        for (auto c : all_counts) {
            if (c >= static_cast<uint32_t>(lo) && c < static_cast<uint32_t>(lo + 500)) ++count;
        }
        if (count > 0) {
            std::fprintf(stderr, "  [%5d, %5d): %d\n", lo, lo + 500, count);
        }
    }

    return 0;
}
