#include "encode.h"

#include "constants.h"

#include <cassert>
#include <cstring>

namespace {

struct FaceCounts {
    uint8_t counts[NUM_FACES];
};

inline uint8_t pack_h1(uint16_t h1) {
    return static_cast<uint8_t>(h1 >> 1);
}

inline uint16_t decode_h1(uint8_t packed_h1, uint8_t parity) {
    return static_cast<uint16_t>(2U * static_cast<uint16_t>(packed_h1) + parity);
}

FaceCounts count_ports_by_face(const FaceData& face_data) {
    FaceCounts face_counts{};
    for (int i = 0; i < face_data.port_count; ++i) {
        const int face = face_data.ports[i].face;
        if (face < 0 || face >= NUM_FACES) {
            continue;
        }
        ++face_counts.counts[face];
    }
    return face_counts;
}

void append_face_groups(TileOp* tileop, int* cursor, const FaceData& face_data, int face) {
    for (int i = 0; i < face_data.port_count; ++i) {
        const Port& port = face_data.ports[i];
        if (port.face != face) {
            continue;
        }
        tileop->bytes[*cursor] = static_cast<uint8_t>(port.group);
        ++(*cursor);
    }
}

void append_face_h1(TileOp* tileop, int* cursor, const FaceData& face_data, int face) {
    for (int i = 0; i < face_data.port_count; ++i) {
        const Port& port = face_data.ports[i];
        if (port.face != face) {
            continue;
        }
        tileop->bytes[*cursor] = pack_h1(port.h1);
        ++(*cursor);
    }
}

uint8_t fixed_coordinate_parity(const TileCoord& coord, int face) {
    switch (face) {
        case FACE_I:
            return static_cast<uint8_t>(coord.b_lo & 1LL);
        case FACE_O:
            return static_cast<uint8_t>((coord.b_lo + TILE_SIDE) & 1LL);
        case FACE_L:
            return static_cast<uint8_t>(coord.a_lo & 1LL);
        case FACE_R:
            return static_cast<uint8_t>((coord.a_lo + TILE_SIDE) & 1LL);
        default:
            return 0;
    }
}

}  // namespace

TileOp make_overflow_tileop() {
    TileOp tileop;
    std::memset(tileop.bytes, OVERFLOW_SENTINEL, sizeof(tileop.bytes));
    return tileop;
}

TileOp make_empty_tileop() {
    TileOp tileop;
    std::memset(&tileop, 0, sizeof(tileop));
    tileop.bytes[0] = EMPTY_OFFSET;
    tileop.bytes[1] = EMPTY_OFFSET;
    tileop.bytes[2] = EMPTY_OFFSET;
    return tileop;
}

TileOpFaceView tileop_face_view(const TileOpLayout& layout, int face) {
    if (!layout.is_valid || layout.is_empty || layout.is_overflow || face < 0 || face >= NUM_FACES) {
        return TileOpFaceView{nullptr, nullptr, 0};
    }

    return layout.faces[face];
}

TileOpLayout parse_tileop_v2(const TileOp& tileop) {
    TileOpLayout layout{};

    if (tileop.bytes[0] == OVERFLOW_SENTINEL) {
        layout.is_valid = true;
        layout.is_overflow = true;
        return layout;
    }

    layout.off_I = tileop.bytes[0];
    layout.off_L = tileop.bytes[1];
    layout.off_R = tileop.bytes[2];

    if (!(layout.off_I >= TILEOP_HEADER_BYTES &&
          layout.off_I <= layout.off_L &&
          layout.off_L <= layout.off_R &&
          layout.off_R <= TILEOP_SIZE)) {
        return layout;
    }

    if (layout.off_I == EMPTY_OFFSET &&
        layout.off_L == EMPTY_OFFSET &&
        layout.off_R == EMPTY_OFFSET &&
        tileop.bytes[3] == 0) {
        layout.is_valid = true;
        layout.is_empty = true;
        layout.payload_bytes_used = 0;
        layout.payload_slack = TILEOP_PAYLOAD_BYTES;
        layout.faces[FACE_O] = TileOpFaceView{&tileop.bytes[TILEOP_HEADER_BYTES], nullptr, 0};
        layout.faces[FACE_I] = TileOpFaceView{&tileop.bytes[TILEOP_HEADER_BYTES], nullptr, 0};
        layout.faces[FACE_L] = TileOpFaceView{&tileop.bytes[TILEOP_HEADER_BYTES], &tileop.bytes[TILEOP_HEADER_BYTES], 0};
        layout.faces[FACE_R] = TileOpFaceView{&tileop.bytes[TILEOP_HEADER_BYTES], &tileop.bytes[TILEOP_HEADER_BYTES], 0};
        return layout;
    }

    layout.o_cnt = static_cast<uint8_t>(layout.off_I - TILEOP_HEADER_BYTES);
    layout.i_cnt = static_cast<uint8_t>(layout.off_L - layout.off_I);
    layout.l_cnt = static_cast<uint8_t>(layout.off_R - layout.off_L);

    const int residual = TILEOP_SIZE - static_cast<int>(layout.off_R) - static_cast<int>(layout.l_cnt);
    if (residual < 0) {
        return layout;
    }

    layout.r_cnt = static_cast<uint8_t>(residual / 2);
    layout.h_start = static_cast<uint8_t>(layout.off_R + layout.r_cnt);
    if (static_cast<int>(layout.h_start) + static_cast<int>(layout.l_cnt) + static_cast<int>(layout.r_cnt) > TILEOP_SIZE) {
        return layout;
    }

    layout.payload_bytes_used = static_cast<uint8_t>(
        layout.o_cnt + layout.i_cnt + 2 * layout.l_cnt + 2 * layout.r_cnt);
    layout.payload_slack = static_cast<uint8_t>(TILEOP_PAYLOAD_BYTES - layout.payload_bytes_used);
    layout.is_valid = true;

    layout.faces[FACE_O] = TileOpFaceView{&tileop.bytes[TILEOP_HEADER_BYTES], nullptr, layout.o_cnt};
    layout.faces[FACE_I] = TileOpFaceView{&tileop.bytes[layout.off_I], nullptr, layout.i_cnt};
    layout.faces[FACE_L] = TileOpFaceView{&tileop.bytes[layout.off_L], &tileop.bytes[layout.h_start], layout.l_cnt};
    layout.faces[FACE_R] = TileOpFaceView{
        &tileop.bytes[layout.off_R],
        &tileop.bytes[layout.h_start + layout.l_cnt],
        layout.r_cnt
    };

    return layout;
}

