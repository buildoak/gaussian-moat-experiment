// include/campaign/snapshot.h
//
// Binary snapshot writer + manifest schema for the cpp-campaign-v2 reference
// build.
//
// Snapshot file layout (plan §3 C10 / §4 M5 brief):
//
//   [header, little-endian integers]
//     uint8   magic[4]                  = "CMV2"
//     uint32  version                   = 1
//     uint8   grid_params_hash[32]     // SHA-256 of canonical grid fields
//     uint8   constants_hash[32]       // SHA-256 of campaign_constants canonical
//     uint8   mr_witness_set_sha256[32]// SHA-256 of pinned MR witness set
//     uint64  tile_count
//     uint32  bytes_per_tile            // = 256
//     uint8   reserved[4]               // zero padding to 8-byte boundary
//
//   [payload]
//     tile_count * bytes_per_tile bytes of TileOp records, emitted in sorted
//     (i, j) order. The tileops vector is indexed by Grid::flat_index(i, j).
//
// Sidecar `<snapshot path>.manifest.json` sibling file:
//
//   {
//     "schema_version": 1,
//     "grid_params_hash": "<hex>",
//     "constants_hash":   "<hex>",
//     "mr_witness_set_sha256": "<hex>",
//     "tile_count": <int>,
//     "bytes_per_tile": 256,
//     "k_sq": <int>,
//     "r_inner": <int>,
//     "r_outer": <int>,
//     "generated_at": "<ISO8601 UTC>"
//   }
//
// CUDA-port parity: all three hashes must match. Any divergence fails
// `compare_snapshots`.
//
// Dependencies: grid.h, tileop.h, campaign_constants.h.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"

namespace campaign {

// Fixed magic + version for the cpp-campaign-v2 snapshot format.
inline constexpr char kSnapshotMagic[4] = {'C', 'M', 'V', '2'};
inline constexpr std::uint32_t kSnapshotMagicLe = 0x32564d43u;
inline constexpr std::uint32_t kSnapshotVersion = 1u;
inline constexpr std::uint32_t kSnapshotHeaderSize = 120u;
inline constexpr const char* kSnapshotTimestampEnv =
    "CAMPAIGN_SNAPSHOT_TIMESTAMP_UTC";

// Snapshot sidecar options. `generated_at_utc`, when set, is copied directly
// into the manifest so tests can freeze timestamps without changing payload
// bytes. If unset, `CAMPAIGN_SNAPSHOT_TIMESTAMP_UTC` is honored before falling
// back to the current UTC time.
struct SnapshotOptions {
  std::optional<std::string> generated_at_utc;
};

// SHA-256 hex digest of the canonical grid fields carried in the snapshot
// header.
std::string grid_params_hash(const Grid& grid);

// SHA-256 hex digest of the compact tower-table grid description. Unlike
// grid_params_hash(), this does not enumerate every active tile and is suitable
// for profile metadata on wide annuli.
std::string grid_tower_table_hash(const Grid& grid);

class SnapshotWriter {
 public:
  SnapshotWriter(const std::filesystem::path& path,
                 const Grid& grid,
                 const CampaignConstants& constants,
                 SnapshotOptions options = {});
  SnapshotWriter(const SnapshotWriter&) = delete;
  SnapshotWriter& operator=(const SnapshotWriter&) = delete;
  SnapshotWriter(SnapshotWriter&&) = delete;
  SnapshotWriter& operator=(SnapshotWriter&&) = delete;

  void append_column(std::int32_t i, std::span<const TileOp> tileops);
  void close();

 private:
  std::filesystem::path path_;
  std::ofstream out_;
  const Grid* grid_ = nullptr;
  const CampaignConstants* constants_ = nullptr;
  SnapshotOptions options_;
  std::uint64_t tile_count_ = 0;
  std::uint64_t written_tiles_ = 0;
  std::int32_t next_i_ = 0;
  std::string grid_hash_hex_;
  std::string constants_hash_hex_;
  std::string mr_hash_hex_;
  bool closed_ = false;
};

// Write snapshot.bin + `<path>.manifest.json`.
//
// Preconditions:
//   * tileops.size() == grid.total_tiles.
//   * tileops[grid.flat_index(i, j)] belongs to tile (i, j).
//   * grid has been built; constants has been built.
//
// Throws:
//   * std::runtime_error on file I/O failures.
void write_snapshot(const std::filesystem::path& path,
                    const Grid& grid,
                    const std::vector<TileOp>& tileops,
                    const CampaignConstants& constants,
                    SnapshotOptions options = {});

// Snapshot header view (read-only). Used by `compare_snapshots`.
struct SnapshotHeader {
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint8_t grid_params_hash[32] = {};
  std::uint8_t constants_hash[32] = {};
  std::uint8_t mr_witness_set_sha256[32] = {};
  std::uint64_t tile_count = 0;
  std::uint32_t bytes_per_tile = 0;
  std::uint32_t reserved_padding = 0;
};

static_assert(kSnapshotHeaderSize == 4 + 4 + 32 + 32 + 32 + 8 + 4 + 4,
              "SnapshotHeader size must match wire layout");

// Parse just the snapshot header. For `compare_snapshots` header-first diff.
SnapshotHeader read_snapshot_header(const std::filesystem::path& path);

}  // namespace campaign
