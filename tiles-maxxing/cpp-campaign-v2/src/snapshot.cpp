// src/snapshot.cpp
//
// STUB — Phase 2 M5 implements.

#include "campaign/snapshot.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace campaign {

void write_snapshot(const std::string& /*out_path*/,
                    const Grid& /*grid*/,
                    const Region& /*region*/,
                    const CampaignConstants& /*constants*/,
                    const std::vector<TileOp>& /*tileops*/) {
  throw std::runtime_error(
      "snapshot::write_snapshot: Phase 1 stub — implemented in M5");
}

SnapshotHeader read_snapshot_header(const std::string& /*path*/) {
  throw std::runtime_error(
      "snapshot::read_snapshot_header: Phase 1 stub — implemented in M5");
}

}  // namespace campaign
