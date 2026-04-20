// tests/test_region.cpp
//
// Exercises Region factories and JSON parsing.

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "campaign/grid.h"
#include "campaign/region.h"

namespace {

std::string write_tmp_json(const std::string& name, const std::string& body) {
  const std::string path = std::string("/tmp/cpp_campaign_v2_") + name + ".json";
  std::ofstream out(path);
  out << body;
  out.close();
  return path;
}

TEST(Region, FromTileBoxRoundtrip) {
  std::vector<campaign::JRange> ranges = {
      {312500, 312502}, {312500, 312501}};
  auto r = campaign::Region::from_tile_box(0, 1, ranges);
  EXPECT_FALSE(r.is_full_octant);
  EXPECT_EQ(r.i_lo, 0);
  EXPECT_EQ(r.i_hi, 1);
  EXPECT_EQ(r.tile_count(), 5);
  EXPECT_EQ(r.column_slice(0).j_lo, 312500);
  EXPECT_EQ(r.column_slice(0).j_hi, 312502);
  EXPECT_EQ(r.column_slice(1).j_lo, 312500);
  EXPECT_EQ(r.column_slice(1).j_hi, 312501);
}

TEST(Region, FromTileBoxRejectsMismatchedSize) {
  std::vector<campaign::JRange> ranges = {{0, 0}};
  EXPECT_THROW(campaign::Region::from_tile_box(0, 2, ranges),
               std::invalid_argument);
}

TEST(Region, FromTileBoxRejectsInvertedRange) {
  std::vector<campaign::JRange> ranges;
  EXPECT_THROW(campaign::Region::from_tile_box(5, 3, ranges),
               std::invalid_argument);
}

TEST(Region, ParseJsonFullOctant) {
  const auto path = write_tmp_json("full_octant", R"({"full_octant": true})");
  auto r = campaign::Region::from_json_file(path);
  EXPECT_TRUE(r.is_full_octant);
}

TEST(Region, ParseJsonTileBox) {
  const std::string body = R"({
    "i_lo": 0,
    "i_hi": 1,
    "columns": [
      {"i": 0, "j_lo": 10, "j_hi": 12},
      {"i": 1, "j_lo": 10, "j_hi": 11}
    ]
  })";
  const auto path = write_tmp_json("tile_box", body);
  auto r = campaign::Region::from_json_file(path);
  EXPECT_FALSE(r.is_full_octant);
  EXPECT_EQ(r.i_lo, 0);
  EXPECT_EQ(r.i_hi, 1);
  EXPECT_EQ(r.tile_count(), 5);
  EXPECT_EQ(r.column_slice(0).size(), 3);
  EXPECT_EQ(r.column_slice(1).size(), 2);
}

TEST(Region, ParseJsonTileListPreservesExactSparseTiles) {
  const std::string body = R"({
    "k_sq": 36,
    "r_inner": 80000000,
    "r_outer": 80008192,
    "offset": [1, 1],
    "tiles": [
      {"i": 117187, "j": 289695},
      {"i": 117187, "j": 289696},
      {"i": 117187, "j": 289710},
      {"i": 117187, "j": 289729},
      {"i": 117188, "j": 289696}
    ]
  })";
  const auto path = write_tmp_json("tile_list", body);
  auto r = campaign::Region::from_json_file(path);
  EXPECT_FALSE(r.is_full_octant);
  EXPECT_TRUE(r.is_explicit_tile_list);
  EXPECT_EQ(r.i_lo, 117187);
  EXPECT_EQ(r.i_hi, 117188);
  EXPECT_EQ(r.tile_count(), 5);

  ASSERT_EQ(r.tiles().size(), 5u);
  EXPECT_EQ(r.tiles()[0].i, 117187);
  EXPECT_EQ(r.tiles()[0].j, 289695);
  EXPECT_EQ(r.tiles()[4].i, 117188);
  EXPECT_EQ(r.tiles()[4].j, 289696);

  const auto col0 = r.column_slices(117187);
  ASSERT_EQ(col0.size(), 3u);
  EXPECT_EQ(col0[0].j_lo, 289695);
  EXPECT_EQ(col0[0].j_hi, 289696);
  EXPECT_EQ(col0[1].j_lo, 289710);
  EXPECT_EQ(col0[1].j_hi, 289710);
  EXPECT_EQ(col0[2].j_lo, 289729);
  EXPECT_EQ(col0[2].j_hi, 289729);

  const auto col1 = r.column_slices(117188);
  ASSERT_EQ(col1.size(), 1u);
  EXPECT_EQ(col1[0].j_lo, 289696);
  EXPECT_EQ(col1[0].j_hi, 289696);
}

TEST(Region, ParseJsonRejectsAmbiguousTileListAndBox) {
  const std::string body = R"({
    "i_lo": 0,
    "i_hi": 0,
    "columns": [{"i": 0, "j_lo": 0, "j_hi": 0}],
    "tiles": [{"i": 0, "j": 0}]
  })";
  const auto path = write_tmp_json("ambiguous", body);
  EXPECT_THROW(campaign::Region::from_json_file(path),
               std::invalid_argument);
}

TEST(Region, ParseJsonRejectsMissingColumn) {
  const std::string body = R"({
    "i_lo": 0,
    "i_hi": 2,
    "columns": [
      {"i": 0, "j_lo": 10, "j_hi": 12},
      {"i": 2, "j_lo": 10, "j_hi": 11}
    ]
  })";
  const auto path = write_tmp_json("missing_col", body);
  EXPECT_THROW(campaign::Region::from_json_file(path),
               std::invalid_argument);
}

TEST(Region, ParseJsonRejectsDuplicateColumn) {
  const std::string body = R"({
    "i_lo": 0,
    "i_hi": 1,
    "columns": [
      {"i": 0, "j_lo": 10, "j_hi": 12},
      {"i": 0, "j_lo": 10, "j_hi": 11}
    ]
  })";
  const auto path = write_tmp_json("dup_col", body);
  EXPECT_THROW(campaign::Region::from_json_file(path),
               std::invalid_argument);
}

TEST(Region, ParseJsonRejectsInvertedJRange) {
  const std::string body = R"({
    "i_lo": 0,
    "i_hi": 0,
    "columns": [ {"i": 0, "j_lo": 20, "j_hi": 10} ]
  })";
  const auto path = write_tmp_json("inverted_j", body);
  EXPECT_THROW(campaign::Region::from_json_file(path),
               std::invalid_argument);
}

TEST(Region, ParseJsonRejectsMalformedJson) {
  const auto path = write_tmp_json("bad_json", "{not-json");
  EXPECT_THROW(campaign::Region::from_json_file(path),
               std::invalid_argument);
}

TEST(Region, ParseJsonMissingFileThrowsRuntime) {
  EXPECT_THROW(campaign::Region::from_json_file("/tmp/does_not_exist_9hZ.json"),
               std::runtime_error);
}

TEST(Region, FullOctantFromBuiltGrid) {
  // Tiny radii — exercise the resolver end-to-end.
  auto g = campaign::Grid::build(10000, 10032, campaign::k_sq_value);
  auto r = campaign::Region::full_octant(g);
  EXPECT_FALSE(r.is_full_octant);
  EXPECT_EQ(r.tile_count(), g.total_tiles);
}

}  // namespace
