#pragma once

#include <cstdint>

namespace cuda_campaign {

struct UfInputBuffers {
  const std::uint32_t* d_bitmap = nullptr;       // [N * BITMAP_WORDS]
  const std::uint16_t* d_row_prefix = nullptr;   // [N * (ACTIVE_ROWS + 1)]
  const std::uint32_t* d_prime_pos = nullptr;    // [N * MAX_PRIMES_GPU]
  const std::uint32_t* d_prime_count = nullptr;  // [N]
};

struct UfOutputBuffers {
  std::uint16_t* d_parent = nullptr;  // [N * MAX_PRIMES_GPU]

  // M4 hook: geo staging, dense-remap, and group-flag accumulation will fill
  // these once Phase B.5/C/D are lifted.
  std::uint8_t* d_prime_geo_bits = nullptr;   // [N * MAX_PRIMES_GPU]
  std::uint8_t* d_wire_label_by_raw_root = nullptr;  // [N * MAX_PRIMES_GPU]
  std::uint8_t* d_max_label = nullptr;        // [N]
  std::uint8_t* d_remap_overflow = nullptr;   // [N]
  std::uint32_t* d_group_flags = nullptr;     // [N * 32]
};

struct UfBuffers {
  UfInputBuffers in;
  UfOutputBuffers out;
};

}  // namespace cuda_campaign
