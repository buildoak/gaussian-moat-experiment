// sieve_worst_case.cpp
// Exhaustive search for the worst-case (maximum) sieve candidate count.
//
// Key insight: the sieve candidate count depends only on the residues
// of (a_lo, b_lo) modulo the sieve primes. Since all tiles are aligned
// to 256-byte boundaries, we need to search all residues of (a_lo/256, b_lo/256)
// modulo the LCM of the sieve primes divided by gcd with 256.
//
// However, the actual sieve processes 271 rows starting at a_start = a_lo - 7,
// and 271 columns starting at b_start = b_lo - 7. The residue of a_start mod p
// varies row by row (a_start, a_start+1, ..., a_start+270), so the candidate
// count is a function of a_start mod p and b_start mod p for EACH prime p.
//
// For a given (a_start mod p, b_start mod p) across all sieve primes p, the
// candidate count is fully determined. But the number of such combinations is
// huge. Instead, we do a large brute-force sweep covering diverse residues.
//
// Strategy: sweep a_lo from 0 to some_bound (step 256) and b_lo from 0 to some_bound
// (step 256). This covers all residue classes mod every prime < 10000, since 256
// and the primes are coprime (primes > 2, 256 = 2^8).
//
// Build: c++ -O3 -march=native -std=c++17 -o sieve_worst_case sieve_worst_case.cpp -lpthread

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

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

struct SieveTables {
    uint32_t split_table[SPLIT_PRIMES_COUNT];
    uint16_t inert_primes[INERT_PRIMES_COUNT];
    int split_count;
    int inert_count;
};

uint64_t mulmod_small(uint64_t a, uint64_t b, uint64_t m) {
    return (a * b) % m;
}

uint64_t fast_sqrt_neg1(uint64_t p) {
    for (uint64_t x = 1; x < p; ++x) {
        if (mulmod_small(x, x, p) == (p - 1ULL)) return x;
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
        for (uint32_t m = p * p; m <= SIEVE_LIMIT; m += p) is_prime_table[m] = 0U;
    }
    for (uint32_t p = 2U; p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) continue;
        if ((p & 3U) == 1U) {
            uint64_t root_raw = fast_sqrt_neg1(static_cast<uint64_t>(p));
            if (root_raw == std::numeric_limits<uint64_t>::max()) return false;
            uint64_t root = root_raw;
            uint64_t neg_root = p - root;
            if (neg_root < root) root = neg_root;
            if (mulmod_small(root, root, p) != p - 1ULL) return false;
            if (tables.split_count >= SPLIT_PRIMES_COUNT) return false;
            tables.split_table[tables.split_count++] = (static_cast<uint32_t>(root) << 16) | p;
        } else if ((p & 3U) == 3U) {
            if (tables.inert_count >= INERT_PRIMES_COUNT) return false;
            tables.inert_primes[tables.inert_count++] = static_cast<uint16_t>(p);
        }
    }
    return tables.split_count == SPLIT_PRIMES_COUNT && tables.inert_count == INERT_PRIMES_COUNT;
}

inline int32_t euclidean_mod(int32_t value, uint32_t modulus) {
    int32_t mod = static_cast<int32_t>(modulus);
    int32_t rem = value % mod;
    if (rem < 0) rem += mod;
    return rem;
}

inline void mark_residue_class_reg(uint32_t ws[BITMAP_WORDS_PER_ROW],
                                    int32_t b_start, uint32_t p, int32_t residue) {
    int32_t b_mod = euclidean_mod(b_start, p);
    int32_t first_col = euclidean_mod(residue - b_mod, p);
    for (int32_t col = first_col; col < SIDE_EXP; col += static_cast<int32_t>(p)) {
        ws[col >> 5] |= 1u << (col & 31);
    }
}

uint32_t sieve_row_count(int32_t a, int32_t b_start, const SieveTables& tables) {
    uint32_t ws[BITMAP_WORDS_PER_ROW];
    uint32_t pattern = ((a ^ b_start) & 1) != 0 ? 0xAAAAAAAAu : 0x55555555u;
    for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) ws[w] = pattern;
    ws[BITMAP_WORDS_PER_ROW - 1] &= LAST_WORD_MASK;

    for (int k = 0; k < tables.split_count; ++k) {
        uint32_t packed = tables.split_table[k];
        uint32_t p = packed & 0xFFFFu;
        uint32_t root = packed >> 16;
        int32_t residue = static_cast<int32_t>(
            (static_cast<int64_t>(euclidean_mod(a, p)) * static_cast<int64_t>(root)) %
            static_cast<int64_t>(p));
        mark_residue_class_reg(ws, b_start, p, residue);
        int32_t neg_res = euclidean_mod(-residue, p);
        if (neg_res != residue) mark_residue_class_reg(ws, b_start, p, neg_res);
    }

    for (int k = 0; k < tables.inert_count; ++k) {
        uint32_t p = static_cast<uint32_t>(tables.inert_primes[k]);
        if (euclidean_mod(a, p) == 0) mark_residue_class_reg(ws, b_start, p, 0);
    }

    uint32_t count = 0;
    for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
        uint32_t surv = ~ws[w];
        if (w == BITMAP_WORDS_PER_ROW - 1) surv &= LAST_WORD_MASK;
        count += static_cast<uint32_t>(__builtin_popcount(surv));
    }
    return count;
}

