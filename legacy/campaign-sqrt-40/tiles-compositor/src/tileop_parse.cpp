#include "tileop_parse.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

bool is_overflow(const uint8_t* tile) {
    assert(tile != nullptr);
    return tile[0] == OVERFLOW_SENTINEL;
}

bool is_dead(const uint8_t* tile) {
    assert(tile != nullptr);
    return tile[0] == EMPTY_OFFSET && tile[1] == EMPTY_OFFSET && tile[2] == EMPTY_OFFSET && tile[3] == 0;
}

TileOpCounts parse_counts(const uint8_t* tile, int payload_budget) {
    assert(tile != nullptr);
    assert(!is_overflow(tile));

    TileOpCounts counts{};
    counts.off_I = tile[0];
    counts.off_L = tile[1];
    counts.off_R = tile[2];

    assert(counts.off_I >= TILEOP_HEADER_BYTES);
    assert(counts.off_I <= counts.off_L);
    assert(counts.off_L <= counts.off_R);
    if (counts.off_I < TILEOP_HEADER_BYTES || counts.off_I > counts.off_L || counts.off_L > counts.off_R) {
        std::fprintf(stderr,
            "FATAL parse_counts: off_I=%u off_L=%u off_R=%u bytes=[%02x %02x %02x %02x] budget=%d\n",
            counts.off_I, counts.off_L, counts.off_R,
            tile[0], tile[1], tile[2], tile[3], payload_budget);
        std::abort();
    }

    const int record_size = payload_budget + TILEOP_HEADER_BYTES;
    assert(record_size == TILEOP_SIZE || record_size == TILEOP_EXT_SIZE);
    assert(counts.off_R <= record_size);
    if ((record_size != TILEOP_SIZE && record_size != TILEOP_EXT_SIZE) || counts.off_R > record_size) {
        std::fprintf(stderr,
            "FATAL parse_counts(2): off_R=%u record_size=%d bytes=[%02x %02x %02x %02x]\n",
            counts.off_R, record_size, tile[0], tile[1], tile[2], tile[3]);
        std::abort();
    }

    counts.o_cnt = static_cast<uint8_t>(counts.off_I - TILEOP_HEADER_BYTES);
    counts.i_cnt = static_cast<uint8_t>(counts.off_L - counts.off_I);
    counts.l_cnt = static_cast<uint8_t>(counts.off_R - counts.off_L);

    const int residual = payload_budget - static_cast<int>(counts.o_cnt) - static_cast<int>(counts.i_cnt) -
                         (2 * static_cast<int>(counts.l_cnt));
    assert(residual >= 0);
    if (residual < 0) {
        std::fprintf(stderr,
            "FATAL parse_counts(3): residual=%d o=%u i=%u l=%u bytes=[%02x %02x %02x %02x]\n",
            residual, counts.o_cnt, counts.i_cnt, counts.l_cnt,
            tile[0], tile[1], tile[2], tile[3]);
        std::abort();
    }

    counts.r_cnt = static_cast<uint8_t>(residual / 2);
    counts.h_start = static_cast<uint8_t>(counts.off_R + counts.r_cnt);

    assert(static_cast<int>(counts.h_start) + static_cast<int>(counts.l_cnt) +
               static_cast<int>(counts.r_cnt) <=
           record_size);
    if (static_cast<int>(counts.h_start) + static_cast<int>(counts.l_cnt) +
            static_cast<int>(counts.r_cnt) >
        record_size) {
        std::abort();
    }

    return counts;
}

uint8_t max_group_label(const uint8_t* tile, int payload_budget) {
    assert(tile != nullptr);
    if (is_overflow(tile) || is_dead(tile)) {
        return 0;
    }

    const TileOpCounts counts = parse_counts(tile, payload_budget);
    uint8_t max_label = 0;

    for (uint8_t i = 0; i < counts.o_cnt; ++i) {
        if (tile[TILEOP_HEADER_BYTES + i] > max_label) {
            max_label = tile[TILEOP_HEADER_BYTES + i];
        }
    }

    for (uint8_t i = 0; i < counts.i_cnt; ++i) {
        const uint8_t label = tile[counts.off_I + i];
        if (label > max_label) {
            max_label = label;
        }
    }

    for (uint8_t i = 0; i < counts.l_cnt; ++i) {
        const uint8_t label = decode_group_id(tile[counts.off_L + i]);
        if (label != 0 && label > max_label) {
            max_label = label;
        }
    }

    for (uint8_t i = 0; i < counts.r_cnt; ++i) {
        const uint8_t label = decode_group_id(tile[counts.off_R + i]);
        if (label != 0 && label > max_label) {
            max_label = label;
        }
    }

    return max_label;
}

FaceSlice face_slice(const uint8_t* tile, int face, int payload_budget) {
    assert(tile != nullptr);
    assert(!is_overflow(tile));

    const TileOpCounts counts = parse_counts(tile, payload_budget);
    switch (face) {
        case FACE_I:
            return FaceSlice{tile + counts.off_I, nullptr, counts.i_cnt};
        case FACE_O:
            return FaceSlice{tile + TILEOP_HEADER_BYTES, nullptr, counts.o_cnt};
        case FACE_L:
            return FaceSlice{tile + counts.off_L, tile + counts.h_start, counts.l_cnt};
        case FACE_R:
            return FaceSlice{tile + counts.off_R, tile + counts.h_start + counts.l_cnt, counts.r_cnt};
        default:
            assert(false);
            return FaceSlice{};
    }
}
