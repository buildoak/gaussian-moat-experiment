#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/grid.h"
#include "campaign/sieve.h"
#include "tileop_internal.h"

namespace cuda_campaign::test_support {

struct ParentParityCase {
  std::vector<campaign::Prime> primes;
  std::vector<std::int32_t> cpu_parent;
};

struct DenseRemapCase {
  std::vector<std::int32_t> raw_roots;
  campaign::internal::DenseRemap cpu_remap;
};

struct GeoSweepRow {
  std::uint64_t norm_sq = 0;
  bool inner = false;
  bool outer = false;
};

struct GeoSweepCase {
  campaign::CampaignConstants constants;
  std::vector<GeoSweepRow> rows;
};

ParentParityCase make_parent_parity_case();
std::vector<std::int32_t> cpu_parent_roots(const std::vector<campaign::Prime>& primes);

DenseRemapCase make_dense_remap_adversarial_case();

GeoSweepCase make_geo_i128_sweep_case(std::uint64_t radius);

std::vector<std::string_view> expected_k3k4_gpu_symbols();
void print_pending_gpu_api(std::string_view test_name);

#ifdef CUDA_CAMPAIGN_ENABLE_K3K4_GPU_TEST_API
namespace gpu_api {

// Expected M3 debug symbol. It should run K3+K4 Phase A/B for one already
// compacted tile-equivalent fixture and return the fully compressed d_parent[]
// array in CPU-visible order.
std::vector<std::int32_t> debug_run_k3k4_parent_parity(
    const std::vector<campaign::Prime>& primes);

// Expected M4 debug symbol. It should run K4's single-thread dense-remap path
// from a supplied compressed parent/raw-root vector and return CPU-visible
// dense-remap outputs.
campaign::internal::DenseRemap debug_run_k4_dense_remap(
    const std::vector<std::int32_t>& raw_roots,
    std::int32_t raw_root_bound);

struct GpuGeoSweepRow {
  std::uint64_t norm_sq = 0;
  bool inner = false;
  bool outer = false;
};

// Expected M4 debug symbol. It should run the GPU geo predicate over the
// supplied norm_sq values with the uploaded CampaignConstants and return flags
// in the same order.
std::vector<GpuGeoSweepRow> debug_run_k4_geo_i128_sweep(
    const campaign::CampaignConstants& constants,
    const std::vector<std::uint64_t>& norm_sq_values);

}  // namespace gpu_api
#endif

}  // namespace cuda_campaign::test_support
