#include "sieve.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

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

bool require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        return false;
    }
    return true;
}

bool bitmaps_equal(const uint32_t* lhs, const uint32_t* rhs) {
    return std::memcmp(lhs, rhs, sizeof(uint32_t) * BITMAP_WORDS) == 0;
}

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
    uint32_t bitmap_a_repeat[BITMAP_WORDS];
    uint32_t bitmap_b[BITMAP_WORDS];
    uint32_t bitmap_b_repeat[BITMAP_WORDS];
    std::memset(bitmap_a, 0, sizeof(bitmap_a));
    std::memset(bitmap_a_repeat, 0, sizeof(bitmap_a_repeat));
    std::memset(bitmap_b, 0, sizeof(bitmap_b));
    std::memset(bitmap_b_repeat, 0, sizeof(bitmap_b_repeat));

    const TileCoord coord_a = {601040640, 601040640};
    const TileCoord coord_b = {736121344, 424999936};

    sieve_tile(coord_a, tables, bitmap_a);
    sieve_tile(coord_a, tables, bitmap_a_repeat);
    sieve_tile(coord_b, tables, bitmap_b);
    sieve_tile(coord_b, tables, bitmap_b_repeat);

    const int count_a = count_bitmap_bits(bitmap_a);
    const int count_b = count_bitmap_bits(bitmap_b);

    std::printf("tile (601040640, 601040640): %d Gaussian primes\n", count_a);
    std::printf("tile (736121344, 424999936): %d Gaussian primes\n", count_b);

    if (!require(bitmaps_equal(bitmap_a, bitmap_a_repeat), "45-degree operating tile is deterministic")) {
        return 1;
    }
    if (!require(bitmaps_equal(bitmap_b, bitmap_b_repeat), "30-degree operating tile is deterministic")) {
        return 1;
    }
    if (!require(count_a > 0, "45-degree operating tile should contain primes")) {
        return 1;
    }
    if (!require(count_b > 0, "30-degree operating tile should contain primes")) {
        return 1;
    }
    if (!require(count_a != count_b, "distinct operating tiles should not collapse to one bitmap population")) {
        return 1;
    }

    std::puts("test_sieve passed");
    return 0;
}
