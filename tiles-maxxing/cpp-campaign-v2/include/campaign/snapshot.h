// include/campaign/snapshot.h
//
// Binary snapshot writer + manifest schema for the cpp-campaign-v2
// reference build.
//
// Snapshot file layout (plan §4 M5 brief):
//
//   [header]
//     uint32  magic              = 0x53434132  ("SCA2" — "Snapshot Campaign v2")
//     uint32  version            = 2
//     uint8   grid_params_hash[32]     // SHA-256 of canonical grid fields
//     uint8   constants_hash[32]       // SHA-256 of campaign_constants canonical
//     uint8   mr_witness_set_sha256[32]// SHA-256 of pinned MR witness set
//     uint64  tile_count
//     uint32  bytes_per_tile            // = 256
//     uint32  reserved                  // 0
//
//   [payload]
//     tile_count * bytes_per_tile bytes of TileOp records, in canonical
//     column-major order (grid.enumerate_active_tiles() order).
//
// Sidecar `manifest.json` sibling file (plan §4 M5 brief):
//
//   {
//     "magic": "SCA2",
//     "version": 2,
//     "grid_params_hash": "<hex>",
//     "constants_hash":   "<hex>",
//     "mr_witness_set_sha256": "<hex>",
//     "tile_count": <int>,
//     "bytes_per_tile": 256,
//     "K_SQ": <int>,
//     "R_inner": <int>,
//     "R_outer": <int>,
//     "offset": [<ox>, <oy>],
//     "collar": <int>,
//     "region": { ... } // echoed from input region spec
//   }
//
// CUDA-port parity: all three hashes must match. Any divergence fails
// `compare_snapshots`.
//
// Dependencies: grid.h, tileop.h, campaign_constants.h, region.h.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/grid.h"
#include "campaign/region.h"
#include "campaign/tileop.h"

namespace campaign {

// Fixed magic + version for the v2 snapshot format.
inline constexpr std::uint32_t kSnapshotMagic = 0x53434132u;  // "SCA2"
inline constexpr std::uint32_t kSnapshotVersion = 2u;

// Write snapshot.bin + manifest.json to `out_path` and `out_path + ".json"`.
//
// Preconditions:
//   * tileops.size() == grid.total_tiles (or == region tile_count if a
//     sub-region was run — caller supplies the matching ordering).
//   * grid has been built; constants has been built.
//
// Throws:
//   * std::runtime_error on file I/O failures.
//
// STUB in Phase 1.
void write_snapshot(const std::string& out_path,
                    const Grid& grid,
                    const Region& region,
                    const CampaignConstants& constants,
                    const std::vector<TileOp>& tileops);

// Snapshot header view (read-only). Used by `compare_snapshots`.
struct SnapshotHeader {
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint8_t grid_params_hash[32] = {};
  std::uint8_t constants_hash[32] = {};
  std::uint8_t mr_witness_set_sha256[32] = {};
  std::uint64_t tile_count = 0;
  std::uint32_t bytes_per_tile = 0;
  std::uint32_t reserved = 0;
};

static_assert(sizeof(SnapshotHeader) == 4 + 4 + 32 + 32 + 32 + 8 + 4 + 4,
              "SnapshotHeader packed size must match wire layout");

// Parse just the snapshot header. For `compare_snapshots` header-first diff.
//
// STUB in Phase 1.
SnapshotHeader read_snapshot_header(const std::string& path);

}  // namespace campaign