uint32_t count_tile_candidates(int64_t a_lo, int64_t b_lo, const SieveTables& tables) {
    int32_t a_start = static_cast<int32_t>(a_lo - COLLAR);
    int32_t b_start = static_cast<int32_t>(b_lo - COLLAR);
    uint32_t total = 0;
    for (int row = 0; row < ACTIVE_ROWS; ++row) {
        total += sieve_row_count(a_start + row, b_start, tables);
    }
    return total;
}

int main(int argc, char** argv) {
    // The largest sieve prime is 9973 (split) or 9967 (inert).
    // To cover all residue classes of a_lo mod p for the largest prime,
    // we need a_lo to take values 0, 256, 512, ..., up to at least 9973*256.
    // Similarly for b_lo. That's ~10000 values per axis = ~100M tiles. Too many.
    //
    // Better approach: the ACTUAL period for the candidate count function
    // w.r.t. a_lo is lcm(256, p1, p2, ...) -- but we only need to cover
    // each prime's residues independently. Since the candidate count is a SUM
    // over rows, and each row's contribution depends on (a_start + row) mod p,
    // the worst case happens when multiple primes simultaneously have "bad"
    // residues that minimize their sieving effect.
    //
    // Practical approach: dense sweep of a limited but large range.
    // 10000 x 10000 tiles = 100M -- too slow.
    // 1000 x 1000 = 1M tiles at ~7K/sec = 2.4 min. Doable.
    //
    // But we can be smarter: the candidate count varies by ~5% (5500 to 5900).
    // We need to find the absolute maximum. Let's do a grid search.

    int grid_size = 1000;  // tiles per axis
    if (argc > 1) grid_size = std::atoi(argv[1]);

    SieveTables tables;
    if (!init_sieve_tables(tables)) {
        std::fprintf(stderr, "error: init_sieve_tables failed\n");
        return 1;
    }

    int total = grid_size * grid_size;
    std::fprintf(stderr, "=== Worst-case sieve candidate search ===\n");
    std::fprintf(stderr, "Grid: %d x %d = %d tiles\n", grid_size, grid_size, total);
    std::fprintf(stderr, "a_lo range: [0, %lld), b_lo range: [0, %lld)\n",
                 static_cast<long long>(grid_size) * TILE_SIDE,
                 static_cast<long long>(grid_size) * TILE_SIDE);

    std::atomic<uint32_t> global_max{0};
    std::atomic<int64_t> best_a{0}, best_b{0};
    std::atomic<int> progress{0};
    std::atomic<uint64_t> global_sum{0};

    // Track top-10 for analysis
    std::mutex top_mutex;
    struct Entry { int64_t a, b; uint32_t count; };
    std::vector<Entry> top_entries;

    unsigned hw = std::thread::hardware_concurrency();
    int nthreads = std::max(1U, hw);

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < nthreads; ++t) {
        threads.emplace_back([&, t]() {
            uint32_t local_max = 0;
            int64_t local_a = 0, local_b = 0;
            std::vector<Entry> local_top;

            for (int ai = t; ai < grid_size; ai += nthreads) {
                int64_t a_lo = static_cast<int64_t>(ai) * TILE_SIDE;
                for (int bi = 0; bi < grid_size; ++bi) {
                    int64_t b_lo = static_cast<int64_t>(bi) * TILE_SIDE;
                    uint32_t c = count_tile_candidates(a_lo, b_lo, tables);
                    global_sum.fetch_add(c, std::memory_order_relaxed);

                    if (c > local_max) {
                        local_max = c;
                        local_a = a_lo;
                        local_b = b_lo;
                    }
                    if (c >= 5800) {
                        local_top.push_back({a_lo, b_lo, c});
                    }
                }

                int p = progress.fetch_add(1) + 1;
                if (p % 100 == 0) {
                    std::fprintf(stderr, "  rows: %d/%d (%.1f%%)\n",
                                 p, grid_size, 100.0 * p / grid_size);
                }
            }

            // Update global max
            uint32_t prev = global_max.load();
            while (local_max > prev && !global_max.compare_exchange_weak(prev, local_max)) {}
            if (local_max >= global_max.load()) {
                best_a.store(local_a);
                best_b.store(local_b);
            }

            // Merge top entries
            std::lock_guard<std::mutex> lock(top_mutex);
            top_entries.insert(top_entries.end(), local_top.begin(), local_top.end());
        });
    }
    for (auto& t : threads) t.join();

    auto end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        end - start).count() / 1.0e6;

    uint32_t max_count = global_max.load();
    double mean = static_cast<double>(global_sum.load()) / total;

    std::fprintf(stderr, "\n=== Results ===\n");
    std::fprintf(stderr, "Tiles:   %d\n", total);
    std::fprintf(stderr, "Time:    %.1fs (%.0f tiles/sec)\n", elapsed, total / elapsed);
    std::fprintf(stderr, "Mean:    %.1f\n", mean);
    std::fprintf(stderr, "Max:     %u at (%lld, %lld)\n", max_count,
                 static_cast<long long>(best_a.load()),
                 static_cast<long long>(best_b.load()));

    // Sort and print top entries
    std::sort(top_entries.begin(), top_entries.end(),
              [](const Entry& a, const Entry& b) { return a.count > b.count; });
    std::fprintf(stderr, "\nTop candidates (count >= 5800): %zu tiles\n", top_entries.size());
    int limit = std::min<int>(50, static_cast<int>(top_entries.size()));
    for (int i = 0; i < limit; ++i) {
        std::fprintf(stderr, "  %u at (%lld, %lld)\n",
                     top_entries[i].count,
                     static_cast<long long>(top_entries[i].a),
                     static_cast<long long>(top_entries[i].b));
    }

    return 0;
}
