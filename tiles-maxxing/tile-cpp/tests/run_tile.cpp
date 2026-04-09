#include "process_tile.h"
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
    std::printf("tileop_first_32_bytes=");
    print_hex_prefix(result.tileop.bytes, 32);

    return 0;
}
