#include "support/k3k4_parity_support.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "campaign/geo_tests.h"

namespace cuda_campaign::test_support {
namespace {

campaign::Prime make_prime(std::int64_t a, std::int64_t b, std::uint32_t packed_pos) {
  return campaign::Prime{
      a,
      b,
      static_cast<std::uint64_t>(a * a + b * b),
      packed_pos,
  };
}

void append_range(std::vector<std::uint64_t>* out,
                  std::uint64_t lo,
                  std::uint64_t hi) {
  for (std::uint64_t value = lo; value <= hi; ++value) {
    out->push_back(value);
  }
}

}  // namespace

ParentParityCase make_parent_parity_case() {
  std::vector<campaign::Prime> primes = {
      make_prime(1000, 1000, 0),
      make_prime(1003, 1000, 1),
      make_prime(1009, 1000, 2),
      make_prime(1024, 1000, 3),
      make_prime(1024, 1005, 4),
      make_prime(1040, 1000, 5),
      make_prime(1040, 1006, 6),
      make_prime(1064, 1000, 7),
  };

  std::sort(primes.begin(), primes.end(), [](const campaign::Prime& lhs,
                                             const campaign::Prime& rhs) {
    if (lhs.a != rhs.a) return lhs.a < rhs.a;
    return lhs.b < rhs.b;
  });

  ParentParityCase out;
  out.primes = std::move(primes);
  out.cpu_parent = cpu_parent_roots(out.primes);
  return out;
}

std::vector<std::int32_t> cpu_parent_roots(const std::vector<campaign::Prime>& primes) {
  campaign::DSU dsu = campaign::internal::build_local_dsu(primes);
  std::vector<std::int32_t> roots;
  roots.reserve(primes.size());
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(primes.size()); ++i) {
    roots.push_back(dsu.find(i));
  }
  return roots;
}

DenseRemapCase make_dense_remap_adversarial_case() {
  DenseRemapCase out;
  out.raw_roots.reserve(100);

  // Fifty components, two primes each. The first component encountered maps to
  // raw root 99 while later components count down to raw root 1. This catches
  // implementations that assign dense labels by sorted raw-root ID instead of
  // first appearance in prime-index order.
  for (std::int32_t component = 0; component < 50; ++component) {
    const std::int32_t raw_root = 99 - component * 2;
    out.raw_roots.push_back(raw_root);
    out.raw_roots.push_back(raw_root);
  }

  out.cpu_remap = campaign::internal::dense_remap_raw_roots_for_test(
      out.raw_roots, 100);
  return out;
}

GeoSweepCase make_geo_i128_sweep_case(std::uint64_t radius) {
  GeoSweepCase out;
  out.constants = campaign::CampaignConstants::from_radii(
      radius, radius + 1000, static_cast<std::uint32_t>(campaign::k_sq_value));

  const std::uint64_t two_k = static_cast<std::uint64_t>(2 * campaign::k_sq_value);
  std::vector<std::uint64_t> values;
  values.reserve(4 * two_k + 2);
  append_range(&values, out.constants.R_inner_sq - two_k,
               out.constants.R_inner_sq + two_k);
  append_range(&values, out.constants.R_outer_sq - two_k,
               out.constants.R_outer_sq + two_k);

  out.rows.reserve(values.size());
  for (const std::uint64_t norm_sq : values) {
    out.rows.push_back(GeoSweepRow{
        norm_sq,
        campaign::is_inner_prime(static_cast<std::int64_t>(norm_sq), out.constants),
        campaign::is_outer_prime(static_cast<std::int64_t>(norm_sq), out.constants),
    });
  }
  return out;
}

std::vector<std::string_view> expected_k3k4_gpu_symbols() {
  return {
      "cuda_campaign::test_support::gpu_api::debug_run_k3k4_parent_parity("
      "const std::vector<campaign::Prime>&)",
      "cuda_campaign::test_support::gpu_api::debug_run_k4_dense_remap("
      "const std::vector<std::int32_t>&, std::int32_t)",
      "cuda_campaign::test_support::gpu_api::debug_run_k4_geo_i128_sweep("
      "const campaign::CampaignConstants&, const std::vector<std::uint64_t>&)",
  };
}

void print_pending_gpu_api(std::string_view test_name) {
  std::cerr << test_name << ": pending GPU K3/K4 test API. Expected symbols:\n";
  for (const std::string_view symbol : expected_k3k4_gpu_symbols()) {
    std::cerr << "  - " << symbol << "\n";
  }
}

}  // namespace cuda_campaign::test_support
