// tests/test_snapshot.cpp

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/snapshot.h"
#include "campaign/tileop.h"

namespace {

struct CommandResult {
  int exit_code = -1;
  std::string out;
  std::string err;
};

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>());
}

std::string shell_quote(const std::filesystem::path& path) {
  std::string s = path.string();
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

CommandResult run_compare(const std::filesystem::path& a,
                          const std::filesystem::path& b,
                          const std::filesystem::path& work_dir) {
  const auto out_path = work_dir / "compare.out";
  const auto err_path = work_dir / "compare.err";
  const std::string cmd = shell_quote(COMPARE_SNAPSHOTS_PATH) + " " +
                          shell_quote(a) + " " + shell_quote(b) +
                          " > " + shell_quote(out_path) +
                          " 2> " + shell_quote(err_path);
  const int rc = std::system(cmd.c_str());
  CommandResult result;
  result.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
  result.out = read_text(out_path);
  result.err = read_text(err_path);
  return result;
}

std::filesystem::path temp_dir_for(const std::string& name) {
  const auto base = std::filesystem::temp_directory_path() /
                    ("cpp_campaign_v2_" + name);
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  return base;
}

std::filesystem::path manifest_path_for(const std::filesystem::path& path) {
  const std::string filename = path.filename().string();
  const std::string snapshot_suffix = ".snapshot.bin";
  const std::string bin_suffix = ".bin";
  std::string stem;
  if (filename.size() > snapshot_suffix.size() &&
      filename.compare(filename.size() - snapshot_suffix.size(),
                       snapshot_suffix.size(), snapshot_suffix) == 0) {
    stem = filename.substr(0, filename.size() - snapshot_suffix.size());
  } else if (filename.size() > bin_suffix.size() &&
             filename.compare(filename.size() - bin_suffix.size(),
                              bin_suffix.size(), bin_suffix) == 0) {
    stem = filename.substr(0, filename.size() - bin_suffix.size());
  } else {
    stem = path.stem().string();
  }
  return path.parent_path() / (stem + ".manifest.json");
}

campaign::Grid synthetic_grid() {
  campaign::Grid g{};
  g.R_inner = 10000;
  g.R_outer = 10032;
  g.R_inner_sq = g.R_inner * g.R_inner;
  g.R_outer_sq = g.R_outer * g.R_outer;
  g.K_SQ_value = campaign::k_sq_value;
  g.S_value = campaign::S;
  g.C_value = campaign::C;
  g.o_x = campaign::OFFSET_X;
  g.o_y = campaign::OFFSET_Y;
  g.i_min = 0;
  g.i_max = 1;
  g.j_low = {10, 10};
  g.j_high = {12, 11};
  g.tower_offset = {0, 3, 5};
  g.total_tiles = 5;
  return g;
}

campaign::CampaignConstants synthetic_constants(std::uint64_t r_inner = 10000,
                                                 std::uint64_t r_outer = 10032) {
  return campaign::CampaignConstants::from_radii(
      r_inner, r_outer, campaign::k_sq_value);
}

campaign::TileOp make_tile(std::uint8_t id) {
  campaign::TileOp op{};
  op.n[0] = static_cast<std::uint8_t>(id);
  op.n[1] = static_cast<std::uint8_t>(id + 1);
  op.n[2] = static_cast<std::uint8_t>(id + 2);
  op.n[3] = static_cast<std::uint8_t>(id + 3);
  for (std::size_t i = 0; i < sizeof(op.face_groups); ++i) {
    op.face_groups[i] = static_cast<std::uint8_t>(id * 17u + i);
  }
  op.inner_flags[0] = static_cast<std::uint8_t>(0x10u | id);
  op.outer_flags[0] = static_cast<std::uint8_t>(0x80u | id);
  op.tile_flags = id;
  return op;
}

std::vector<std::uint8_t> tile_bytes(const campaign::TileOp& op) {
  std::vector<std::uint8_t> out;
  out.insert(out.end(), std::begin(op.n), std::end(op.n));
  out.insert(out.end(), std::begin(op.face_groups), std::end(op.face_groups));
  out.insert(out.end(), std::begin(op.inner_flags), std::end(op.inner_flags));
  out.insert(out.end(), std::begin(op.outer_flags), std::end(op.outer_flags));
  out.push_back(op.tile_flags);
  out.insert(out.end(), std::begin(op.reserved), std::end(op.reserved));
  return out;
}

std::vector<std::vector<std::uint8_t>> read_payload_tiles(
    const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  in.seekg(campaign::kSnapshotHeaderSize, std::ios::beg);
  std::vector<std::vector<std::uint8_t>> tiles;
  for (;;) {
    std::vector<std::uint8_t> tile(campaign::TILEOP_SIZE);
    in.read(reinterpret_cast<char*>(tile.data()),
            static_cast<std::streamsize>(tile.size()));
    if (in.gcount() == 0) break;
    EXPECT_EQ(in.gcount(), static_cast<std::streamsize>(tile.size()));
    tiles.push_back(std::move(tile));
  }
  return tiles;
}

}  // namespace

