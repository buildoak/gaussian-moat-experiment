#include "cuda_campaign/kernels.cuh"

#include <cuda_runtime.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "campaign/campaign_constants.h"
#include "cuda_campaign/campaign_constants.cuh"
#include "cuda_campaign/fj64_table.cuh"
#include "cuda_campaign/gpu_math.cuh"
#include "sha256.h"

namespace cuda_campaign {

__constant__ SplitPrimeBarrettGPU c_split_barrett[SPLIT_PRIMES_COUNT];
__constant__ InertPrimeBarrettGPU c_inert_barrett[INERT_PRIMES_COUNT];
__constant__ std::uint32_t c_trial_primes[NUM_TRIAL_PRIMES];
__constant__ std::int8_t c_bk_dr[NUM_BACKWARD_OFFSETS];
__constant__ std::int8_t c_bk_dc[NUM_BACKWARD_OFFSETS];
__constant__ DeviceCampaignConstants c_campaign_constants;

namespace {

inline void check_cuda(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    std::fprintf(stderr, "%s failed: %s\n", what, cudaGetErrorString(status));
    std::abort();
  }
}

std::uint64_t mulmod_small(std::uint64_t a, std::uint64_t b, std::uint64_t m) {
  return (a * b) % m;
}

std::uint64_t fast_sqrt_neg1(std::uint64_t p) {
  for (std::uint64_t x = 1; x < p; ++x) {
    if (mulmod_small(x, x, p) == p - 1ULL) {
      return x;
    }
  }
  return std::numeric_limits<std::uint64_t>::max();
}

std::uint32_t barrett_host_mod(std::uint32_t x,
                               std::uint32_t p,
                               std::uint32_t mu) {
  const std::uint32_t q =
      static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * mu) >> 32);
  std::uint32_t r = x - q * p;
  if (r >= p) {
    r -= p;
  }
  return r;
}

SieveTablesBarrett build_sieve_tables() {
  SieveTablesBarrett tables{};

  std::array<std::uint8_t, SIEVE_LIMIT + 1> is_prime{};
  is_prime.fill(1U);
  is_prime[0] = 0U;
  is_prime[1] = 0U;

  for (std::uint32_t p = 2U; p * p <= SIEVE_LIMIT; ++p) {
    if (is_prime[p] == 0U) continue;
    for (std::uint32_t multiple = p * p; multiple <= SIEVE_LIMIT; multiple += p) {
      is_prime[multiple] = 0U;
    }
  }

  for (std::uint32_t p = 2U; p <= SIEVE_LIMIT; ++p) {
    if (is_prime[p] == 0U) continue;

    const std::uint32_t mu =
        static_cast<std::uint32_t>((1ULL << 32) / static_cast<std::uint64_t>(p));

    if ((p & 3U) == 1U) {
      const std::uint64_t root_raw = fast_sqrt_neg1(p);
      if (root_raw == std::numeric_limits<std::uint64_t>::max()) {
        std::fprintf(stderr, "sqrt(-1) lookup failed for split prime %u\n", p);
        std::abort();
      }

      std::uint64_t root = root_raw;
      const std::uint64_t neg_root = static_cast<std::uint64_t>(p) - root;
      if (neg_root < root) {
        root = neg_root;
      }

      if (tables.split_count >= SPLIT_PRIMES_COUNT) {
        std::fprintf(stderr, "split-prime table overflow\n");
        std::abort();
      }

      const std::uint32_t root32 = static_cast<std::uint32_t>(root);
      if (barrett_host_mod(root32 * root32, p, mu) != p - 1U) {
        std::fprintf(stderr, "Barrett validation failed for split prime %u\n", p);
        std::abort();
      }

      tables.split_table[tables.split_count++] =
          SplitPrimeBarrettGPU{static_cast<std::uint16_t>(p),
                               static_cast<std::uint16_t>(root), mu};
    } else if ((p & 3U) == 3U) {
      if (tables.inert_count >= INERT_PRIMES_COUNT) {
        std::fprintf(stderr, "inert-prime table overflow\n");
        std::abort();
      }
      tables.inert_primes[tables.inert_count++] =
          InertPrimeBarrettGPU{static_cast<std::uint16_t>(p), 0, mu};
    }
  }

  if (tables.split_count != SPLIT_PRIMES_COUNT ||
      tables.inert_count != INERT_PRIMES_COUNT) {
    std::fprintf(stderr,
                 "sieve table count mismatch: split=%d/%d inert=%d/%d\n",
                 tables.split_count, SPLIT_PRIMES_COUNT,
                 tables.inert_count, INERT_PRIMES_COUNT);
    std::abort();
  }
  return tables;
}