uint8_t max_group_label(const TileOp& tileop) {
    const TileOpLayout layout = parse_tileop_v2(tileop);
    if (!layout.is_valid || layout.is_empty || layout.is_overflow) {
        return 0;
    }

    uint8_t max_label = 0;
    for (int face = 0; face < NUM_FACES; ++face) {
        const TileOpFaceView view = tileop_face_view(layout, face);
        for (uint8_t i = 0; i < view.count; ++i) {
            if (view.groups[i] > max_label) {
                max_label = view.groups[i];
            }
        }
    }
    return max_label;
}

uint16_t face_h1(const TileCoord& coord, int face, uint8_t packed_h1) {
    const uint8_t parity = static_cast<uint8_t>(1U - fixed_coordinate_parity(coord, face));
    return decode_h1(packed_h1, parity);
}

TileOp encode_tileop(const FaceData& face_data) {
    TileOp tileop = make_empty_tileop();

    if (face_data.group_count >= OVERFLOW_SENTINEL) {
        return make_overflow_tileop();
    }

    const FaceCounts face_counts = count_ports_by_face(face_data);
    const int o_cnt = face_counts.counts[FACE_O];
    const int i_cnt = face_counts.counts[FACE_I];
    const int l_cnt = face_counts.counts[FACE_L];
    const int r_cnt = face_counts.counts[FACE_R];

    if (o_cnt + i_cnt + 2 * l_cnt + 2 * r_cnt > TILEOP_PAYLOAD_BYTES) {
        return make_overflow_tileop();
    }

    tileop.bytes[0] = static_cast<uint8_t>(TILEOP_HEADER_BYTES + o_cnt);
    tileop.bytes[1] = static_cast<uint8_t>(TILEOP_HEADER_BYTES + o_cnt + i_cnt);
    tileop.bytes[2] = static_cast<uint8_t>(TILEOP_HEADER_BYTES + o_cnt + i_cnt + l_cnt);

    const int off_R = TILEOP_HEADER_BYTES + o_cnt + i_cnt + l_cnt;
    // The parser derives r_cnt from the budget formula, not from actual R port count.
    // We must place h1 bytes at the parser's expected h_start = off_R + derived_r_cnt.
    // Excess R group slots between actual_r and derived_r remain zero (group=0 = unused).
    const int derived_r_cnt = (TILEOP_SIZE - off_R - l_cnt) / 2;
    const int h_start = off_R + derived_r_cnt;

    int cursor = TILEOP_HEADER_BYTES;
    append_face_groups(&tileop, &cursor, face_data, FACE_O);
    append_face_groups(&tileop, &cursor, face_data, FACE_I);
    append_face_groups(&tileop, &cursor, face_data, FACE_L);
    append_face_groups(&tileop, &cursor, face_data, FACE_R);

    // Skip to h_start — zero-padded R group slots are already 0 from make_empty_tileop()
    cursor = h_start;
    append_face_h1(&tileop, &cursor, face_data, FACE_L);
    append_face_h1(&tileop, &cursor, face_data, FACE_R);

    assert(cursor <= TILEOP_SIZE);
    return tileop;
}
