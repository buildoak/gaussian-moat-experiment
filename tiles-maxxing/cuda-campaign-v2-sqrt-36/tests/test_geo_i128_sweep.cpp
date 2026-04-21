#include <cstdint>
#include <iostream>
#include <vector>

#include "campaign/geo_tests.h"
#include "support/k3k4_parity_support.h"

int main() {
  const auto fixture = cuda_campaign::test_support::make_geo_i128_sweep_case(1000);

  for (const auto& row : fixture.rows) {
    const bool inner = campaign::is_inner_prime(
        static_cast<std::int64_t>(row.norm_sq), fixture.constants);
    const bool outer = campaign::is_outer_prime(
        static_cast<std::int64_t>(row.norm_sq), fixture.constants);
    if (inner != row.inner || outer != row.outer) {
      std::cerr << "CPU geo oracle drift at norm_sq=" << row.norm_sq << "\n";
      return 1;
    }
  }

#ifdef CUDA_CAMPAIGN_ENABLE_K3K4_GPU_TEST_API
  std::vector<std::uint64_t> norm_sq_values;
  norm_sq_values.reserve(fixture.rows.size());
  for (const auto& row : fixture.rows) {
    norm_sq_values.push_back(row.norm_sq);
  }

  const auto gpu = cuda_campaign::test_support::gpu_api::debug_run_k4_geo_i128_sweep(
      fixture.constants, norm_sq_values);
  if (gpu.size() != fixture.rows.size()) {
    std::cerr << "GPU geo sweep returned " << gpu.size()
              << " rows, expected " << fixture.rows.size() << "\n";
    return 1;
  }
  for (std::size_t i = 0; i < fixture.rows.size(); ++i) {
    if (gpu[i].norm_sq != fixture.rows[i].norm_sq ||
        gpu[i].inner != fixture.rows[i].inner ||
        gpu[i].outer != fixture.rows[i].outer) {
      std::cerr << "GPU geo flags differ at norm_sq=" << fixture.rows[i].norm_sq
                << "\n";
      return 1;
    }
  }
#else
  cuda_campaign::test_support::print_pending_gpu_api("test_geo_i128_sweep");
#endif

  return 0;
}
