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

void expect_true(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void expect_eq_u16(uint16_t actual, uint16_t expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (actual=%u expected=%u)\n",
                     message,
                     static_cast<unsigned>(actual),
                     static_cast<unsigned>(expected));
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

    const TileCoord coord = {850000128, 490746880};
    const FaceData face_data = extract_faces(coord, bitmap, prefix, prime_pos, 4, parent);

    expect_eq_int(face_data.port_count, 4, "extracted port count");
    expect_eq_int(face_data.group_count, 3, "extracted group count");

    expect_eq_int(face_data.ports[0].face, FACE_I, "port 0 face");
    expect_eq_int(face_data.ports[0].group, 1, "port 0 group");
    expect_eq_u16(face_data.ports[0].h1, 0, "port 0 h1");

    expect_eq_int(face_data.ports[1].face, FACE_I, "port 1 face");
    expect_eq_int(face_data.ports[1].group, 2, "port 1 group");
    expect_eq_u16(face_data.ports[1].h1, 20, "port 1 h1");

    expect_eq_int(face_data.ports[2].face, FACE_O, "port 2 face");
    expect_eq_int(face_data.ports[2].group, 3, "port 2 group");
    expect_eq_u16(face_data.ports[2].h1, 30, "port 2 h1");

    expect_eq_int(face_data.ports[3].face, FACE_L, "port 3 face");
    expect_eq_int(face_data.ports[3].group, 1, "port 3 group");
    expect_eq_u16(face_data.ports[3].h1, 0, "port 3 h1");
}

void test_extract_shared_boundary_points() {
    auto extract_single = [](int tile_row, int tile_col, int expected_face, int expected_h1) {
        uint32_t bitmap[BITMAP_WORDS];
        uint32_t prefix[BITMAP_WORDS];
        std::memset(bitmap, 0, sizeof(bitmap));
        std::memset(prefix, 0, sizeof(prefix));

        set_bit(bitmap, COLLAR + tile_row, COLLAR + tile_col);
        build_prefix(bitmap, prefix);

        uint32_t prime_pos[1] = {
            static_cast<uint32_t>((COLLAR + tile_row) * SIDE_EXP + (COLLAR + tile_col)),
        };
        uint16_t parent[1] = {0};
        const TileCoord coord = {601040640, 601040640};
        const FaceData face_data = extract_faces(coord, bitmap, prefix, prime_pos, 1, parent);

        for (int i = 0; i < face_data.port_count; ++i) {
            if (face_data.ports[i].face == expected_face && face_data.ports[i].h1 == expected_h1) {
                return true;
            }
        }
        return false;
    };

    expect_true(extract_single(TILE_SIDE, 250, FACE_O, 250), "FACE_O includes h=250 near shared boundary");
    expect_true(extract_single(TILE_SIDE, TILE_SIDE, FACE_O, TILE_SIDE), "FACE_O includes h=256 shared endpoint");
    expect_true(extract_single(250, TILE_SIDE, FACE_R, 250), "FACE_R includes h=250 near shared boundary");
    expect_true(extract_single(TILE_SIDE, TILE_SIDE, FACE_R, TILE_SIDE), "FACE_R includes h=256 shared endpoint");
}

void test_parser_helpers() {
    TileOp invalid{};
    std::memset(&invalid, 0, sizeof(invalid));
    const TileOpLayout invalid_layout = parse_tileop_v2(invalid);
    expect_true(!invalid_layout.is_valid, "all-zero V1-style bytes reject as invalid V2");

    const TileOp empty = make_empty_tileop();
    const TileOpLayout empty_layout = parse_tileop_v2(empty);
    expect_true(empty_layout.is_valid, "empty tile parses");
    expect_true(empty_layout.is_empty, "empty tile detected");
    expect_eq_int(empty_layout.o_cnt, 0, "empty o_cnt");
    expect_eq_int(empty_layout.i_cnt, 0, "empty i_cnt");
    expect_eq_int(empty_layout.l_cnt, 0, "empty l_cnt");
    expect_eq_int(empty_layout.r_cnt, 0, "empty r_cnt");

    const TileOp overflow = make_overflow_tileop();
    const TileOpLayout overflow_layout = parse_tileop_v2(overflow);
    expect_true(overflow_layout.is_valid, "overflow tile parses");
    expect_true(overflow_layout.is_overflow, "overflow tile detected");

    TileOp handcrafted = make_empty_tileop();
    handcrafted.bytes[0] = 5;
    handcrafted.bytes[1] = 6;
    handcrafted.bytes[2] = 66;
    handcrafted.bytes[3] = 11;
    handcrafted.bytes[4] = 12;
    handcrafted.bytes[5] = 21;
    for (int i = 0; i < 60; ++i) {
        handcrafted.bytes[6 + i] = static_cast<uint8_t>(31 + (i % 7));
    }
    handcrafted.bytes[66] = 41;
    handcrafted.bytes[67] = 5;
    handcrafted.bytes[68] = 6;
    handcrafted.bytes[69] = 7;

    const TileOpLayout layout = parse_tileop_v2(handcrafted);
    expect_true(layout.is_valid, "handcrafted tile parses");
    expect_true(!layout.is_empty, "handcrafted tile is live");
    expect_eq_int(layout.o_cnt, 2, "handcrafted o_cnt");
    expect_eq_int(layout.i_cnt, 1, "handcrafted i_cnt");
    expect_eq_int(layout.l_cnt, 60, "handcrafted l_cnt");
    expect_eq_int(layout.r_cnt, 1, "handcrafted r_cnt");
    expect_eq_int(layout.h_start, 67, "handcrafted h_start");
    expect_eq_int(layout.payload_bytes_used, 125, "handcrafted payload used");
    expect_eq_int(layout.payload_slack, 0, "handcrafted payload slack");
}

