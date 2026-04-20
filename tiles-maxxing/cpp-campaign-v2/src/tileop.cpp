// src/tileop.cpp
//
// STUB — Phase 2 M4 implements the full pipeline. Phase 1 writes an
// EMPTY_BIT TileOp to exercise the struct wire layout.

#include "campaign/tileop.h"

#include <cstring>

namespace campaign {

void process_tile(const TileCoord& /*coord*/,
                  const Grid& /*grid*/,
                  const CampaignConstants& /*constants*/,
                  TileOp* out) {
  std::memset(out, 0, sizeof(TileOp));
  out->tile_flags = EMPTY_BIT;  // Phase 1 stub: emit empty
}

}  // namespace campaign
