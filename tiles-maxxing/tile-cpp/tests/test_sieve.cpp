#include "sieve.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {

struct Point {
    int64_t a;
    int64_t b;
};

bool brute_is_prime(uint64_t n) {
    if (n < 2ULL) {
        return false;
    }
    if (n == 2ULL) {
        return true;
    }
    if ((n & 1ULL) == 0ULL) {
        return false;
    }
    for (uint64_t d = 3ULL; d * d <= n; d += 2ULL) {
        if (n % d == 0ULL) {
            return false;
        }
    }
    return true;
}

uint64_t abs_i64_to_u64(int64_t value) {
    return value < 0
        ? static_cast<uint64_t>(-(value + 1)) + 1ULL
        : static_cast<uint64_t>(value);
}

bool brute_is_gaussian_prime(int64_t a, int64_t b) {
    if (a == 0 && b == 0) {
        return false;
    }
    if (a == 0 || b == 0) {
        const uint64_t axis = abs_i64_to_u64(a == 0 ? b : a);
        return axis != 0ULL && (axis & 3ULL) == 3ULL && brute_is_prime(axis);
    }

    const uint64_t ua = abs_i64_to_u64(a);
    const uint64_t ub = abs_i64_to_u64(b);
    const uint64_t norm = ua * ua + ub * ub;
    return brute_is_prime(norm);
}

int count_bitmap_bits(const uint32_t* bitmap) {
    int count = 0;
    for (int i = 0; i < BITMAP_WORDS; ++i) {
#if defined(__GNUC__) || defined(__clang__)
        count += __builtin_popcount(bitmap[i]);
#else
        uint32_t word = bitmap[i];
        while (word != 0U) {
            word &= (word - 1U);
            ++count;
        }
#endif
    }
    return count;
}

bool bitmap_contains(const TileCoord& coord, const uint32_t* bitmap, int64_t a, int64_t b) {
    const int64_t row = a - (coord.a_lo - static_cast<int64_t>(COLLAR));
    const int64_t col = b - (coord.b_lo - static_cast<int64_t>(COLLAR));
    if (row < 0 || row >= SIDE_EXP || col < 0 || col >= SIDE_EXP) {
        return false;
    }
    const int pos = static_cast<int>(row) * SIDE_EXP + static_cast<int>(col);
    return ((bitmap[pos >> 5] >> (pos & 31)) & 1U) != 0U;
}

bool require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        return false;
    }
    return true;
}

bool find_known_primes(const TileCoord& coord, Point* points, int target_count) {
    int found = 0;
    const int64_t a_start = coord.a_lo - static_cast<int64_t>(COLLAR);
    const int64_t b_start = coord.b_lo - static_cast<int64_t>(COLLAR);
    for (int row = 0; row < SIDE_EXP && found < target_count; ++row) {
        const int64_t a = a_start + row;
        for (int col = 0; col < SIDE_EXP && found < target_count; ++col) {
            const int64_t b = b_start + col;
            if (!brute_is_gaussian_prime(a, b)) {
                continue;
            }
            points[found].a = a;
            points[found].b = b;
            ++found;
        }
    }
    return found == target_count;
}

int brute_count_tile(const TileCoord& coord) {
    int count = 0;
    const int64_t a_start = coord.a_lo - static_cast<int64_t>(COLLAR);
    const int64_t b_start = coord.b_lo - static_cast<int64_t>(COLLAR);
    for (int row = 0; row < SIDE_EXP; ++row) {
        const int64_t a = a_start + row;
        for (int col = 0; col < SIDE_EXP; ++col) {
            const int64_t b = b_start + col;
            if (brute_is_gaussian_prime(a, b)) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace

int main() {
    SieveTables tables;
    if (!require(init_sieve_tables(tables), "init_sieve_tables failed")) {
        return 1;
    }
    if (!require(tables.split_count == SPLIT_PRIMES_COUNT, "split prime count mismatch")) {
        return 1;
    }
    if (!require(tables.inert_count == INERT_PRIMES_COUNT, "inert prime count mismatch")) {
        return 1;
    }

    uint32_t bitmap_a[BITMAP_WORDS];
    uint32_t bitmap_b[BITMAP_WORDS];
    uint32_t axis_bitmap[BITMAP_WORDS];
    std::memset(bitmap_a, 0, sizeof(bitmap_a));
    std::memset(bitmap_b, 0, sizeof(bitmap_b));
    std::memset(axis_bitmap, 0, sizeof(axis_bitmap));

    const TileCoord coord_a = {100, 100};
    const TileCoord coord_b = {10000, 10000};
    const TileCoord axis_coord = {-3, -3};

    sieve_tile(coord_a, tables, bitmap_a);
    sieve_tile(coord_b, tables, bitmap_b);
    sieve_tile(axis_coord, tables, axis_bitmap);

    const int count_a = count_bitmap_bits(bitmap_a);
    const int count_b = count_bitmap_bits(bitmap_b);
    const int brute_count_a = brute_count_tile(coord_a);
    const int brute_count_b = brute_count_tile(coord_b);

    std::printf("tile (100, 100): %d Gaussian primes\n", count_a);
    std::printf("tile (10000, 10000): %d Gaussian primes\n", count_b);
    std::printf("tile (100, 100) brute-force check: %d\n", brute_count_a);
    std::printf("tile (10000, 10000) brute-force check: %d\n", brute_count_b);

    if (!require(count_a == brute_count_a, "tile (100,100) prime count mismatch vs brute force")) {
        return 1;
    }
    if (!require(count_b == brute_count_b, "tile (10000,10000) prime count mismatch vs brute force")) {
        return 1;
    }

    Point known_points[5];
    if (!require(find_known_primes(coord_a, known_points, 5), "failed to find known Gaussian primes in tile (100,100)")) {
        return 1;
    }
    for (int i = 0; i < 5; ++i) {
        std::printf("known prime %d: (%lld, %lld)\n",
                    i + 1,
                    static_cast<long long>(known_points[i].a),
                    static_cast<long long>(known_points[i].b));
        if (!require(bitmap_contains(coord_a, bitmap_a, known_points[i].a, known_points[i].b),
                     "known Gaussian prime missing from bitmap")) {
            return 1;
        }
    }

    if (!require(bitmap_contains(axis_coord, axis_bitmap, 3, 0), "axis prime (3,0) missing")) {
        return 1;
    }
    if (!require(bitmap_contains(axis_coord, axis_bitmap, 0, 3), "axis prime (0,3) missing")) {
        return 1;
    }
    if (!require(bitmap_contains(axis_coord, axis_bitmap, 7, 0), "axis prime (7,0) missing")) {
        return 1;
    }
    if (!require(bitmap_contains(axis_coord, axis_bitmap, 0, 7), "axis prime (0,7) missing")) {
        return 1;
    }
    if (!require(bitmap_contains(axis_coord, axis_bitmap, 1, 1), "off-axis prime (1,1) missing near origin")) {
        return 1;
    }
    if (!require(!bitmap_contains(axis_coord, axis_bitmap, 5, 0), "composite axis point (5,0) was marked prime")) {
        return 1;
    }
    if (!require(!bitmap_contains(axis_coord, axis_bitmap, 0, 5), "composite axis point (0,5) was marked prime")) {
        return 1;
    }
    if (!require(!bitmap_contains(axis_coord, axis_bitmap, 0, 0), "origin was marked prime")) {
        return 1;
    }

    std::puts("test_sieve passed");
    return 0;
}
