#pragma once
#include "types.h"

TileOp make_overflow_tileop();
TileOp make_empty_tileop();
TileOpLayout parse_tileop_v2(const TileOp& tileop);
TileOpFaceView tileop_face_view(const TileOpLayout& layout, int face);
uint8_t max_group_label(const TileOp& tileop);
uint16_t face_h1(const TileCoord& coord, int face, uint8_t packed_h1);

TileOp encode_tileop(const FaceData& face_data);
