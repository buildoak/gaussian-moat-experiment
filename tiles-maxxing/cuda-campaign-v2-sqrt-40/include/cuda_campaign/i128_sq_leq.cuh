#pragma once

#include <cstdint>

namespace cuda_campaign {

struct U128Limbs {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
};

static __host__ __device__ __forceinline__ std::uint64_t abs_i64_as_u64(
    std::int64_t value) {
  const std::uint64_t raw = static_cast<std::uint64_t>(value);
  return value < 0 ? (~raw + 1ULL) : raw;
}

static __host__ __device__ __forceinline__ U128Limbs square_signed_i64_to_u128(
    std::int64_t eps) {
  const std::uint64_t mag = abs_i64_as_u64(eps);
#if defined(__CUDA_ARCH__)
  return U128Limbs{
      static_cast<std::uint64_t>(__umul64hi(
          static_cast<unsigned long long>(mag),
          static_cast<unsigned long long>(mag))),
      mag * mag,
  };
#else
  const unsigned __int128 sq =
      static_cast<unsigned __int128>(mag) * static_cast<unsigned __int128>(mag);
  return U128Limbs{
      static_cast<std::uint64_t>(sq >> 64),
      static_cast<std::uint64_t>(sq),
  };
#endif
}

static __host__ __device__ __forceinline__ bool i128_sq_leq(
    std::int64_t eps,
    std::uint64_t bound_hi,
    std::uint64_t bound_lo) {
  const U128Limbs sq = square_signed_i64_to_u128(eps);
  return sq.hi < bound_hi || (sq.hi == bound_hi && sq.lo <= bound_lo);
}

}  // namespace cuda_campaign
