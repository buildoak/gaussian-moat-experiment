#include "support/k5_parity_support.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "../../../cpp-campaign-v2/src/tileop_internal.h"

namespace k5_parity {
namespace {

std::uint32_t packed_pos_for(const campaign::TileCoord& coord,
                             std::int64_t a,
                             std::int64_t b) {
  const std::int64_t col = a - (coord.a_lo - campaign::C);
  const std::int64_t row = b - (coord.b_lo - campaign::C);
  const std::int64_t side = campaign::SIDE_EXP;
  return static_cast<std::uint32_t>(row * side + col);
}

campaign::Prime synthetic_prime(const campaign::TileCoord& coord,
                                std::int64_t rel_a,
                                std::int64_t rel_b) {
  const std::int64_t a = coord.a_lo + rel_a;
  const std::int64_t b = coord.b_lo + rel_b;
  const auto norm_sq = static_cast<std::uint64_t>(
      static_cast<__int128>(a) * static_cast<__int128>(a) +
      static_cast<__int128>(b) * static_cast<__int128>(b));
  return campaign::Prime{a, b, norm_sq, packed_pos_for(coord, a, b)};
}

}  // namespace

campaign::CampaignConstants make_constants(std::uint64_t r_inner,
                                            std::uint64_t r_outer) {
  return campaign::CampaignConstants::from_radii(
      r_inner, r_outer, static_cast<std::uint32_t>(campaign::k_sq_value));
}

std::vector<campaign::TileCoord> first_active_tiles(std::uint64_t r_inner,
                                                    std::uint64_t r_outer,
                                                    std::size_t limit) {
  const campaign::Grid grid = campaign::Grid::build(
      r_inner, r_outer, static_cast<std::uint32_t>(campaign::k_sq_value));
  std::vector<campaign::TileCoord> coords = grid.enumerate_active_tiles();
  if (coords.size() > limit) {
    coords.resize(limit);
  }
  return coords;
}

campaign::TileOp cpu_tileop(const campaign::TileCoord& coord,
                            const campaign::CampaignConstants& constants) {
  const campaign::Grid grid{};
  return campaign::process_tile(coord, constants, grid);
}

GpuTileOpResult gpu_tileop_or_pending(const campaign::TileCoord&,
                                      const campaign::CampaignConstants&) {
  return GpuTileOpResult{
      false,
      campaign::TileOp{},
      "pending GPU: K5 kernel entrypoint is not wired in this scaffold",
  };
}

bool face_group_padding_zero(const campaign::TileOp& tileop) {
  int used = 0;
  for (std::uint8_t n : tileop.n) {
    used += static_cast<int>(n);
  }
  if (used > campaign::MAX_PORTS_PER_TILE) {
    return false;
  }
  for (int i = used; i < campaign::MAX_PORTS_PER_TILE; ++i) {
    if (tileop.face_groups[i] != 0) {
      return false;
    }
  }
  return true;
}

bool same_face_payload(const campaign::TileOp& lhs, const campaign::TileOp& rhs) {
  return std::memcmp(lhs.n, rhs.n, sizeof(lhs.n)) == 0 &&
         std::memcmp(lhs.face_groups, rhs.face_groups,
                     sizeof(lhs.face_groups)) == 0;
}

bool same_tileop_bytes(const campaign::TileOp& lhs, const campaign::TileOp& rhs) {
  return std::memcmp(&lhs, &rhs, sizeof(campaign::TileOp)) == 0;
}

std::string tileop_mismatch_summary(const campaign::TileOp& lhs,
                                    const campaign::TileOp& rhs) {
  const auto* lhs_bytes = reinterpret_cast<const std::uint8_t*>(&lhs);
  const auto* rhs_bytes = reinterpret_cast<const std::uint8_t*>(&rhs);
  for (std::size_t i = 0; i < sizeof(campaign::TileOp); ++i) {
    if (lhs_bytes[i] != rhs_bytes[i]) {
      std::ostringstream out;
      out << "first byte mismatch at offset " << i << ": lhs="
          << static_cast<int>(lhs_bytes[i]) << " rhs="
          << static_cast<int>(rhs_bytes[i]);
      return out.str();
    }
  }
  return "TileOp bytes match";
}

std::vector<PortRecord> sort_ports_for_test(std::vector<PortRecord> ports) {
  std::sort(ports.begin(), ports.end(), [](const PortRecord& lhs,
                                           const PortRecord& rhs) {
    if (lhs.h != rhs.h) return lhs.h < rhs.h;
    if (lhs.p_perp != rhs.p_perp) return lhs.p_perp < rhs.p_perp;
    return lhs.global_wire_label < rhs.global_wire_label;
  });
  return ports;
}

campaign::TileOp empty_cpu_fixture() {
  const campaign::TileCoord coord{0, 0, campaign::OFFSET_X, campaign::OFFSET_Y};
  const campaign::CampaignConstants constants = make_constants(100, 500);
  return campaign::internal::build_tileop_for_primes({}, {}, coord, constants);
}

campaign::TileOp overflow_cpu_fixture() {
  const campaign::TileCoord coord{0, 0, campaign::OFFSET_X, campaign::OFFSET_Y};
  const campaign::CampaignConstants constants = make_constants(100, 100000);
  std::vector<campaign::Prime> primes;
  std::vector<campaign::internal::PrimeGeoFlags> flags;
  primes.reserve(campaign::MAX_GROUPS_PER_TILE + 1);
  flags.reserve(campaign::MAX_GROUPS_PER_TILE + 1);

  for (int i = 0; i < campaign::MAX_GROUPS_PER_TILE + 1; ++i) {
    primes.push_back(synthetic_prime(coord, i * (campaign::C + 1), 0));
    flags.push_back(campaign::internal::PrimeGeoFlags{});
  }

  return campaign::internal::build_tileop_for_primes(std::move(primes),
                                                     std::move(flags), coord,
                                                     constants);
}

}  // namespace k5_parity
