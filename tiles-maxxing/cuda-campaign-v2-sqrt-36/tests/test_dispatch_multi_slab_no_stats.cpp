#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"
#include "cuda_campaign/host_driver.h"

int main() {
  try {
    const auto constants = campaign::CampaignConstants::from_radii(
        100, 600, static_cast<std::uint32_t>(campaign::k_sq_value));
    const auto grid = campaign::Grid::build(
        100, 600, static_cast<std::uint32_t>(campaign::k_sq_value));
    std::vector<campaign::TileCoord> coords = grid.enumerate_active_tiles();
    std::sort(coords.begin(), coords.end(),
              [](const campaign::TileCoord& a,
                 const campaign::TileCoord& b) {
                if (a.i != b.i) return a.i < b.i;
                return a.j < b.j;
              });
    if (coords.size() < 5) {
      std::cerr << "expected at least five active tiles\n";
      return 1;
    }
    coords.resize(5);

    cuda_campaign::DispatchConfig reference_config;
    reference_config.host_chunk_tiles = coords.size();
    reference_config.device_slab_tiles = coords.size();
    reference_config.ring_slots = 1;
    cuda_campaign::DispatchStats reference_stats;
    const std::vector<campaign::TileOp> reference =
        cuda_campaign::dispatch_tile_batch(coords, constants, reference_config,
                                           &reference_stats);
    if (!reference_stats.first_overflow_tiles.empty()) {
      std::cerr << "default stats path unexpectedly collected diagnostics\n";
      return 1;
    }
    cuda_campaign::DispatchConfig diagnostic_config = reference_config;
    diagnostic_config.overflow_diagnostics = true;
    cuda_campaign::DispatchStats diagnostic_stats;
    const std::vector<campaign::TileOp> diagnostic =
        cuda_campaign::dispatch_tile_batch(coords, constants, diagnostic_config,
                                           &diagnostic_stats);
    if (reference_stats.k1_cand_overflow_count !=
            diagnostic_stats.k1_cand_overflow_count ||
        reference_stats.k4_prime_overflow_count !=
            diagnostic_stats.k4_prime_overflow_count ||
        reference_stats.k4_group_overflow_count !=
            diagnostic_stats.k4_group_overflow_count ||
        reference_stats.k5_port_overflow_count !=
            diagnostic_stats.k5_port_overflow_count) {
      std::cerr << "summary stats counters diverged from diagnostic stats\n";
      return 1;
    }

    cuda_campaign::DispatchConfig slab_config;
    slab_config.host_chunk_tiles = 3;
    slab_config.device_slab_tiles = 1;
    slab_config.ring_slots = 2;
    cuda_campaign::DispatchStats slab_stats;
    const std::vector<campaign::TileOp> slabbed =
        cuda_campaign::dispatch_tile_batch(coords, constants, slab_config,
                                           &slab_stats);
    const std::vector<campaign::TileOp> slabbed_no_stats =
        cuda_campaign::dispatch_tile_batch(coords, constants, slab_config,
                                           nullptr);
    if (!slab_stats.first_overflow_tiles.empty()) {
      std::cerr
          << "multi-slab stats path unexpectedly collected diagnostics\n";
      return 1;
    }
    if (reference_stats.k1_cand_overflow_count !=
            slab_stats.k1_cand_overflow_count ||
        reference_stats.k4_prime_overflow_count !=
            slab_stats.k4_prime_overflow_count ||
        reference_stats.k4_group_overflow_count !=
            slab_stats.k4_group_overflow_count ||
        reference_stats.k5_port_overflow_count !=
            slab_stats.k5_port_overflow_count) {
      std::cerr << "multi-slab summary stats counters changed\n";
      return 1;
    }

    if (reference.size() != diagnostic.size() ||
        reference.size() != slabbed.size() ||
        reference.size() != slabbed_no_stats.size()) {
      std::cerr << "dispatch result size mismatch\n";
      return 1;
    }
    for (std::size_t i = 0; i < reference.size(); ++i) {
      if (std::memcmp(&reference[i], &diagnostic[i], sizeof(reference[i])) !=
          0) {
        std::cerr << "diagnostic dispatch changed TileOp at index "
                  << i << "\n";
        return 1;
      }
      if (std::memcmp(&reference[i], &slabbed[i], sizeof(reference[i])) != 0) {
        std::cerr << "multi-slab stats dispatch changed TileOp at index "
                  << i << "\n";
        return 1;
      }
      if (std::memcmp(&reference[i], &slabbed_no_stats[i],
                      sizeof(reference[i])) != 0) {
        std::cerr << "multi-slab no-stats dispatch changed TileOp at index "
                  << i << "\n";
        return 1;
      }
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_dispatch_multi_slab_no_stats: " << e.what() << "\n";
    return 1;
  }
}
