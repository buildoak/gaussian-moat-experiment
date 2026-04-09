#include "process_tile.h"
#include "union_find.h"

#include <cstdio>
#include <cstdint>

namespace {

void print_hex(const uint8_t* data, int len) {
    for (int i = 0; i < len; ++i) {
        std::printf("%02x", data[i]);
        if ((i & 15) == 15) {
            std::printf("\n");
        } else {
            std::printf(" ");
        }
    }
    if (len % 16 != 0) {
        std::printf("\n");
    }
}

bool run_tile(const char* label, const TileCoord& coord, const SieveTables& tables) {
    std::printf("--- %s: tile (%lld, %lld) ---\n",
                label,
                static_cast<long long>(coord.a_lo),
                static_cast<long long>(coord.b_lo));

    const TileResult result = process_tile(coord, tables);

    std::printf("  prime_count:          %u\n", result.prime_count);
    std::printf("  group_count:          %u\n", result.group_count);
    std::printf("  ports_before_pruning: %u\n", result.ports_before_pruning);
    std::printf("  ports_after_pruning:  %u\n", result.ports_after_pruning);

    std::printf("  TileOp first 64 bytes:\n    ");
    print_hex(result.tileop.bytes, 64);

    if (result.prime_count == 0) {
        std::fprintf(stderr, "FAIL: %s — prime_count is 0\n", label);
        return false;
    }

    std::printf("  PASS\n\n");
    return true;
}

}  // namespace

int main() {
    // Init sieve tables
    SieveTables tables;
    if (!init_sieve_tables(tables)) {
        std::fprintf(stderr, "FAIL: init_sieve_tables failed\n");
        return 1;
    }
    std::printf("Sieve tables initialized: %d split, %d inert\n\n",
                tables.split_count, tables.inert_count);

    // Init backward offsets (used internally by build_components,
    // but we warm it up here so any init failure is visible)
    BackwardOffsets offsets;
    init_backward_offsets(offsets);
    std::printf("Backward offsets: %d entries\n\n", offsets.count);

    bool all_pass = true;

    const TileCoord tile_a = {601040640, 601040640};
    all_pass &= run_tile("tile_a_45deg", tile_a, tables);

    const TileCoord tile_b = {736121088, 424999936};
    all_pass &= run_tile("tile_b_30deg", tile_b, tables);

    const TileCoord tile_c = {820888320, 220000000};
    all_pass &= run_tile("tile_c_15deg", tile_c, tables);

    if (all_pass) {
        std::puts("test_e2e: ALL PASSED");
        return 0;
    } else {
        std::puts("test_e2e: SOME FAILED");
        return 1;
    }
}
