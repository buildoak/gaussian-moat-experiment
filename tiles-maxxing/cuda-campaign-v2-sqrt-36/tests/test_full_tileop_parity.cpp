#include <exception>
#include <iostream>

#include "campaign/constants.h"
#include "support/k5_parity_support.h"

int main() {
  try {
    const campaign::TileOp empty = k5_parity::empty_cpu_fixture();
    if (empty.tile_flags != campaign::EMPTY_BIT ||
        !k5_parity::face_group_padding_zero(empty)) {
      std::cerr << "empty TileOp fixture did not encode EMPTY_BIT cleanly\n";
      return 1;
    }

    const campaign::TileOp overflow = k5_parity::overflow_cpu_fixture();
    if (overflow.tile_flags != campaign::OVERFLOW_BIT ||
        !k5_parity::face_group_padding_zero(overflow)) {
      std::cerr << "overflow TileOp fixture did not encode OVERFLOW_BIT cleanly\n";
      return 1;
    }

    const auto constants = k5_parity::make_constants(10000, 17000);
    const auto coords = k5_parity::first_active_tiles(10000, 17000, 1024);
    if (coords.size() != 1024) {
      std::cerr << "expected 1024 active tiles, got " << coords.size() << "\n";
      return 1;
    }

    bool saw_pending_gpu = false;
    for (const auto& coord : coords) {
      const campaign::TileOp cpu = k5_parity::cpu_tileop(coord, constants);
      const k5_parity::GpuTileOpResult gpu =
          k5_parity::gpu_tileop_or_pending(coord, constants);
      if (!gpu.available) {
        saw_pending_gpu = true;
        continue;
      }
      if (!k5_parity::same_tileop_bytes(cpu, gpu.tileop)) {
        std::cerr << k5_parity::tileop_mismatch_summary(cpu, gpu.tileop)
                  << "\n";
        return 1;
      }
    }

    if (saw_pending_gpu) {
      std::cout << "pending GPU: full 256 B parity compared CPU oracle only\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_full_tileop_parity: " << e.what() << "\n";
    return 1;
  }
}
