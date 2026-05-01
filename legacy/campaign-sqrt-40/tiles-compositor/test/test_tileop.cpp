#ifdef NDEBUG
#undef NDEBUG
#endif

#include "tileop_parse.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace {

void make_tileop(uint8_t* buf,
                 uint8_t off_I,
                 uint8_t off_L,
                 uint8_t off_R,
                 const uint8_t* payload,
                 int payload_len) {
    std::memset(buf, 0, TILEOP_SIZE);
    buf[0] = off_I;
    buf[1] = off_L;
    buf[2] = off_R;
    if (payload != nullptr && payload_len > 0) {
        std::memcpy(buf + TILEOP_HEADER_BYTES, payload, static_cast<std::size_t>(payload_len));
    }
}

void test_overflow_detection() {
    std::printf("  test_overflow_detection... ");
    uint8_t tile[TILEOP_SIZE];
    std::memset(tile, 0xFF, sizeof(tile));

    assert(is_overflow(tile));
    assert(!is_dead(tile));
    std::printf("OK\n");
}

void test_dead_tile_detection() {
    std::printf("  test_dead_tile_detection... ");
    uint8_t tile[TILEOP_SIZE];
    std::memset(tile, 0, sizeof(tile));
    tile[0] = EMPTY_OFFSET;
    tile[1] = EMPTY_OFFSET;
    tile[2] = EMPTY_OFFSET;
    tile[3] = 0;

    assert(is_dead(tile));
    assert(!is_overflow(tile));
    std::printf("OK\n");
}

void test_synthetic_normal_tile() {
    std::printf("  test_synthetic_normal_tile... ");
    uint8_t tile[TILEOP_SIZE];
    make_tileop(tile, 6, 9, 11, nullptr, 0);

    tile[3] = 4;
    tile[4] = 7;
    tile[5] = 2;

    tile[6] = 1;
    tile[7] = 5;
    tile[8] = 3;

    tile[9] = 6;
    tile[10] = 9;
    tile[68] = 17;
    tile[69] = 200;

    tile[11] = 11;
    tile[12] = 12;
    tile[13] = 8;
    tile[70] = 33;
    tile[71] = 99;
    tile[72] = 255;

    const TileOpCounts counts = parse_counts(tile);
    assert(counts.off_I == 6);
    assert(counts.off_L == 9);
    assert(counts.off_R == 11);
    assert(counts.o_cnt == 3);
    assert(counts.i_cnt == 3);
    assert(counts.l_cnt == 2);
    assert(counts.r_cnt == 57);
    assert(counts.h_start == 68);

    const FaceSlice o_face = face_slice(tile, FACE_O);
    assert(o_face.count == 3);
    assert(o_face.h1_bytes == nullptr);
    assert(o_face.groups[0] == 4);
    assert(o_face.groups[1] == 7);
    assert(o_face.groups[2] == 2);

    const FaceSlice i_face = face_slice(tile, FACE_I);
    assert(i_face.count == 3);
    assert(i_face.h1_bytes == nullptr);
    assert(i_face.groups[0] == 1);
    assert(i_face.groups[1] == 5);
    assert(i_face.groups[2] == 3);

    const FaceSlice l_face = face_slice(tile, FACE_L);
    assert(l_face.count == 2);
    assert(decode_group_id(l_face.groups[0]) == 6);
    assert(decode_group_id(l_face.groups[1]) == 9);
    assert(decode_h1(l_face.groups[0], l_face.h1_bytes[0]) == 17);
    assert(decode_h1(l_face.groups[1], l_face.h1_bytes[1]) == 200);

    const FaceSlice r_face = face_slice(tile, FACE_R);
    assert(r_face.count == 57);
    assert(decode_group_id(r_face.groups[0]) == 11);
    assert(decode_group_id(r_face.groups[1]) == 12);
    assert(decode_group_id(r_face.groups[2]) == 8);
    assert(decode_h1(r_face.groups[0], r_face.h1_bytes[0]) == 33);
    assert(decode_h1(r_face.groups[1], r_face.h1_bytes[1]) == 99);
    assert(decode_h1(r_face.groups[2], r_face.h1_bytes[2]) == 255);

    assert(max_group_label(tile) == 12);
    std::printf("OK\n");
}

void test_h1_256_edge_case() {
    std::printf("  test_h1_256_edge_case... ");
    assert(decode_group_id(0x83) == 3);
    assert(decode_h1(0x83, 0x00) == 256);
    std::printf("OK\n");
}

void test_zero_count_faces() {
    std::printf("  test_zero_count_faces... ");
    uint8_t tile[TILEOP_SIZE];
    make_tileop(tile, 3, 3, 3, nullptr, 0);
    tile[3] = 5;
    tile[65] = 9;

    const TileOpCounts counts = parse_counts(tile);
    assert(counts.o_cnt == 0);
    assert(counts.i_cnt == 0);
    assert(counts.l_cnt == 0);
    assert(counts.r_cnt == 62);
    assert(counts.h_start == 65);

    const FaceSlice o_face = face_slice(tile, FACE_O);
    const FaceSlice i_face = face_slice(tile, FACE_I);
    const FaceSlice l_face = face_slice(tile, FACE_L);
    const FaceSlice r_face = face_slice(tile, FACE_R);

    assert(o_face.count == 0);
    assert(i_face.count == 0);
    assert(l_face.count == 0);
    assert(r_face.count == 62);
    assert(r_face.groups == tile + 3);
    assert(r_face.h1_bytes == tile + 65);
    assert(decode_group_id(r_face.groups[0]) == 5);
    assert(decode_h1(r_face.groups[0], r_face.h1_bytes[0]) == 9);
    std::printf("OK\n");
}

