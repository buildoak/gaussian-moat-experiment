#pragma once
#include "types.h"
#include <cstdint>

FaceData extract_faces(const TileCoord& coord,
                       const uint32_t* bitmap, const uint32_t* prefix,
                       const uint32_t* prime_pos, int prime_count,
                       const uint16_t* parent);
