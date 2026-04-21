#include <cstdint>
#include <iostream>

#include "campaign/constants.h"
#include "support/k3k4_parity_support.h"

int main() {
  const auto fixture =
      cuda_campaign::test_support::make_dense_remap_adversarial_case();
  const auto& remap = fixture.cpu_remap;

  if (remap.overflow) {
    std::cerr << "50-component dense-remap fixture unexpectedly overflowed\n";
    return 1;
  }
  if (remap.max_label != 50) {
    std::cerr << "expected 50 dense labels, got " << remap.max_label << "\n";
    return 1;
  }

  if (remap.wire_label_by_raw_root[99] != 1 ||
      remap.wire_label_by_raw_root[97] != 2 ||
      remap.wire_label_by_raw_root[1] != 50) {
    std::cerr << "dense-remap labels are not first-appearance ordered\n";
    return 1;
  }

  if (remap.wire_label_by_raw_root[1] < remap.wire_label_by_raw_root[99]) {
    std::cerr << "fixture failed to invert raw-root order\n";
    return 1;
  }

#ifdef CUDA_CAMPAIGN_ENABLE_K3K4_GPU_TEST_API
  const auto gpu =
      cuda_campaign::test_support::gpu_api::debug_run_k4_dense_remap(
          fixture.raw_roots, 100);
  if (gpu.overflow != remap.overflow ||
      gpu.max_label != remap.max_label ||
      gpu.wire_label_by_raw_root != remap.wire_label_by_raw_root) {
    std::cerr << "GPU dense-remap output does not match CPU oracle\n";
    return 1;
  }
#else
  cuda_campaign::test_support::print_pending_gpu_api(
      "test_dense_remap_adversarial");
#endif

  return 0;
}
