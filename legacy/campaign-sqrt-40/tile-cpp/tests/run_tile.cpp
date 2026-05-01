#include "process_tile.h"
#include "encode.h"
#include "union_find.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool parse_i64(const char* text, int64_t* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }

    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }

    *out = static_cast<int64_t>(value);
    return true;
}

void print_hex_prefix(const uint8_t* data, int len) {
    for (int i = 0; i < len; ++i) {
        std::printf("%02x", data[i]);
        if (i + 1 < len) {
            std::printf(" ");
        }
    }
    std::printf("\n");
}

void print_face_summary(const TileOpLayout& layout) {
    std::printf("tileop_status=%s\n",
                layout.is_overflow ? "overflow" : (layout.is_empty ? "empty" : (layout.is_valid ? "normal" : "invalid")));
    if (!layout.is_valid || layout.is_overflow) {
        return;
    }

    std::printf("tileop_offsets=%u,%u,%u\n",
                static_cast<unsigned>(layout.off_I),
                static_cast<unsigned>(layout.off_L),
                static_cast<unsigned>(layout.off_R));
    std::printf("tileop_face_counts=O:%u I:%u L:%u R:%u\n",
                static_cast<unsigned>(layout.o_cnt),
                static_cast<unsigned>(layout.i_cnt),
                static_cast<unsigned>(layout.l_cnt),
                static_cast<unsigned>(layout.r_cnt));
    std::printf("tileop_payload=used:%u slack:%u\n",
                static_cast<unsigned>(layout.payload_bytes_used),
                static_cast<unsigned>(layout.payload_slack));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <a_lo> <b_lo>\n", argv[0]);
        return 2;
    }

    TileCoord coord{};
    if (!parse_i64(argv[1], &coord.a_lo) || !parse_i64(argv[2], &coord.b_lo)) {
        std::fprintf(stderr, "error: invalid int64 tile coordinates\n");
        return 2;
    }

    SieveTables tables;
    if (!init_sieve_tables(tables)) {
        std::fprintf(stderr, "error: init_sieve_tables failed\n");
        return 1;
    }

    BackwardOffsets offsets;
    init_backward_offsets(offsets);

    const auto start = std::chrono::steady_clock::now();
    const TileResult result = process_tile(coord, tables);
    const auto end = std::chrono::steady_clock::now();
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::printf("tile=(%lld,%lld)\n",
                static_cast<long long>(coord.a_lo),
                static_cast<long long>(coord.b_lo));
    std::printf("prime_count=%u\n", result.prime_count);
    std::printf("group_count=%u\n", result.group_count);
    std::printf("ports_before_pruning=%u\n", result.ports_before_pruning);
    std::printf("ports_after_pruning=%u\n", result.ports_after_pruning);
    std::printf("wall_ms=%lld\n", static_cast<long long>(wall_ms));
    const TileOpLayout layout = parse_tileop_v2(result.tileop);
    print_face_summary(layout);
    std::printf("tileop_hex=");
    print_hex_prefix(result.tileop.bytes, TILEOP_SIZE);

    return 0;
}