void test_exact_full_payload() {
    std::printf("  test_exact_full_payload... ");
    uint8_t tile[TILEOP_SIZE];
    make_tileop(tile, 4, 4, 4, nullptr, 0);
    tile[3] = 7;

    const TileOpCounts counts = parse_counts(tile);
    assert(counts.o_cnt == 1);
    assert(counts.i_cnt == 0);
    assert(counts.l_cnt == 0);
    assert(counts.r_cnt == 62);
    assert(counts.h_start == 66);
    assert(static_cast<int>(counts.h_start) + static_cast<int>(counts.l_cnt) +
               static_cast<int>(counts.r_cnt) ==
           TILEOP_SIZE);

    const FaceSlice o_face = face_slice(tile, FACE_O);
    const FaceSlice r_face = face_slice(tile, FACE_R);
    assert(o_face.count == 1);
    assert(o_face.groups[0] == 7);
    assert(r_face.count == 62);
    assert(r_face.groups == tile + 4);
    assert(r_face.h1_bytes == tile + 66);
    assert(r_face.h1_bytes + r_face.count == tile + TILEOP_SIZE);
    std::printf("OK\n");
}

void test_zero_padded_r_face_slots() {
    std::printf("  test_zero_padded_r_face_slots... ");
    uint8_t tile[TILEOP_SIZE];
    make_tileop(tile, 120, 120, 120, nullptr, 0);

    tile[120] = 1;
    tile[121] = 2;
    tile[124] = 10;
    tile[125] = 20;

    const TileOpCounts counts = parse_counts(tile);
    assert(counts.o_cnt == 117);
    assert(counts.i_cnt == 0);
    assert(counts.l_cnt == 0);
    assert(counts.r_cnt == 4);
    assert(counts.h_start == 124);

    const FaceSlice r_face = face_slice(tile, FACE_R);
    assert(r_face.count == 4);
    assert(decode_group_id(r_face.groups[0]) == 1);
    assert(decode_group_id(r_face.groups[1]) == 2);
    assert(decode_group_id(r_face.groups[2]) == 0);
    assert(decode_group_id(r_face.groups[3]) == 0);
    assert(decode_h1(r_face.groups[0], r_face.h1_bytes[0]) == 10);
    assert(decode_h1(r_face.groups[1], r_face.h1_bytes[1]) == 20);
    assert(max_group_label(tile) == 2);
    std::printf("OK\n");
}

void test_real_data_probe() {
    std::printf("  test_real_data_probe... ");
    const char* candidates[] = {
        "../../tiles-maxxing/results/4090-300k/cuda_45deg.bin",
        "tiles-maxxing/results/4090-300k/cuda_45deg.bin",
    };

    std::ifstream input;
    const char* path_used = nullptr;
    for (const char* candidate : candidates) {
        input.open(candidate, std::ios::binary);
        if (input.is_open()) {
            path_used = candidate;
            break;
        }
        input.clear();
    }

    if (!input.is_open()) {
        std::printf("SKIPPED\n");
        return;
    }

    uint32_t tile_count = 0;
    input.read(reinterpret_cast<char*>(&tile_count), sizeof(tile_count));
    assert(input.good());

    const uint32_t probe_count = std::min<uint32_t>(tile_count, 10U);
    for (uint32_t i = 0; i < probe_count; ++i) {
        char record[20 + TILEOP_SIZE];
        input.read(record, sizeof(record));
        assert(input.good());

        const auto* tile = reinterpret_cast<const uint8_t*>(record + 20);
        assert(!is_overflow(tile));
        assert(!is_dead(tile));

        const TileOpCounts counts = parse_counts(tile);
        (void)counts;
        const FaceSlice o_face = face_slice(tile, FACE_O);
        const FaceSlice i_face = face_slice(tile, FACE_I);
        const FaceSlice l_face = face_slice(tile, FACE_L);
        const FaceSlice r_face = face_slice(tile, FACE_R);
        assert(o_face.count == counts.o_cnt);
        assert(i_face.count == counts.i_cnt);
        assert(l_face.count == counts.l_cnt);
        assert(r_face.count == counts.r_cnt);
    }

    std::printf("path=%s tiles=%u OK\n", path_used, probe_count);
}

}  // namespace

int main() {
    std::printf("=== test_tileop ===\n");
    test_overflow_detection();
    test_dead_tile_detection();
    test_synthetic_normal_tile();
    test_h1_256_edge_case();
    test_zero_count_faces();
    test_exact_full_payload();
    test_zero_padded_r_face_slots();
    test_real_data_probe();
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
