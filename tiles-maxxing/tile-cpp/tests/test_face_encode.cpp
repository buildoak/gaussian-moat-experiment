#include "compact.h"
#include "constants.h"
#include "encode.h"
#include "face_extract.h"
#include "prune.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void expect_eq_u8(uint8_t actual, uint8_t expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (actual=%u expected=%u)\n",
                     message,
                     static_cast<unsigned>(actual),
                     static_cast<unsigned>(expected));
        std::exit(1);
    }
}

void expect_eq_int(int actual, int expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (actual=%d expected=%d)\n", message, actual, expected);
        std::exit(1);
    }
}

void build_prefix(const uint32_t* bitmap, uint32_t* prefix) {
    uint32_t running = 0;
    for (int i = 0; i < BITMAP_WORDS; ++i) {
        prefix[i] = running;
        running += static_cast<uint32_t>(__builtin_popcount(bitmap[i]));
    }
}

void set_bit(uint32_t* bitmap, int row, int col) {
    const uint32_t pos = static_cast<uint32_t>(row * SIDE_EXP + col);
    bitmap[pos >> 5] |= (1U << (pos & 31U));
}

void test_extract_faces() {
    uint32_t bitmap[BITMAP_WORDS];
    uint32_t prefix[BITMAP_WORDS];
    std::memset(bitmap, 0, sizeof(bitmap));
    std::memset(prefix, 0, sizeof(prefix));

    const uint32_t pos_a = static_cast<uint32_t>((COLLAR + 0) * SIDE_EXP + (COLLAR + 0));
    const uint32_t pos_b = static_cast<uint32_t>((COLLAR + 4) * SIDE_EXP + (COLLAR + 1));
    const uint32_t pos_c = static_cast<uint32_t>((COLLAR + 0) * SIDE_EXP + (COLLAR + 20));
    const uint32_t pos_d = static_cast<uint32_t>((COLLAR + 250) * SIDE_EXP + (COLLAR + 30));

    set_bit(bitmap, COLLAR + 0, COLLAR + 0);
    set_bit(bitmap, COLLAR + 4, COLLAR + 1);
    set_bit(bitmap, COLLAR + 0, COLLAR + 20);
    set_bit(bitmap, COLLAR + 250, COLLAR + 30);
    build_prefix(bitmap, prefix);

    uint32_t prime_pos[4] = {pos_a, pos_c, pos_b, pos_d};
    const int idx_a = bitmap_pos_to_uf_index(pos_a, bitmap, prefix);
    const int idx_b = bitmap_pos_to_uf_index(pos_b, bitmap, prefix);
    const int idx_c = bitmap_pos_to_uf_index(pos_c, bitmap, prefix);
    const int idx_d = bitmap_pos_to_uf_index(pos_d, bitmap, prefix);
    expect_eq_int(idx_a, 0, "UF index A");
    expect_eq_int(idx_b, 2, "UF index B");
    expect_eq_int(idx_c, 1, "UF index C");
    expect_eq_int(idx_d, 3, "UF index D");

    uint16_t parent[4];
    parent[0] = 0;
    parent[1] = 2;
    parent[2] = 0;
    parent[3] = 3;

    const TileCoord coord = {0, 0};
    const FaceData face_data = extract_faces(coord, bitmap, prefix, prime_pos, 4, parent);

    expect_eq_int(face_data.port_count, 4, "extracted port count");
    expect_eq_int(face_data.group_count, 3, "extracted group count");

    expect_eq_int(face_data.ports[0].face, FACE_I, "port 0 face");
    expect_eq_int(face_data.ports[0].group, 1, "port 0 group");
    expect_eq_u8(face_data.ports[0].h1, 0, "port 0 h1");

    expect_eq_int(face_data.ports[1].face, FACE_I, "port 1 face");
    expect_eq_int(face_data.ports[1].group, 2, "port 1 group");
    expect_eq_u8(face_data.ports[1].h1, 20, "port 1 h1");

    expect_eq_int(face_data.ports[2].face, FACE_O, "port 2 face");
    expect_eq_int(face_data.ports[2].group, 3, "port 2 group");
    expect_eq_u8(face_data.ports[2].h1, 30, "port 2 h1");

    expect_eq_int(face_data.ports[3].face, FACE_L, "port 3 face");
    expect_eq_int(face_data.ports[3].group, 1, "port 3 group");
    expect_eq_u8(face_data.ports[3].h1, 0, "port 3 h1");
}

void test_prune_and_encode_packing() {
    FaceData face_data;
    std::memset(&face_data, 0, sizeof(face_data));
    face_data.group_count = 4;
    face_data.port_count = 5;

    face_data.ports[0] = {FACE_I, 1, 1};
    face_data.ports[1] = {FACE_I, 2, 5};
    face_data.ports[2] = {FACE_O, 2, 9};
    face_data.ports[3] = {FACE_L, 3, 11};
    face_data.ports[4] = {FACE_L, 3, 13};

    const FaceData pruned = prune_dead_ends(face_data);
    expect_eq_int(pruned.port_count, 4, "pruned port count");
    expect_eq_int(pruned.group_count, 2, "pruned group count");

    const TileOp tileop = encode_tileop(pruned);

    expect_eq_u8(tileop.bytes[0], 1, "I group[0]");
    expect_eq_u8(tileop.bytes[1], 0, "I group[1]");
    expect_eq_u8(tileop.bytes[16], 1, "O group[0]");
    expect_eq_u8(tileop.bytes[32], 2, "L group[0]");
    expect_eq_u8(tileop.bytes[33], 2, "L group[1]");
    expect_eq_u8(tileop.bytes[48], 0, "R group[0]");
    expect_eq_u8(tileop.bytes[64], 11, "L h1[0]");
    expect_eq_u8(tileop.bytes[65], 13, "L h1[1]");
    expect_eq_u8(tileop.bytes[80], 0, "R h1[0]");
}

void test_face_overflow() {
    FaceData face_data;
    std::memset(&face_data, 0, sizeof(face_data));
    face_data.group_count = 1;
    face_data.port_count = 17;
    for (int i = 0; i < 17; ++i) {
        face_data.ports[i] = {FACE_I, 1, static_cast<uint8_t>(i)};
    }

    const TileOp tileop = encode_tileop(face_data);
    expect_eq_u8(tileop.bytes[0], OVERFLOW_SENTINEL, "overflow sentinel");
    expect_eq_u8(tileop.bytes[16], OVERFLOW_SENTINEL, "whole tile is poisoned");
    expect_eq_u8(tileop.bytes[127], OVERFLOW_SENTINEL, "reserved bytes poisoned too");
}

}  // namespace

int main() {
    test_extract_faces();
    test_prune_and_encode_packing();
    test_face_overflow();
    std::puts("test_face_encode: OK");
    return 0;
}