void test_prune_and_encode_v2_round_trip() {
    FaceData face_data;
    std::memset(&face_data, 0, sizeof(face_data));
    face_data.group_count = 4;
    face_data.port_count = 63;
    face_data.ports[0] = {FACE_I, 1, 1};
    face_data.ports[1] = {FACE_O, 2, 9};
    face_data.ports[2] = {FACE_L, 3, 11};
    face_data.ports[3] = {FACE_L, 3, 13};
    for (int i = 0; i < 59; ++i) {
        face_data.ports[4 + i] = {FACE_R, 4, static_cast<uint16_t>(2 * i + 2)};
    }

    const TileOp tileop = encode_tileop(face_data);
    const TileOpLayout layout = parse_tileop_v2(tileop);
    expect_true(layout.is_valid, "encoded tile parses");
    expect_eq_u8(tileop.bytes[0], 4, "off_I");
    expect_eq_u8(tileop.bytes[1], 5, "off_L");
    expect_eq_u8(tileop.bytes[2], 7, "off_R");
    expect_eq_int(layout.o_cnt, 1, "round-trip o_cnt");
    expect_eq_int(layout.i_cnt, 1, "round-trip i_cnt");
    expect_eq_int(layout.l_cnt, 2, "round-trip l_cnt");
    expect_eq_int(layout.r_cnt, 59, "round-trip r_cnt");

    const TileOpFaceView o_view = tileop_face_view(layout, FACE_O);
    const TileOpFaceView i_view = tileop_face_view(layout, FACE_I);
    const TileOpFaceView l_view = tileop_face_view(layout, FACE_L);
    const TileOpFaceView r_view = tileop_face_view(layout, FACE_R);
    expect_eq_u8(o_view.groups[0], 2, "O group[0]");
    expect_eq_u8(i_view.groups[0], 1, "I group[0]");
    expect_eq_u8(decode_group_id(l_view.groups[0]), 3, "L group[0]");
    expect_eq_u8(decode_group_id(l_view.groups[1]), 3, "L group[1]");
    expect_eq_u8(decode_group_id(r_view.groups[0]), 4, "R group[0]");
    expect_eq_u8(decode_group_id(r_view.groups[58]), 4, "R group[last]");
    expect_eq_u8(l_view.h1_packed[0], 11, "L h1_byte[0]");
    expect_eq_u8(l_view.h1_packed[1], 13, "L h1_byte[1]");
    expect_eq_u8(r_view.h1_packed[0], 2, "R h1_byte[0]");
    expect_eq_u8(r_view.h1_packed[58], 118, "R h1_byte[last]");
    expect_eq_u16(decode_h1(l_view.groups[0], l_view.h1_packed[0]), 11, "decoded L h1[0]");
    expect_eq_u16(decode_h1(l_view.groups[1], l_view.h1_packed[1]), 13, "decoded L h1[1]");
    expect_eq_u16(decode_h1(r_view.groups[0], r_view.h1_packed[0]), 2, "decoded R h1[0]");
    expect_eq_u16(decode_h1(r_view.groups[58], r_view.h1_packed[58]), 118, "decoded R h1[last]");
    expect_eq_u8(max_group_label(tileop), 4, "max group label");
}

