#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

constexpr int32_t TILE_SIDE = 256;
constexpr int32_t RADIAL_DEPTH = 8192;
constexpr int32_t TILES_PER_TOWER = 32;
constexpr int32_t TILES_PER_TOWER_MAX = 46;
constexpr int32_t TILEOP_SIZE = 128;
constexpr int32_t TILEOP_EXT_SIZE = 256;
constexpr int32_t TILEOP_HEADER_BYTES = 3;
constexpr int32_t TILEOP_PAYLOAD_BYTES = 125;
constexpr int32_t TILEOP_EXT_PAYLOAD_BYTES = 253;
constexpr uint8_t OVERFLOW_SENTINEL = 0xFF;
constexpr uint8_t EMPTY_OFFSET = 3;

enum Face : int {
    FACE_I = 0,
    FACE_O = 1,
    FACE_L = 2,
    FACE_R = 3,
};

struct Grid {
    int64_t R = 0;
    int S = 0;
    int W = 0;
    int num_towers = 0;
    std::vector<uint32_t> tiles_per_tower;   // per-tower height [32..46]
    std::vector<uint64_t> tower_offset;      // prefix sums for flat tile indexing
    uint64_t total_tiles = 0;                // sum of all tiles_per_tower
    std::vector<int64_t> base_y;
    std::vector<int64_t> delta;
};

struct ExtendedTileSideTable {
    std::unordered_map<uint32_t, std::array<uint8_t, 256>> extended_ops;

    bool is_extended(uint32_t flat_idx) const {
        return extended_ops.count(flat_idx) > 0U;
    }
};

struct CompositorResult {
    enum Verdict {
        SPANNING,
        MOAT,
    };

    Verdict verdict = MOAT;
    uint32_t total_groups = 0;
    uint32_t inner_root_count = 0;
    uint32_t outer_root_count = 0;
};
