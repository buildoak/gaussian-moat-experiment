#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "campaign/primality.h"
#include "campaign/sieve.h"

namespace campaign {

namespace {

__int128 square_i128(std::int64_t x) noexcept {
  return static_cast<__int128>(x) * static_cast<__int128>(x);
}

std::uint64_t floor_isqrt_u64(std::uint64_t n) noexcept {
  std::uint64_t lo = 0;
  std::uint64_t hi = std::uint64_t{1} << 32;
  while (lo + 1 < hi) {
    const std::uint64_t mid = lo + ((hi - lo) >> 1);
    if (static_cast<__int128>(mid) * static_cast<__int128>(mid) <=
        static_cast<__int128>(n)) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

std::uint64_t checked_norm_sq(std::int64_t a, std::int64_t b) {
  const __int128 norm = square_i128(a) + square_i128(b);
  if (norm < 0 ||
      norm > static_cast<__int128>(std::numeric_limits<std::uint64_t>::max())) {
    throw std::overflow_error("Gaussian norm does not fit in uint64_t");
  }
  return static_cast<std::uint64_t>(norm);
}

bool norm_in_annulus(std::uint64_t norm_sq,
                     const CampaignConstants& constants) noexcept {
  return norm_sq >= constants.R_inner_sq && norm_sq <= constants.R_outer_sq;
}

std::uint32_t packed_pos_for(const TileCoord& coord, std::int64_t a,
                             std::int64_t b,
                             const CampaignConstants& constants) {
  const std::int64_t col = a - (coord.a_lo - constants.C_value);
  const std::int64_t row = b - (coord.b_lo - constants.C_value);
  if (col < 0 || row < 0 || col >= constants.S_value + 1 + 2 * constants.C_value ||
      row >= constants.S_value + 1 + 2 * constants.C_value) {
    throw std::logic_error("sieve packed position outside halo");
  }
  const std::int64_t side = constants.S_value + 1 + 2 * constants.C_value;
  return static_cast<std::uint32_t>(row * side + col);
}

bool is_split_prime_norm(std::uint64_t norm_sq) {
  if (norm_sq == 2) return true;
  return (norm_sq & 3ULL) == 1ULL && campaign::is_prime(norm_sq);
}

}  // namespace

std::vector<Prime> sieve_tile(const TileCoord& coord,
                              const CampaignConstants& constants) {
  if (constants.K_SQ_value != static_cast<std::uint32_t>(k_sq_value)) {
    throw std::invalid_argument("CampaignConstants K_SQ_value mismatch");
  }

  std::vector<Prime> out;

  const std::int64_t halo = static_cast<std::int64_t>(constants.C_value);
  const std::int64_t a_begin = coord.a_lo - halo;
  const std::int64_t a_end = coord.a_lo + constants.S_value + halo;
  const std::int64_t b_begin = coord.b_lo - halo;
  const std::int64_t b_end = coord.b_lo + constants.S_value + halo;

  for (std::int64_t a = std::max<std::int64_t>(0, a_begin); a <= a_end; ++a) {
    const std::int64_t b_lo = std::max<std::int64_t>(std::max(a, b_begin), 0);
    for (std::int64_t b = b_lo; b <= b_end; ++b) {
      if (a == 0) {
        const std::uint64_t q = static_cast<std::uint64_t>(b);
        if ((q & 3ULL) != 3ULL || !campaign::is_prime(q)) continue;
      } else {
        const std::uint64_t norm_sq = checked_norm_sq(a, b);
        if (!is_split_prime_norm(norm_sq)) continue;
        if (!norm_in_annulus(norm_sq, constants)) continue;
        out.push_back(
            Prime{a, b, norm_sq, packed_pos_for(coord, a, b, constants)});
        continue;
      }

      const std::uint64_t norm_sq = checked_norm_sq(a, b);
      if (!norm_in_annulus(norm_sq, constants)) continue;
      out.push_back(
          Prime{a, b, norm_sq, packed_pos_for(coord, a, b, constants)});
    }
  }

  std::sort(out.begin(), out.end(), [](const Prime& lhs, const Prime& rhs) {
    if (lhs.a != rhs.a) return lhs.a < rhs.a;
    return lhs.b < rhs.b;
  });
  return out;
}

bool is_prime_u64(std::uint64_t n) {
  return campaign::is_prime(n);
}

bool is_gaussian_prime_norm(std::uint64_t n) {
  if (n == 2) return true;
  if (campaign::is_prime(n)) return (n & 3ULL) == 1ULL;

  const std::uint64_t q = floor_isqrt_u64(n);
  if (static_cast<__int128>(q) * static_cast<__int128>(q) !=
      static_cast<__int128>(n)) {
    return false;
  }
  return (q & 3ULL) == 3ULL && campaign::is_prime(q);
}

}  // namespace campaign
