#include "process_tile.h"

#include "compact.h"
#include "union_find.h"
#include "face_extract.h"
#include "prune.h"
#include "encode.h"

#include <chrono>
#include <cstring>

namespace {

uint32_t count_tile_proper_primes(const uint32_t* prime_pos, int prime_count) {
    uint32_t count = 0;
    for (int i = 0; i < prime_count; ++i) {
        const uint32_t pos = prime_pos[i];
        const int row = static_cast<int>(pos / SIDE_EXP);
        const int col = static_cast<int>(pos % SIDE_EXP);
        const int tile_row = row - COLLAR;
        const int tile_col = col - COLLAR;
        if (tile_row >= 0 && tile_row <= TILE_SIDE && tile_col >= 0 && tile_col <= TILE_SIDE) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TileResult process_tile(const TileCoord& coord, const SieveTables& tables,
                        PhaseTimings* timings) {
    using clock = std::chrono::steady_clock;
    const auto t0 = timings ? clock::now() : clock::time_point{};

    TileResult result;
    std::memset(&result, 0, sizeof(result));
    result.tileop = make_empty_tileop();

    // Phase 1: Sieve — produce the prime bitmap
    uint32_t bitmap[BITMAP_WORDS];
    std::memset(bitmap, 0, sizeof(bitmap));
    sieve_tile(coord, tables, bitmap);
    const auto t1 = timings ? clock::now() : clock::time_point{};

    // Phase 2: Compact — build prefix popcount + dense prime positions
    uint32_t prefix[BITMAP_WORDS];
    uint32_t prime_pos[MAX_PRIMES];
    const int bitmap_prime_count = compact_primes(bitmap, prefix, prime_pos);
    result.prime_count = static_cast<uint32_t>(bitmap_prime_count);  // match CUDA: count all bitmap primes incl. collar
    const auto t2 = timings ? clock::now() : clock::time_point{};

    if (bitmap_prime_count == 0) {
        if (timings) {
            auto ns = [](clock::time_point a, clock::time_point b) -> int64_t {
                return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
            };
            timings->sieve_ns        = ns(t0, t1);
            timings->compact_ns      = ns(t1, t2);
            timings->union_find_ns   = 0;
            timings->face_extract_ns = 0;
            timings->prune_encode_ns = 0;
            timings->total_ns        = ns(t0, t2);
        }
        return result;
    }

    // Phase 3: Union-Find — build connected components
    uint16_t parent[MAX_PRIMES];
    build_components(bitmap, prefix, prime_pos, bitmap_prime_count, parent);
    const auto t3 = timings ? clock::now() : clock::time_point{};

    // Phase 4: Face extraction — identify boundary ports
    const FaceData face_data = extract_faces(coord, bitmap, prefix, prime_pos, bitmap_prime_count, parent);
    result.ports_before_pruning = static_cast<uint32_t>(face_data.port_count);
    const auto t4 = timings ? clock::now() : clock::time_point{};

    // Phase 5a: Dead-end pruning — remove single-face, single-port groups and renumber survivors
    const FaceData pruned_face_data = prune_dead_ends(face_data);
    result.ports_after_pruning = static_cast<uint32_t>(pruned_face_data.port_count);
    result.group_count = static_cast<uint32_t>(pruned_face_data.group_count);

    // Phase 5b: Encode — produce the 256-byte TileOp from already-pruned face data
    result.tileop = encode_tileop(pruned_face_data);
    const auto t5 = timings ? clock::now() : clock::time_point{};

    if (timings) {
        auto ns = [](clock::time_point a, clock::time_point b) -> int64_t {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        };
        timings->sieve_ns        = ns(t0, t1);
        timings->compact_ns      = ns(t1, t2);
        timings->union_find_ns   = ns(t2, t3);
        timings->face_extract_ns = ns(t3, t4);
        timings->prune_encode_ns = ns(t4, t5);
        timings->total_ns        = ns(t0, t5);
    }

    return result;
}
