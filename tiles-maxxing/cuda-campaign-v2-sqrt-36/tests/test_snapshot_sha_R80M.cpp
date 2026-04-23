#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class Sha256 {
 public:
  void update(const std::uint8_t* data, std::size_t len) {
    bit_len_ += static_cast<std::uint64_t>(len) * 8ULL;
    for (std::size_t i = 0; i < len; ++i) {
      block_[block_len_++] = data[i];
      if (block_len_ == block_.size()) {
        transform(block_.data());
        block_len_ = 0;
      }
    }
  }

  std::array<std::uint8_t, 32> final() {
    block_[block_len_++] = 0x80U;
    if (block_len_ > 56) {
      while (block_len_ < 64) block_[block_len_++] = 0;
      transform(block_.data());
      block_len_ = 0;
    }
    while (block_len_ < 56) block_[block_len_++] = 0;

    for (int i = 7; i >= 0; --i) {
      block_[block_len_++] =
          static_cast<std::uint8_t>((bit_len_ >> (i * 8)) & 0xffU);
    }
    transform(block_.data());

    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
      out[i * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffU);
      out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffU);
      out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffU);
      out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xffU);
    }
    return out;
  }

 private:
  static constexpr std::array<std::uint32_t, 64> k = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
      0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
      0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  static std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
    return (x >> n) | (x << (32U - n));
  }

  static std::uint32_t choose(std::uint32_t e, std::uint32_t f,
                              std::uint32_t g) {
    return (e & f) ^ (~e & g);
  }

  static std::uint32_t majority(std::uint32_t a, std::uint32_t b,
                                std::uint32_t c) {
    return (a & b) ^ (a & c) ^ (b & c);
  }

  static std::uint32_t big_sigma0(std::uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
  }

  static std::uint32_t big_sigma1(std::uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
  }

  static std::uint32_t small_sigma0(std::uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
  }

  static std::uint32_t small_sigma1(std::uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
  }

  void transform(const std::uint8_t* data) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(data[i * 4 + 0]) << 24) |
             (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(data[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
      w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) +
             w[i - 16];
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
      const std::uint32_t t1 =
          h + big_sigma1(e) + choose(e, f, g) + k[i] + w[i];
      const std::uint32_t t2 = big_sigma0(a) + majority(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> block_{};
  std::uint64_t bit_len_ = 0;
  std::size_t block_len_ = 0;
};

std::string hex(const std::array<std::uint8_t, 32>& digest) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::uint8_t byte : digest) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

std::string sha256_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("could not open " + path.string());
  }

  Sha256 sha;
  std::array<char, 1 << 16> buffer{};
  while (in.good()) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      sha.update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                 static_cast<std::size_t>(got));
    }
  }
  if (!in.eof()) {
    throw std::runtime_error("error reading " + path.string());
  }
  return hex(sha.final());
}

void write_file(const std::filesystem::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    throw std::runtime_error("could not write " + path.string());
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

int self_test(const char* argv0) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("snapshot-sha-self-test-" +
                    std::to_string(static_cast<unsigned long long>(
                        std::time(nullptr))));
  std::filesystem::create_directories(dir);
  const auto a = dir / "a.bin";
  const auto b = dir / "b.bin";
  const auto c = dir / "c.bin";
  write_file(a, "same snapshot bytes\n");
  write_file(b, "same snapshot bytes\n");
  write_file(c, "same snapshot bytes!\n");

  const std::string ha = sha256_file(a);
  const std::string hb = sha256_file(b);
  const std::string hc = sha256_file(c);
  std::filesystem::remove_all(dir);

  if (ha != hb) {
    std::cerr << argv0 << ": self-test failed: identical files mismatched\n";
    return 1;
  }
  if (ha == hc) {
    std::cerr << argv0
              << ": self-test failed: injected mismatch was not detected\n";
    return 1;
  }
  std::cout << "OK: SHA comparator self-test passed, injected mismatch "
               "detected\n";
  return 0;
}

void print_usage(const char* argv0) {
  std::cerr << "usage: " << argv0 << " <cpu.snapshot.bin> <cuda.snapshot.bin>\n"
            << "       " << argv0 << " --self-test\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
      return self_test(argv[0]);
    }
    if (argc != 3) {
      print_usage(argv[0]);
      return 2;
    }

    const std::filesystem::path cpu_path = argv[1];
    const std::filesystem::path cuda_path = argv[2];
    const std::string cpu_sha = sha256_file(cpu_path);
    const std::string cuda_sha = sha256_file(cuda_path);

    if (cpu_sha == cuda_sha) {
      std::cout << "OK: snapshot SHA-256 match\n"
                << "  cpu:  " << cpu_sha << "  " << cpu_path << "\n"
                << "  cuda: " << cuda_sha << "  " << cuda_path << "\n";
      return 0;
    }

    std::cerr << "ERROR: snapshot SHA-256 mismatch\n"
              << "  cpu:  " << cpu_sha << "  " << cpu_path << "\n"
              << "  cuda: " << cuda_sha << "  " << cuda_path << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 3;
  }
}
