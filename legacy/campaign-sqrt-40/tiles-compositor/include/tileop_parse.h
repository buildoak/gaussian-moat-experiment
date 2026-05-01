#pragma once

#include "types.h"

#include <cstdint>

struct TileOpCounts {
    uint8_t off_I = 0;
    uint8_t off_L = 0;
    uint8_t off_R = 0;
    uint8_t o_cnt = 0;
    uint8_t i_cnt = 0;
    uint8_t l_cnt = 0;
    uint8_t r_cnt = 0;
    uint8_t h_start = 0;
};

struct FaceSlice {
    const uint8_t* groups = nullptr;
    const uint8_t* h1_bytes = nullptr;
    uint8_t count = 0;
};

bool is_overflow(const uint8_t* tile);
bool is_dead(const uint8_t* tile);
TileOpCounts parse_counts(const uint8_t* tile, int payload_budget = TILEOP_PAYLOAD_BYTES);
uint8_t max_group_label(const uint8_t* tile, int payload_budget = TILEOP_PAYLOAD_BYTES);
FaceSlice face_slice(const uint8_t* tile, int face, int payload_budget = TILEOP_PAYLOAD_BYTES);

inline uint8_t decode_group_id(uint8_t group_byte) {
    return static_cast<uint8_t>(group_byte & 0x7FU);
}

inline uint16_t decode_h1(uint8_t group_byte, uint8_t h1_byte) {
    return static_cast<uint16_t>(((group_byte >> 7) << 8) | h1_byte);
}

inline const uint8_t* l_h1_bytes(const uint8_t* tile, int payload_budget = TILEOP_PAYLOAD_BYTES) {
    const TileOpCounts counts = parse_counts(tile, payload_budget);
    return tile + counts.h_start;
}

inline const uint8_t* r_h1_bytes(const uint8_t* tile, int payload_budget = TILEOP_PAYLOAD_BYTES) {
    const TileOpCounts counts = parse_counts(tile, payload_budget);
    return tile + counts.h_start + counts.l_cnt;
}
