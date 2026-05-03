#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
  bool overflow_diagnostics = false;
  bool collect_stage_timings = false;
  std::size_t max_overflow_diagnostics = 10;
};

struct DispatchStats {
  struct StageTimings {
    double h2d_seconds = 0.0;
    double k1_sieve_seconds = 0.0;
    double mr_seconds = 0.0;
    double compact_seconds = 0.0;
    double uf_seconds = 0.0;
    double face_encode_seconds = 0.0;
    double face_sort_pack_seconds = 0.0;
    double overflow_summary_seconds = 0.0;
    double d2h_seconds = 0.0;
  };

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
  std::uint64_t k1_cand_overflow_count = 0;
  std::uint64_t k4_prime_overflow_count = 0;
  std::uint64_t k4_group_overflow_count = 0;
  std::uint64_t k5_port_overflow_count = 0;
  StageTimings stage_timings;

  struct OverflowDiagnostic {
    campaign::TileCoord coord{};
    std::uint32_t candidate_count = 0;
    std::uint32_t prime_count = 0;
    std::uint16_t group_count = 0;
    std::uint16_t port_counts[4] = {0, 0, 0, 0};
    bool k1_cand_overflow = false;
    bool k4_prime_overflow = false;
    bool k4_group_overflow = false;
    bool k5_port_overflow = false;
  };
  std::vector<OverflowDiagnostic> first_overflow_tiles;
};

class TileBatchDispatcher {
 public:
  TileBatchDispatcher(const campaign::CampaignConstants& constants,
                      const DispatchConfig& config = DispatchConfig{});
  ~TileBatchDispatcher();

  TileBatchDispatcher(const TileBatchDispatcher&) = delete;
  TileBatchDispatcher& operator=(const TileBatchDispatcher&) = delete;

  void dispatch(const campaign::TileCoord* tiles,
                std::size_t count,
                campaign::TileOp* output_tileops,
                DispatchStats* stats = nullptr);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
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
