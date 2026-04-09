#include "compact.h"
#include "union_find.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

inline void set_bitmap_bit(uint32_t* bitmap, uint32_t pos) {
    bitmap[pos >> 5] |= (1U << (pos & 31U));
}

inline uint32_t make_pos(int row, int col) {
    return static_cast<uint32_t>(row * SIDE_EXP + col);
}

void test_backward_offsets() {
    BackwardOffsets offsets;
    init_backward_offsets(offsets);

    assert(offsets.count == 64);
    for (int i = 0; i < offsets.count; ++i) {
        const int dr = offsets.dr[i];
        const int dc = offsets.dc[i];
        assert((dr < 0) || (dr == 0 && dc < 0));
        assert((dr * dr + dc * dc) <= K_SQ);
    }
}

void test_compaction_and_components() {
    uint32_t bitmap[BITMAP_WORDS];
    uint32_t prefix[BITMAP_WORDS];
    uint32_t prime_pos[MAX_PRIMES];
    uint16_t parent[MAX_PRIMES];

    std::memset(bitmap, 0, sizeof(bitmap));
    std::memset(prefix, 0, sizeof(prefix));
    std::memset(prime_pos, 0, sizeof(prime_pos));
    std::memset(parent, 0, sizeof(parent));

    const uint32_t expected_positions[] = {
        make_pos(20, 20),
        make_pos(20, 26),
        make_pos(24, 20),
        make_pos(50, 50),
        make_pos(56, 52),
        make_pos(100, 100),
    };
    constexpr int expected_count =
        static_cast<int>(sizeof(expected_positions) / sizeof(expected_positions[0]));

    for (int i = 0; i < expected_count; ++i) {
        set_bitmap_bit(bitmap, expected_positions[i]);
    }

    const int prime_count = compact_primes(bitmap, prefix, prime_pos);
    assert(prime_count == expected_count);

    for (int i = 0; i < prime_count; ++i) {
        assert(prime_pos[i] == expected_positions[i]);
        assert(bitmap_pos_to_uf_index(expected_positions[i], bitmap, prefix) == i);
    }

    build_components(bitmap, prefix, prime_pos, prime_count, parent);

    assert(parent[0] == 0);
    assert(parent[1] == 0);
    assert(parent[2] == 0);
    assert(parent[3] == 3);
    assert(parent[4] == 3);
    assert(parent[5] == 5);
}

}  // namespace

int main() {
    test_backward_offsets();
    test_compaction_and_components();
    std::puts("test_compact_uf: OK");
    return 0;
}
