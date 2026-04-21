#include <exception>
#include <iostream>

#include "campaign/constants.h"
#include "support/k5_parity_support.h"

int main() {
  try {
    const auto constants = k5_parity::make_constants(10000, 11000);
    const auto coords = k5_parity::first_active_tiles(10000, 11000, 100);
    if (coords.size() != 100) {
      std::cerr << "expected 100 active tiles, got " << coords.size() << "\n";
      return 1;
    }

    bool saw_pending_gpu = false;
    for (const auto& coord : coords) {
      const campaign::TileOp cpu = k5_parity::cpu_tileop(coord, constants);
      if (!k5_parity::face_group_padding_zero(cpu)) {
        std::cerr << "CPU oracle emitted non-zero face_groups padding\n";
        return 1;
      }

      const k5_parity::GpuTileOpResult gpu =
          k5_parity::gpu_tileop_or_pending(coord, constants);
      if (!gpu.available) {
        saw_pending_gpu = true;
        continue;
      }
      if (!k5_parity::same_face_payload(cpu, gpu.tileop)) {
        std::cerr << k5_parity::tileop_mismatch_summary(cpu, gpu.tileop)
                  << "\n";
        return 1;
      }
      if (!k5_parity::face_group_padding_zero(gpu.tileop)) {
        std::cerr << "GPU emitted non-zero face_groups padding\n";
        return 1;
      }
    }

    if (saw_pending_gpu) {
      std::cout << "pending GPU: K5 face group parity compared CPU oracle only\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_face_groups_parity: " << e.what() << "\n";
    return 1;
  }
}