TEST(Snapshot, RoundtripWritesHeaderManifestAndPayload) {
  const auto dir = temp_dir_for("roundtrip");
  const auto path = dir / "snapshot.bin";
  const auto grid = synthetic_grid();
  const auto constants = synthetic_constants();
  std::vector<campaign::TileOp> tileops;
  for (std::uint8_t i = 0; i < 5; ++i) tileops.push_back(make_tile(i));

  campaign::write_snapshot(path, grid, tileops, constants);

  const auto header = campaign::read_snapshot_header(path);
  EXPECT_EQ(header.magic, campaign::kSnapshotMagicLe);
  EXPECT_EQ(header.version, campaign::kSnapshotVersion);
  EXPECT_EQ(header.tile_count, 5u);
  EXPECT_EQ(header.bytes_per_tile, campaign::TILEOP_SIZE);

  const auto tiles = read_payload_tiles(path);
  ASSERT_EQ(tiles.size(), tileops.size());
  for (std::size_t i = 0; i < tileops.size(); ++i) {
    EXPECT_EQ(tiles[i], tile_bytes(tileops[i]));
  }

  std::ifstream manifest_in(manifest_path_for(path));
  nlohmann::json manifest;
  manifest_in >> manifest;
  EXPECT_EQ(manifest.at("schema_version"), 1);
  EXPECT_EQ(manifest.at("tile_count"), 5);
  EXPECT_EQ(manifest.at("bytes_per_tile"), 256);
  EXPECT_EQ(manifest.at("k_sq"), campaign::k_sq_value);
  EXPECT_EQ(manifest.at("r_inner"), constants.R_inner);
  EXPECT_EQ(manifest.at("r_outer"), constants.R_outer);
  EXPECT_TRUE(manifest.at("generated_at").get<std::string>().ends_with("Z"));
}

TEST(Snapshot, ManifestUsesStemConventionAndCompareFindsIt) {
  const auto dir = temp_dir_for("stem_manifest");
  const auto a = dir / "a.snapshot.bin";
  const auto b = dir / "b.snapshot.bin";
  const auto grid = synthetic_grid();
  const auto constants = synthetic_constants();
  std::vector<campaign::TileOp> tileops;
  for (std::uint8_t i = 0; i < 5; ++i) tileops.push_back(make_tile(i));

  campaign::write_snapshot(a, grid, tileops, constants);
  campaign::write_snapshot(b, grid, tileops, constants);

  EXPECT_TRUE(std::filesystem::exists(dir / "a.manifest.json"));
  EXPECT_TRUE(std::filesystem::exists(dir / "b.manifest.json"));
  EXPECT_FALSE(std::filesystem::exists(a.string() + ".manifest.json"));
  EXPECT_FALSE(std::filesystem::exists(b.string() + ".manifest.json"));

  const auto result = run_compare(a, b, dir);
  EXPECT_EQ(result.exit_code, 0) << result.err;
}

TEST(Snapshot, HeaderOnlyDiffFailsFast) {
  const auto dir = temp_dir_for("header_diff");
  const auto grid = synthetic_grid();
  std::vector<campaign::TileOp> tileops;
  for (std::uint8_t i = 0; i < 5; ++i) tileops.push_back(make_tile(i));
  const auto a = dir / "a.snapshot.bin";
  const auto b = dir / "b.snapshot.bin";

  campaign::write_snapshot(a, grid, tileops, synthetic_constants());
  campaign::write_snapshot(b, grid, tileops, synthetic_constants(10001, 10033));

  const auto result = run_compare(a, b, dir);
  EXPECT_EQ(result.exit_code, 2);
  EXPECT_NE(result.err.find("constants_hash"), std::string::npos);
}

TEST(Snapshot, PayloadDiffReportsTileAndOffset) {
  const auto dir = temp_dir_for("payload_diff");
  const auto grid = synthetic_grid();
  const auto constants = synthetic_constants();
  std::vector<campaign::TileOp> a_ops;
  std::vector<campaign::TileOp> b_ops;
  for (std::uint8_t i = 0; i < 5; ++i) {
    a_ops.push_back(make_tile(i));
    b_ops.push_back(make_tile(i));
  }
  b_ops[2].face_groups[7] ^= 0x55u;

  const auto a = dir / "a.snapshot.bin";
  const auto b = dir / "b.snapshot.bin";
  campaign::write_snapshot(a, grid, a_ops, constants);
  campaign::write_snapshot(b, grid, b_ops, constants);

  const auto result = run_compare(a, b, dir);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("tile index 2"), std::string::npos);
  EXPECT_NE(result.err.find("byte offset 11"), std::string::npos);
}

TEST(Snapshot, DeterministicBytesAcrossRepeatedWrites) {
  const auto dir = temp_dir_for("determinism");
  const auto grid = synthetic_grid();
  const auto constants = synthetic_constants();
  std::vector<campaign::TileOp> tileops;
  for (std::uint8_t i = 0; i < 5; ++i) tileops.push_back(make_tile(i));

  std::vector<std::vector<std::uint8_t>> snapshots;
  for (int i = 0; i < 3; ++i) {
    const auto path = dir / ("snapshot_" + std::to_string(i) + ".bin");
    campaign::write_snapshot(path, grid, tileops, constants);
    snapshots.push_back(read_bytes(path));
  }

  EXPECT_EQ(snapshots[0], snapshots[1]);
  EXPECT_EQ(snapshots[1], snapshots[2]);
}

TEST(Snapshot, EmitsTilesInSortedGridOrderFromFlatIndex) {
  const auto dir = temp_dir_for("sorted");
  const auto path = dir / "snapshot.bin";
  const auto grid = synthetic_grid();
  const auto constants = synthetic_constants();
  std::vector<campaign::TileOp> tileops;
  for (std::uint8_t i = 0; i < 5; ++i) tileops.push_back(make_tile(i));

  campaign::write_snapshot(path, grid, tileops, constants);

  const auto tiles = read_payload_tiles(path);
  ASSERT_EQ(tiles.size(), tileops.size());
  for (std::size_t i = 0; i < tileops.size(); ++i) {
    EXPECT_EQ(tiles[i], tile_bytes(tileops[i]));
  }
}