void verify_fj64_sha256() {
  static_assert(sizeof(campaign::kFj64Table) ==
                    FJ64_TABLE_SIZE * sizeof(std::uint16_t),
                "FJ64 table must be 262144 uint16_t entries");
  const std::string hash = campaign::detail::sha256_hex(
      reinterpret_cast<const std::uint8_t*>(campaign::kFj64Table),
      sizeof(campaign::kFj64Table));
  if (hash != campaign::kFj64WitnessTableSha256) {
    std::fprintf(stderr, "FJ64 SHA-256 mismatch: got %s expected %s\n",
                 hash.c_str(), campaign::kFj64WitnessTableSha256);
    std::abort();
  }
}

}  // namespace

DeviceCampaignConstants make_device_constants(
    const campaign::CampaignConstants& constants) {
  return DeviceCampaignConstants{
      constants.R_inner,
      constants.R_outer,
      constants.R_inner_sq,
      constants.R_outer_sq,
      constants.prefilter_inner,
      constants.prefilter_outer,
      constants.four_rin_sq_k_hi,
      constants.four_rin_sq_k_lo,
      constants.four_rout_sq_k_hi,
      constants.four_rout_sq_k_lo,
      constants.K_SQ_value,
      constants.S_value,
      constants.C_value,
      constants.o_x,
      constants.o_y,
  };
}

void upload_cuda_constants(const campaign::CampaignConstants& constants) {
  const DeviceCampaignConstants device_constants = make_device_constants(constants);
  check_cuda(cudaMemcpyToSymbol(c_campaign_constants, &device_constants,
                                sizeof(device_constants)),
             "cudaMemcpyToSymbol(c_campaign_constants)");
  upload_sieve_tables();
  upload_backward_offsets();
}

void upload_sieve_tables() {
  const SieveTablesBarrett tables = build_sieve_tables();
  check_cuda(cudaMemcpyToSymbol(c_split_barrett, tables.split_table,
                                sizeof(tables.split_table)),
             "cudaMemcpyToSymbol(c_split_barrett)");
  check_cuda(cudaMemcpyToSymbol(c_inert_barrett, tables.inert_primes,
                                sizeof(tables.inert_primes)),
             "cudaMemcpyToSymbol(c_inert_barrett)");

  constexpr std::uint32_t kTrialPrimes[NUM_TRIAL_PRIMES] = {
      3U, 5U, 7U, 11U, 13U, 17U, 19U, 23U, 29U, 31U, 37U, 41U,
  };
  check_cuda(cudaMemcpyToSymbol(c_trial_primes, kTrialPrimes,
                                sizeof(kTrialPrimes)),
             "cudaMemcpyToSymbol(c_trial_primes)");
}

void upload_backward_offsets() {
  std::int8_t dr[NUM_BACKWARD_OFFSETS]{};
  std::int8_t dc[NUM_BACKWARD_OFFSETS]{};
  int count = 0;
  for (int r = -C; r <= 0; ++r) {
    for (int c = -C; c <= C; ++c) {
      if ((r > 0) || (r == 0 && c >= 0)) continue;
      if (r * r + c * c > k_sq_value) continue;
      dr[count] = static_cast<std::int8_t>(r);
      dc[count] = static_cast<std::int8_t>(c);
      ++count;
    }
  }

  if (count != NUM_BACKWARD_OFFSETS) {
    std::fprintf(stderr, "backward offset count mismatch: got %d expected %d\n",
                 count, NUM_BACKWARD_OFFSETS);
    std::abort();
  }
  check_cuda(cudaMemcpyToSymbol(c_bk_dr, dr, sizeof(dr)),
             "cudaMemcpyToSymbol(c_bk_dr)");
  check_cuda(cudaMemcpyToSymbol(c_bk_dc, dc, sizeof(dc)),
             "cudaMemcpyToSymbol(c_bk_dc)");
}

void upload_fj64_table(std::uint16_t** d_fj64_table) {
  verify_fj64_sha256();
  check_cuda(cudaMalloc(reinterpret_cast<void**>(d_fj64_table),
                        sizeof(campaign::kFj64Table)),
             "cudaMalloc(d_fj64_table)");
  check_cuda(cudaMemcpy(*d_fj64_table, campaign::kFj64Table,
                        sizeof(campaign::kFj64Table), cudaMemcpyHostToDevice),
             "cudaMemcpy(d_fj64_table)");
}

void free_fj64_table(std::uint16_t* d_fj64_table) {
  if (d_fj64_table != nullptr) {
    check_cuda(cudaFree(d_fj64_table), "cudaFree(d_fj64_table)");
  }
}

}  // namespace cuda_campaign
