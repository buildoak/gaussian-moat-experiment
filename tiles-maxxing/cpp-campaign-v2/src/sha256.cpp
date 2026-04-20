// src/sha256.cpp
//
// Reference SHA-256 implementation. Based on FIPS PUB 180-4.
// Public-domain-equivalent reference impl (no external deps).

#include "sha256.h"

#include <cstdint>
#include <cstring>

namespace campaign::detail {

namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline std::uint32_t rotr(std::uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

void transform(std::uint32_t state[8], const std::uint8_t block[64]) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           (static_cast<std::uint32_t>(block[i * 4 + 3]));
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = s0 + mj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

std::array<std::uint8_t, 32> sha256_bytes(const std::uint8_t* data, std::size_t n) {
  std::uint32_t state[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };

  std::size_t processed = 0;
  while (n - processed >= 64) {
    transform(state, data + processed);
    processed += 64;
  }

  std::uint8_t buf[128] = {};
  const std::size_t tail = n - processed;
  std::memcpy(buf, data + processed, tail);
  buf[tail] = 0x80;

  const std::size_t pad_to = (tail < 56) ? 64 : 128;
  const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8ULL;
  for (int i = 0; i < 8; ++i) {
    buf[pad_to - 1 - i] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFFu);
  }
  transform(state, buf);
  if (pad_to == 128) transform(state, buf + 64);

  std::array<std::uint8_t, 32> out{};
  for (int i = 0; i < 8; ++i) {
    out[i * 4 + 0] = static_cast<std::uint8_t>((state[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = static_cast<std::uint8_t>((state[i]) & 0xFFu);
  }
  return out;
}

std::string sha256_hex(const std::string& input) {
  return sha256_hex(reinterpret_cast<const std::uint8_t*>(input.data()),
                    input.size());
}

std::string sha256_hex(const std::uint8_t* data, std::size_t n) {
  const auto digest = sha256_bytes(data, n);
  static const char kHex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 32; ++i) {
    out[i * 2 + 0] = kHex[(digest[i] >> 4) & 0xF];
    out[i * 2 + 1] = kHex[digest[i] & 0xF];
  }
  return out;
}

}  // namespace campaign::detail
