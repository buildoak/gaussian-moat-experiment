#pragma once
#include "types.h"

TileOp make_overflow_tileop();
TileOp make_empty_tileop();
TileOpLayout parse_tileop_v2(const TileOp& tileop);
TileOpFaceView tileop_face_view(const TileOpLayout& layout, int face);
uint8_t max_group_label(const TileOp& tileop);
uint8_t decode_group_id(uint8_t group_byte);
uint16_t decode_h1(uint8_t group_byte, uint8_t h1_byte);

TileOp encode_tileop(const FaceData& face_data);