void test_dynamic_budget_and_overflow() {
    FaceData face_data;
    std::memset(&face_data, 0, sizeof(face_data));
    face_data.group_count = 17;
    face_data.port_count = 17;
    for (int i = 0; i < 17; ++i) {
        face_data.ports[i] = {FACE_O, 1, static_cast<uint16_t>(2 * i + 1)};
    }

    const TileOp fits_v2 = encode_tileop(face_data);
    const TileOpLayout fits_layout = parse_tileop_v2(fits_v2);
    expect_true(fits_layout.is_valid, "17-port single-face tile fits V2");
    expect_true(!fits_layout.is_overflow, "17-port single-face tile is not overflow");
    expect_eq_int(fits_layout.o_cnt, 17, "17-port O count");

    FaceData overflow;
    std::memset(&overflow, 0, sizeof(overflow));
    overflow.group_count = 1;
    overflow.port_count = 63;
    for (int i = 0; i < 31; ++i) {
        overflow.ports[i] = {FACE_L, 1, static_cast<uint16_t>(2 * i)};
    }
    for (int i = 0; i < 32; ++i) {
        overflow.ports[31 + i] = {FACE_R, 1, static_cast<uint16_t>(2 * i + 1)};
    }

    const TileOp overflow_tileop = encode_tileop(overflow);
    expect_eq_u8(overflow_tileop.bytes[0], OVERFLOW_SENTINEL, "payload budget overflow sentinel");
    expect_eq_u8(overflow_tileop.bytes[64], OVERFLOW_SENTINEL, "payload overflow poisons middle bytes");
    expect_eq_u8(overflow_tileop.bytes[127], OVERFLOW_SENTINEL, "payload overflow poisons tail");
}

void test_group_bit_steal_round_trip() {
    auto make_group_bit_tile = [](uint16_t left_h1, int left_group) {
        FaceData face_data;
        std::memset(&face_data, 0, sizeof(face_data));
        face_data.group_count = 2;
        face_data.port_count = 62;
        face_data.ports[0] = {FACE_L, left_group, left_h1};
        for (int i = 0; i < 61; ++i) {
            face_data.ports[1 + i] = {FACE_R, 2, static_cast<uint16_t>(2 * i + 136)};
        }
        return encode_tileop(face_data);
    };

    const TileOp interior_tileop = make_group_bit_tile(255, 1);
    const TileOpLayout interior_layout = parse_tileop_v2(interior_tileop);
    expect_true(interior_layout.is_valid, "interior group-bit tile parses");
    expect_eq_int(interior_layout.l_cnt, 1, "interior l_cnt");
    expect_eq_int(interior_layout.r_cnt, 61, "interior r_cnt");
    const TileOpFaceView interior_l_view = tileop_face_view(interior_layout, FACE_L);
    expect_eq_u8(interior_l_view.groups[0], 1, "group byte preserves interior group");
    expect_eq_u8(interior_l_view.h1_packed[0], 255, "interior h1 byte");
    expect_eq_u8(decode_group_id(interior_l_view.groups[0]), 1, "decode interior group");
    expect_eq_u16(decode_h1(interior_l_view.groups[0], interior_l_view.h1_packed[0]), 255, "decode interior h1");

    const TileOp boundary_tileop = make_group_bit_tile(256, 1);
    const TileOpLayout boundary_layout = parse_tileop_v2(boundary_tileop);
    expect_true(boundary_layout.is_valid, "boundary group-bit tile parses");
    const TileOpFaceView boundary_l_view = tileop_face_view(boundary_layout, FACE_L);
    expect_eq_u8(boundary_l_view.groups[0], 0x81, "group bit carries h1 ninth bit");
    expect_eq_u8(boundary_l_view.h1_packed[0], 0, "boundary h1 low byte");
    expect_eq_u8(decode_group_id(boundary_l_view.groups[0]), 1, "decode boundary group");
    expect_eq_u16(decode_h1(boundary_l_view.groups[0], boundary_l_view.h1_packed[0]), 256, "decode boundary h1");
}

void test_group_cap_overflow() {
    FaceData overflow;
    std::memset(&overflow, 0, sizeof(overflow));
    overflow.group_count = 128;
    overflow.port_count = 1;
    overflow.ports[0] = {FACE_O, 1, 0};

    const TileOp overflow_tileop = encode_tileop(overflow);
    expect_eq_u8(overflow_tileop.bytes[0], OVERFLOW_SENTINEL, "group cap overflow sentinel");
    expect_eq_u8(overflow_tileop.bytes[64], OVERFLOW_SENTINEL, "group cap poisons middle bytes");
    expect_eq_u8(overflow_tileop.bytes[127], OVERFLOW_SENTINEL, "group cap poisons tail");
}

}  // namespace

int main() {
    test_parser_helpers();
    test_extract_faces();
    test_extract_shared_boundary_points();
    test_prune_and_encode_v2_round_trip();
    test_dynamic_budget_and_overflow();
    test_group_bit_steal_round_trip();
    test_group_cap_overflow();
    std::puts("test_face_encode: OK");
    return 0;
}
