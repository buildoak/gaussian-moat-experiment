#pragma once

#include <cstddef>
#include <cstdint>

#include "cuda_campaign/constants.cuh"

namespace cuda_campaign {

inline constexpr int ROW_PREFIX_ENTRIES = ACTIVE_ROWS + 1;
inline constexpr std::size_t ROW_PREFIX_BYTES_PER_TILE =
    sizeof(std::uint16_t) * ROW_PREFIX_ENTRIES;
inline constexpr std::size_t PRIME_POS_BYTES_PER_TILE =
    sizeof(std::uint32_t) * MAX_PRIMES_GPU;

struct CompactBuffers {
  std::uint16_t* d_row_prefix;   // [N * ROW_PREFIX_ENTRIES]
  std::uint32_t* d_prime_pos;    // [N * MAX_PRIMES_GPU]
  std::uint32_t* d_prime_count;  // [N]
};

static_assert(MAX_PRIMES_GPU >= 6144, "K3 compact capacity must be at least 6144");
static_assert(ROW_PREFIX_ENTRIES == ACTIVE_ROWS + 1,
              "K3 row-prefix stride must include one sentinel entry");
static_assert(ROW_PREFIX_ENTRIES <= BLOCK_THREADS,
              "K3 row-prefix scan expects one CUDA block to cover all rows plus sentinel");
static_assert(ROW_PREFIX_BYTES_PER_TILE ==
                  sizeof(std::uint16_t) * ROW_PREFIX_ENTRIES,
              "K3 row-prefix byte budget must track stride");

}  // namespace cuda_campaign
