#include "encode.h"

#include "constants.h"

#include <cstring>

void mark_tile_overflow(TileOp* tileop) {
    std::memset(tileop->bytes, OVERFLOW_SENTINEL, sizeof(tileop->bytes));
}

TileOp encode_tileop(const FaceData& face_data) {
    TileOp tileop;
    std::memset(&tileop, 0, sizeof(tileop));

    // Early overflow: more groups than u8 format can represent (1..254 valid range)
    if (face_data.group_count >= OVERFLOW_SENTINEL) {
        mark_tile_overflow(&tileop);
        return tileop;
    }

    int face_offsets[NUM_FACES];
    int face_counts[NUM_FACES];
    for (int face = 0; face < NUM_FACES; ++face) {
        face_offsets[face] = -1;
        face_counts[face] = 0;
    }

    for (int i = 0; i < face_data.port_count; ++i) {
        const int face = face_data.ports[i].face;
        if (face_offsets[face] < 0) {
            face_offsets[face] = i;
        }
        ++face_counts[face];
    }

    for (int face = 0; face < NUM_FACES; ++face) {
        const int count = face_counts[face];
        if (count == 0) {
            continue;
        }
        if (count > PORTS_PER_FACE) {
            mark_tile_overflow(&tileop);
            return tileop;
        }

        const int offset = face_offsets[face];
        for (int i = 0; i < count; ++i) {
            tileop.bytes[face * PORTS_PER_FACE + i] = static_cast<uint8_t>(face_data.ports[offset + i].group);
        }

        if (face == FACE_L) {
            for (int i = 0; i < count; ++i) {
                if (face_data.ports[offset + i].h1 > 0xFFU) {
                    mark_tile_overflow(&tileop);
                    return tileop;
                }
                tileop.bytes[64 + i] = static_cast<uint8_t>(face_data.ports[offset + i].h1);
            }
        } else if (face == FACE_R) {
            for (int i = 0; i < count; ++i) {
                if (face_data.ports[offset + i].h1 > 0xFFU) {
                    mark_tile_overflow(&tileop);
                    return tileop;
                }
                tileop.bytes[80 + i] = static_cast<uint8_t>(face_data.ports[offset + i].h1);
            }
        }
    }

    return tileop;
}
