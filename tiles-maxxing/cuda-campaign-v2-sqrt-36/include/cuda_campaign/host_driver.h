#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"

namespace cuda_campaign {

struct DispatchConfig {
  std::size_t host_chunk_tiles = 200000;
  std::size_t device_slab_tiles = 0;
  std::size_t device_budget_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL +
                                    512ULL * 1024ULL * 1024ULL;
  std::size_t device_safety_bytes = 1024ULL * 1024ULL * 1024ULL;
  int ring_slots = 3;
};

struct DispatchStats {
  std::size_t tiles = 0;
  std::size_t chunks = 0;
  std::size_t slabs = 0;
  std::size_t host_chunk_tiles = 0;
  std::size_t device_slab_tiles = 0;
  std::size_t phase1_peak_bytes = 0;
  std::size_t phase2_peak_bytes = 0;
  std::size_t pinned_host_bytes = 0;
  int stream_count = 0;
  std::string device_name;
};

std::size_t phase1_bytes_for_tiles(std::size_t tiles);
std::size_t phase2_bytes_for_tiles(std::size_t tiles);
std::size_t pinned_bytes_for_tiles(std::size_t tiles, int ring_slots);

void dispatch_tile_batch(const campaign::TileCoord* tiles,
                         std::size_t count,
                         const campaign::CampaignConstants& constants,
                         campaign::TileOp* output_tileops,
                         const DispatchConfig& config = DispatchConfig{},
                         DispatchStats* stats = nullptr);

std::vector<campaign::TileOp> dispatch_tile_batch(
    const std::vector<campaign::TileCoord>& tiles,
    const campaign::CampaignConstants& constants,
    const DispatchConfig& config = DispatchConfig{},
    DispatchStats* stats = nullptr);

}  // namespace cuda_campaign
