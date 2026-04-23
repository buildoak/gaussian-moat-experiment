#include <cstdint>
#include <exception>
#include <iostream>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/compositor.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"
#include "cuda_campaign/kernels.cuh"

namespace {

campaign::Grid single_tile_grid() {
  campaign::Grid grid;
  grid.i_min = 0;
  grid.i_max = 0;
  grid.j_low = {0};
  grid.j_high = {0};
  grid.tower_offset = {0, 1};
  grid.total_tiles = 1;
  return grid;
}

}  // namespace

int main() {
  try {
    const auto constants = campaign::CampaignConstants::from_radii(
        100, 500, static_cast<std::uint32_t>(campaign::k_sq_value));
    const campaign::Grid full_grid = campaign::Grid::build(
        100, 500, static_cast<std::uint32_t>(campaign::k_sq_value));
    const std::vector<campaign::TileCoord> coords =
        full_grid.enumerate_active_tiles();
    if (coords.empty()) {
      std::cerr << "expected at least one active tile\n";
      return 1;
    }

    const int forced_k1_capacity = 1;
    const cuda_campaign::K1K5DebugDownload result =
        cuda_campaign::run_k1_to_k5_debug({coords.front()}, constants,
                                          forced_k1_capacity);
    if (result.k1k4.overflow.empty() || result.k1k4.overflow[0] == 0) {
      std::cerr << "K1 overflow did not propagate to K4 overflow flag\n";
      return 1;
    }
    if (result.tileops.empty() ||
        (result.tileops[0].tile_flags & campaign::OVERFLOW_BIT) == 0) {
      std::cerr << "K1 overflow did not emit OVERFLOW_BIT TileOp\n";
      return 1;
    }

    campaign::Compositor compositor;
    const campaign::Grid grid = single_tile_grid();
    compositor.init(grid);
    compositor.ingest_column(0, result.tileops.data());
    const campaign::Verdict verdict = compositor.finalize();
    if (!compositor.has_spanning() || verdict != campaign::Verdict::kSpanning) {
      std::cerr << "K1 overflow TileOp did not conservatively span\n";
      return 1;
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_k1_overflow_spanning: " << e.what() << "\n";
    return 1;
  }
}
