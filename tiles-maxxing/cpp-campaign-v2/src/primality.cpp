// src/primality.cpp
//
// Host C++ implementation of the FJ64 single-table Miller-Rabin test.

#include "campaign/primality.h"

#include <array>
#include <cstdint>

#include "campaign/fj64_table.h"

namespace campaign {

namespace {

std::uint64_t mul_mod(std::uint64_t a, std::uint64_t b, std::uint64_t m) noexcept {
  return static_cast<std::uint64_t>(
      (static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b)) %
      static_cast<unsigned __int128>(m));
}

std::uint64_t pow_mod(std::uint64_t base, std::uint64_t exp, std::uint64_t mod) noexcept {
  std::uint64_t result = 1;
  base %= mod;
  while (exp != 0) {
    if ((exp & 1ULL) != 0) {
      result = mul_mod(result, base, mod);
    }
    exp >>= 1;
    if (exp != 0) {
      base = mul_mod(base, base, mod);
    }
  }
  return result;
}

std::uint32_t trailing_zero_count(std::uint64_t n) noexcept {
  std::uint32_t count = 0;
  while ((n & 1ULL) == 0) {
    ++count;
    n >>= 1;
  }
  return count;
}

bool miller_rabin_witness(std::uint64_t n,
                          std::uint64_t d,
                          std::uint32_t s,
                          std::uint64_t witness) noexcept {
  const std::uint64_t a = witness % n;
  if (a == 0) {
    return true;
  }

  std::uint64_t x = pow_mod(a, d, n);
  if (x == 1 || x == n - 1) {
    return true;
  }

  for (std::uint32_t r = 1; r < s; ++r) {
    x = mul_mod(x, x, n);
    if (x == n - 1) {
      return true;
    }
  }
  return false;
}

std::uint16_t fj64_witness(std::uint64_t n) noexcept {
  std::uint64_t h = n;
  h = ((h >> 32) ^ h) * 0x45d9f3b3335b369ULL;
  h = ((h >> 32) ^ h) * 0x3335b36945d9f3bULL;
  h = ((h >> 32) ^ h);
  return kFj64Table[h & 0x3FFFFULL];
}

}  // namespace

bool is_prime(std::uint64_t n) {
  if (n < 2) {
    return false;
  }
  if (n == 2 || n == 3) {
    return true;
  }
  if ((n & 1ULL) == 0) {
    return false;
  }

  constexpr std::array<std::uint64_t, 12> kSmallPrimes = {
      3ULL, 5ULL, 7ULL, 11ULL, 13ULL, 17ULL,
      19ULL, 23ULL, 29ULL, 31ULL, 37ULL, 41ULL,
  };
  for (const std::uint64_t p : kSmallPrimes) {
    if (n == p) {
      return true;
    }
    if ((n % p) == 0) {
      return false;
    }
  }

  std::uint64_t d = n - 1;
  const std::uint32_t s = trailing_zero_count(d);
  d >>= s;

  if (!miller_rabin_witness(n, d, s, 2ULL)) {
    return false;
  }
  return miller_rabin_witness(n, d, s, fj64_witness(n));
}

}  // namespace campaign
