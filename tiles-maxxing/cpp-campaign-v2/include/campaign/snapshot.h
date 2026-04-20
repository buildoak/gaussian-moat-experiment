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
                    const CampaignConstants& constants);

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
