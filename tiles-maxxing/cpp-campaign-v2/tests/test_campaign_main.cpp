// tests/test_campaign_main.cpp

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "campaign/constants.h"
#include "campaign/snapshot.h"

namespace {

struct CommandResult {
  int exit_code = -1;
  std::string out;
  std::string err;
};

std::filesystem::path temp_dir_for(const std::string& name) {
  const auto base = std::filesystem::temp_directory_path() /
                    ("cpp_campaign_v2_main_" + name);
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  return base;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(in),
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

CommandResult run_command(const std::string& cmd,
                          const std::filesystem::path& dir,
                          const std::string& stem) {
  const auto out_path = dir / (stem + ".out");
  const auto err_path = dir / (stem + ".err");
  const std::string full_cmd = cmd + " > " + shell_quote(out_path) +
                               " 2> " + shell_quote(err_path);
  const int rc = std::system(full_cmd.c_str());
  CommandResult result;
  result.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
  result.out = read_text(out_path);
  result.err = read_text(err_path);
  return result;
}

std::filesystem::path write_region3(const std::filesystem::path& dir) {
  const auto path = dir / "region3.json";
  std::ofstream out(path);
  out << "{\n"
      << "  \"i_lo\": 0,\n"
      << "  \"i_hi\": 0,\n"
      << "  \"columns\": [\n"
      << "    {\"i\": 0, \"j_lo\": 3, \"j_hi\": 5}\n"
      << "  ]\n"
      << "}\n";
  return path;
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

std::string campaign_cmd(const std::filesystem::path& out,
                         const std::filesystem::path& region,
                         int threads) {
  std::ostringstream cmd;
  cmd << "OMP_NUM_THREADS=" << threads << " "
      << shell_quote(CAMPAIGN_MAIN_PATH)
      << " --k-sq=" << campaign::k_sq_value
      << " --r-inner=1000"
      << " --r-outer=1600"
      << " --region " << shell_quote(region)
      << " --out " << shell_quote(out);
  return cmd.str();
}

std::string campaign_cmd_no_snapshot(const std::filesystem::path& region,
                                     int threads) {
  std::ostringstream cmd;
  cmd << "OMP_NUM_THREADS=" << threads << " "
      << shell_quote(CAMPAIGN_MAIN_PATH)
      << " --k-sq=" << campaign::k_sq_value
      << " --r-inner=1000"
      << " --r-outer=1600"
      << " --region " << shell_quote(region);
  return cmd.str();
}

CommandResult run_compare(const std::filesystem::path& a,
                          const std::filesystem::path& b,
                          const std::filesystem::path& dir,
                          const std::string& stem) {
  const std::string cmd = shell_quote(COMPARE_SNAPSHOTS_PATH) + " " +
                          shell_quote(a) + " " + shell_quote(b);
  return run_command(cmd, dir, stem);
}

}  // namespace

TEST(CampaignMain, TinyEndToEndWritesSnapshotManifestAndVerdict) {
  const auto dir = temp_dir_for("tiny_e2e");
  const auto region = write_region3(dir);
  const auto snapshot = dir / "snapshot.bin";

  const auto result = run_command(campaign_cmd(snapshot, region, 2), dir, "run");

  EXPECT_EQ(result.exit_code, 0) << result.err;
  EXPECT_NE(result.out.find("VERDICT: "), std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(snapshot));
  EXPECT_TRUE(std::filesystem::exists(manifest_path_for(snapshot)));

  std::ifstream manifest_in(manifest_path_for(snapshot));
  nlohmann::json manifest;
  manifest_in >> manifest;
  EXPECT_EQ(manifest.at("schema_version"), 1);
  EXPECT_EQ(manifest.at("tile_count"), 3);
  EXPECT_EQ(manifest.at("bytes_per_tile"), campaign::TILEOP_SIZE);
  EXPECT_EQ(manifest.at("k_sq"), campaign::k_sq_value);
}

TEST(CampaignMain, DefaultDoesNotWriteSnapshot) {
  const auto dir = temp_dir_for("default_no_snapshot");
  const auto region = write_region3(dir);

  const auto result =
      run_command(campaign_cmd_no_snapshot(region, 2), dir, "run");

  EXPECT_EQ(result.exit_code, 0) << result.err;
  EXPECT_NE(result.out.find("VERDICT: "), std::string::npos);
  EXPECT_NE(result.out.find("snapshot:      disabled"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dir / "snapshot.bin"));
  EXPECT_FALSE(std::filesystem::exists(dir / "snapshot.manifest.json"));
}

TEST(CampaignMain, SnapshotOutWritesSnapshot) {
  const auto dir = temp_dir_for("snapshot_out");
  const auto region = write_region3(dir);
  const auto snapshot = dir / "snapshot.bin";
  std::ostringstream cmd;
  cmd << campaign_cmd_no_snapshot(region, 2)
      << " --snapshot-out " << shell_quote(snapshot);

  const auto result = run_command(cmd.str(), dir, "run");

  EXPECT_EQ(result.exit_code, 0) << result.err;
  EXPECT_NE(result.out.find("VERDICT: "), std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(snapshot));
  EXPECT_TRUE(std::filesystem::exists(manifest_path_for(snapshot)));
}

TEST(CampaignMain, OutAliasMayMatchSnapshotOut) {
  const auto dir = temp_dir_for("matching_alias");
  const auto region = write_region3(dir);
  const auto snapshot = dir / "snapshot.bin";
  std::ostringstream cmd;
  cmd << campaign_cmd_no_snapshot(region, 2)
      << " --out " << shell_quote(snapshot)
      << " --snapshot-out " << shell_quote(snapshot);

  const auto result = run_command(cmd.str(), dir, "run");

  EXPECT_EQ(result.exit_code, 0) << result.err;
  EXPECT_TRUE(std::filesystem::exists(snapshot));
}

TEST(CampaignMain, OutAndSnapshotOutConflictFails) {
  const auto dir = temp_dir_for("conflict_alias");
  const auto region = write_region3(dir);
  const auto out = dir / "out.bin";
  const auto snapshot = dir / "snapshot.bin";
  std::ostringstream cmd;
  cmd << campaign_cmd_no_snapshot(region, 2)
      << " --out " << shell_quote(out)
      << " --snapshot-out " << shell_quote(snapshot);

  const auto result = run_command(cmd.str(), dir, "run");

  EXPECT_EQ(result.exit_code, 2);
  EXPECT_NE(result.err.find("--out and --snapshot-out differ"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(out));
  EXPECT_FALSE(std::filesystem::exists(snapshot));
}

TEST(CampaignMain, ThicknessFailExitsOneWithClearError) {
  const auto dir = temp_dir_for("thin");
  const auto region = write_region3(dir);
  const auto snapshot = dir / "snapshot.bin";
  std::ostringstream cmd;
  cmd << shell_quote(CAMPAIGN_MAIN_PATH)
      << " --k-sq=" << campaign::k_sq_value
      << " --r-inner=10000"
      << " --r-outer=10032"
      << " --region " << shell_quote(region)
      << " --out " << shell_quote(snapshot);

  const auto result = run_command(cmd.str(), dir, "thin");

  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("ERROR: annulus too thin"), std::string::npos);
  EXPECT_NE(result.err.find("Pipeline soundness requires"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(snapshot));
}

TEST(CampaignMain, ThreadingDeterminismOneVsTwelve) {
  const auto dir = temp_dir_for("threads");
  const auto region = write_region3(dir);
  const auto one = dir / "one.bin";
  const auto twelve = dir / "twelve.bin";

  const auto one_result = run_command(campaign_cmd(one, region, 1), dir, "one");
  const auto twelve_result =
      run_command(campaign_cmd(twelve, region, 12), dir, "twelve");
  ASSERT_EQ(one_result.exit_code, 0) << one_result.err;
  ASSERT_EQ(twelve_result.exit_code, 0) << twelve_result.err;

  const auto compare = run_compare(one, twelve, dir, "compare_threads");
  EXPECT_EQ(compare.exit_code, 0) << compare.err;
  EXPECT_EQ(read_bytes(one), read_bytes(twelve));
}

TEST(CampaignMain, RegionSubsetWritesOnlyRequestedTiles) {
  const auto dir = temp_dir_for("region_subset");
  const auto region = write_region3(dir);
  const auto snapshot = dir / "snapshot.bin";

  const auto result = run_command(campaign_cmd(snapshot, region, 2), dir, "run");
  ASSERT_EQ(result.exit_code, 0) << result.err;

  const auto header = campaign::read_snapshot_header(snapshot);
  EXPECT_EQ(header.tile_count, 3u);

  std::ifstream manifest_in(manifest_path_for(snapshot));
  nlohmann::json manifest;
  manifest_in >> manifest;
  EXPECT_EQ(manifest.at("tile_count"), 3);
}

TEST(CampaignMain, RepeatDeterminism) {
  const auto dir = temp_dir_for("repeat");
  const auto region = write_region3(dir);
  std::vector<std::vector<unsigned char>> snapshots;

  for (int run = 0; run < 3; ++run) {
    const auto snapshot = dir / ("snapshot_" + std::to_string(run) + ".bin");
    const auto result =
        run_command(campaign_cmd(snapshot, region, 4), dir,
                    "run_" + std::to_string(run));
    ASSERT_EQ(result.exit_code, 0) << result.err;
    snapshots.push_back(read_bytes(snapshot));
  }

  EXPECT_EQ(snapshots[0], snapshots[1]);
  EXPECT_EQ(snapshots[1], snapshots[2]);
}
