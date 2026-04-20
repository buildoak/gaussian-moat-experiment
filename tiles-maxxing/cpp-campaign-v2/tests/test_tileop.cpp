// tests/test_tileop.cpp
//
// PLACEHOLDER — M4 fills in dense-remap, face-strip UF, 256 B encode tests.

#include <gtest/gtest.h>

#include "campaign/tileop.h"

TEST(TileOp, Placeholder) {
  EXPECT_EQ(1, 1);
}

// Lock the 256 B size up front — it's a wire-format promise to Phase 2.
TEST(TileOp, SizeIs256Bytes) {
  static_assert(sizeof(campaign::TileOp) == 256, "TileOp must be 256 B");
  EXPECT_EQ(sizeof(campaign::TileOp), 256u);
}

TEST(TileOp, OffsetsAreLocked) {
  EXPECT_EQ(offsetof(campaign::TileOp, n), 0u);
  EXPECT_EQ(offsetof(campaign::TileOp, face_groups), 4u);
  EXPECT_EQ(offsetof(campaign::TileOp, inner_flags), 196u);
  EXPECT_EQ(offsetof(campaign::TileOp, outer_flags), 212u);
  EXPECT_EQ(offsetof(campaign::TileOp, tile_flags), 228u);
  EXPECT_EQ(offsetof(campaign::TileOp, reserved), 229u);
}
