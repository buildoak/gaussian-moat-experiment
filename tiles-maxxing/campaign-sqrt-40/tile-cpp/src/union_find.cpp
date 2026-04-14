#include "union_find.h"

#include "compact.h"

#include <cassert>
#include <cstdint>

namespace {

static_assert(MAX_PRIMES <= 65535, "parent array entries must fit in uint16_t");

inline bool bitmap_test(const uint32_t* bitmap, uint32_t pos) {
    return ((bitmap[pos >> 5] >> (pos & 31U)) & 1U) != 0U;
}

inline uint16_t find_root(uint16_t* parent, uint16_t x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

inline void union_sets(uint16_t* parent, uint16_t x, uint16_t y) {
    uint16_t rx = find_root(parent, x);
    uint16_t ry = find_root(parent, y);
    if (rx == ry) {
        return;
    }
    if (rx > ry) {
        const uint16_t tmp = rx;
        rx = ry;
        ry = tmp;
    }
    parent[ry] = rx;
}

const BackwardOffsets& get_backward_offsets() {
    static BackwardOffsets offsets = []() {
        BackwardOffsets o;
        init_backward_offsets(o);
        return o;
    }();
    return offsets;
}

}  // namespace

void init_backward_offsets(BackwardOffsets& offsets) {
    int count = 0;

    for (int dr = -COLLAR; dr <= 0; ++dr) {
        for (int dc = -COLLAR; dc <= COLLAR; ++dc) {
            if ((dr > 0) || (dr == 0 && dc >= 0)) {
                continue;
            }
            if ((dr * dr + dc * dc) > K_SQ) {
                continue;
            }

            assert(count < MAX_BACKWARD_OFFSETS);
            offsets.dr[count] = dr;
            offsets.dc[count] = dc;
            ++count;
        }
    }

    offsets.count = count;
}

void build_components(const uint32_t* bitmap, const uint32_t* prefix,
                      const uint32_t* prime_pos, int prime_count,
                      uint16_t* parent) {
    const BackwardOffsets& offsets = get_backward_offsets();

    for (int i = 0; i < prime_count; ++i) {
        parent[i] = static_cast<uint16_t>(i);
    }

    for (int i = 0; i < prime_count; ++i) {
        const uint32_t pos = prime_pos[i];
        const int row = static_cast<int>(pos / static_cast<uint32_t>(SIDE_EXP));
        const int col = static_cast<int>(pos % static_cast<uint32_t>(SIDE_EXP));

        for (int k = 0; k < offsets.count; ++k) {
            const int nr = row + offsets.dr[k];
            const int nc = col + offsets.dc[k];
            if (nr < 0 || nr >= SIDE_EXP || nc < 0 || nc >= SIDE_EXP) {
                continue;
            }

            const uint32_t npos = static_cast<uint32_t>(nr * SIDE_EXP + nc);
            if (!bitmap_test(bitmap, npos)) {
                continue;
            }

            const int j = bitmap_pos_to_uf_index(npos, bitmap, prefix);
            union_sets(parent, static_cast<uint16_t>(i), static_cast<uint16_t>(j));
        }
    }

    for (int i = 0; i < prime_count; ++i) {
        parent[i] = find_root(parent, static_cast<uint16_t>(i));
    }
}
