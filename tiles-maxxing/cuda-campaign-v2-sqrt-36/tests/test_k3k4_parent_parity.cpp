#include <cstdint>
#include <iostream>
#include <vector>

#include "campaign/constants.h"
#include "support/k3k4_parity_support.h"

int main() {
  const auto fixture = cuda_campaign::test_support::make_parent_parity_case();
  const std::vector<std::int32_t> recomputed =
      cuda_campaign::test_support::cpu_parent_roots(fixture.primes);

  if (fixture.cpu_parent != recomputed) {
    std::cerr << "CPU parent oracle is not stable across repeated DSU builds\n";
    return 1;
  }

  const std::vector<std::int32_t> expected =
      campaign::k_sq_value >= 36
          ? std::vector<std::int32_t>{0, 0, 0, 3, 3, 5, 5, 7}
          : std::vector<std::int32_t>{0, 0, 2, 3, 3, 5, 6, 7};
  if (fixture.cpu_parent != expected) {
    std::cerr << "unexpected CPU parent roots:";
    for (const std::int32_t root : fixture.cpu_parent) {
      std::cerr << ' ' << root;
    }
    std::cerr << "\n";
    return 1;
  }

#ifdef CUDA_CAMPAIGN_ENABLE_K3K4_GPU_TEST_API
  const std::vector<std::int32_t> gpu =
      cuda_campaign::test_support::gpu_api::debug_run_k3k4_parent_parity(
          fixture.primes);
  if (gpu != fixture.cpu_parent) {
    std::cerr << "GPU compressed parent array does not match CPU oracle\n";
    return 1;
  }
#else
  cuda_campaign::test_support::print_pending_gpu_api("test_k3k4_parent_parity");
#endif

  return 0;
}
