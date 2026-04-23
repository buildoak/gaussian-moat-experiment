#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/grid.h"
#include "campaign/sieve.h"
#include "campaign/tileop.h"

namespace k5_parity {

struct PortRecord {
  std::int64_t h = 0;
  std::int64_t p_perp = 0;
  std::uint8_t global_wire_label = 0;
};

struct GpuTileOpResult {
  bool available = false;
  campaign::TileOp tileop{};
  std::string pending_reason;
};

campaign::CampaignConstants make_constants(std::uint64_t r_inner,
                                            std::uint64_t r_outer);

std::vector<campaign::TileCoord> first_active_tiles(std::uint64_t r_inner,
                                                    std::uint64_t r_outer,
                                                    std::size_t limit);

campaign::TileOp cpu_tileop(const campaign::TileCoord& coord,
                            const campaign::CampaignConstants& constants);

campaign::TileOp row_major_cpu_tileop(
    const campaign::TileCoord& coord,
    const campaign::CampaignConstants& constants);

GpuTileOpResult gpu_tileop_or_pending(const campaign::TileCoord& coord,
                                      const campaign::CampaignConstants& constants);

bool face_group_padding_zero(const campaign::TileOp& tileop);

bool same_face_payload(const campaign::TileOp& lhs, const campaign::TileOp& rhs);

bool same_tileop_bytes(const campaign::TileOp& lhs, const campaign::TileOp& rhs);

std::string tileop_mismatch_summary(const campaign::TileOp& lhs,
                                    const campaign::TileOp& rhs);

std::vector<PortRecord> sort_ports_for_test(std::vector<PortRecord> ports);

campaign::TileOp empty_cpu_fixture();

campaign::TileOp overflow_cpu_fixture();

}  // namespace k5_parity
