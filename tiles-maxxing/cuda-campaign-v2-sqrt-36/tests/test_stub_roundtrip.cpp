#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"
#include "cuda_campaign/kernels.cuh"

int main() {
  try {
    const auto constants = campaign::CampaignConstants::from_radii(
        100, 500, static_cast<std::uint32_t>(campaign::k_sq_value));
    const auto grid = campaign::Grid::build(
        100, 500, static_cast<std::uint32_t>(campaign::k_sq_value));
    const std::vector<campaign::TileCoord> coords = grid.enumerate_active_tiles();
    if (coords.empty()) {
      std::cerr << "expected at least one active tile\n";
      return 1;
    }

    const campaign::TileOp cpu =
        campaign::process_tile(coords.front(), constants, grid);
    const campaign::TileOp cuda =
        cuda_campaign::run_stub_passthrough(cpu);
    if (std::memcmp(&cpu, &cuda, sizeof(cpu)) != 0) {
      std::cerr << "stub roundtrip changed TileOp bytes\n";
      return 1;
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_stub_roundtrip: " << e.what() << "\n";
    return 1;
  }
}
