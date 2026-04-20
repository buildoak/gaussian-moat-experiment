// apps/compare_snapshots.cpp
//
// Header-first, tile-by-tile byte diff for cpp-campaign-v2 snapshots.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "campaign/constants.h"
#include "campaign/snapshot.h"

namespace {

std::filesystem::path manifest_path_for(const std::filesystem::path& path) {
  const std::string filename = path.filename().string();
  std::string stem;
  static constexpr const char* kSnapshotBin = ".snapshot.bin";
  static constexpr const char* kBin = ".bin";
  if (filename.size() > std::strlen(kSnapshotBin) &&
      filename.compare(filename.size() - std::strlen(kSnapshotBin),
                       std::strlen(kSnapshotBin), kSnapshotBin) == 0) {
    stem = filename.substr(0, filename.size() - std::strlen(kSnapshotBin));
  } else if (filename.size() > std::strlen(kBin) &&
             filename.compare(filename.size() - std::strlen(kBin),
                              std::strlen(kBin), kBin) == 0) {
    stem = filename.substr(0, filename.size() - std::strlen(kBin));
  } else {
    stem = path.stem().string();
  }
  return path.parent_path() / (stem + ".manifest.json");
}

std::string hash_hex(const std::uint8_t* data) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (std::size_t i = 0; i < 32; ++i) {
    out[i * 2] = kHex[(data[i] >> 4) & 0xf];
    out[i * 2 + 1] = kHex[data[i] & 0xf];
  }
  return out;
}

bool hashes_equal(const std::uint8_t* a, const std::uint8_t* b) {
  for (std::size_t i = 0; i < 32; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

nlohmann::json read_manifest(const std::filesystem::path& snapshot_path) {
  const auto manifest_path = manifest_path_for(snapshot_path);
  std::ifstream in(manifest_path);
  if (!in.is_open()) {
    throw std::runtime_error("could not open manifest " + manifest_path.string());
  }
  nlohmann::json j;
  in >> j;
  return j;
}

bool report_hash_mismatch(const char* name,
                          const std::uint8_t* a,
                          const std::uint8_t* b) {
  if (hashes_equal(a, b)) return false;
  std::cerr << "HEADER MISMATCH: " << name << "\n"
            << "  A: " << hash_hex(a) << "\n"
            << "  B: " << hash_hex(b) << "\n";
  return true;
}

template <typename T>
bool report_scalar_mismatch(const char* name, T a, T b) {
  if (a == b) return false;
  std::cerr << "HEADER MISMATCH: " << name << "\n"
            << "  A: " << a << "\n"
            << "  B: " << b << "\n";
  return true;
}

std::ifstream open_payload(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("could not open snapshot " + path.string());
  }
  in.seekg(static_cast<std::streamoff>(campaign::kSnapshotHeaderSize),
           std::ios::beg);
  if (!in.good()) {
    throw std::runtime_error("could not seek payload in " + path.string());
  }
  return in;
}

void print_help(const char* argv0) {
  std::cout << "Usage: " << argv0 << " <a.snapshot.bin> <b.snapshot.bin>\n"
            << "Compare cpp-campaign-v2 snapshot headers and TileOp payload bytes.\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    print_help(argv[0]);
    return 0;
  }
  if (argc != 3) {
    print_help(argv[0]);
    return 2;
  }

  try {
    const std::filesystem::path a_path = argv[1];
    const std::filesystem::path b_path = argv[2];

    (void)read_manifest(a_path);
    (void)read_manifest(b_path);

    const auto a = campaign::read_snapshot_header(a_path);
    const auto b = campaign::read_snapshot_header(b_path);

    bool header_mismatch = false;
    header_mismatch |= report_hash_mismatch(
        "grid_params_hash", a.grid_params_hash, b.grid_params_hash);
    header_mismatch |= report_hash_mismatch(
        "constants_hash", a.constants_hash, b.constants_hash);
    header_mismatch |= report_hash_mismatch(
        "mr_witness_set_sha256", a.mr_witness_set_sha256,
        b.mr_witness_set_sha256);
    header_mismatch |= report_scalar_mismatch("tile_count",
                                              a.tile_count, b.tile_count);
    header_mismatch |= report_scalar_mismatch("bytes_per_tile",
                                              a.bytes_per_tile, b.bytes_per_tile);
    if (header_mismatch) {
      return 2;
    }

    auto a_in = open_payload(a_path);
    auto b_in = open_payload(b_path);
    std::array<std::uint8_t, campaign::TILEOP_SIZE> a_tile{};
    std::array<std::uint8_t, campaign::TILEOP_SIZE> b_tile{};
    std::uint64_t identical = 0;
    std::uint64_t different = 0;

    for (std::uint64_t tile = 0; tile < a.tile_count; ++tile) {
      a_in.read(reinterpret_cast<char*>(a_tile.data()),
                static_cast<std::streamsize>(a_tile.size()));
      b_in.read(reinterpret_cast<char*>(b_tile.data()),
                static_cast<std::streamsize>(b_tile.size()));
      if (a_in.gcount() != static_cast<std::streamsize>(a_tile.size()) ||
          b_in.gcount() != static_cast<std::streamsize>(b_tile.size())) {
        throw std::runtime_error("short payload while reading tile " +
                                 std::to_string(tile));
      }

      bool tile_same = true;
      for (std::size_t off = 0; off < a_tile.size(); ++off) {
        if (a_tile[off] != b_tile[off]) {
          std::cerr << "PAYLOAD MISMATCH: tile index " << tile
                    << ", byte offset " << off
                    << " (A=0x" << std::hex << std::setw(2)
                    << std::setfill('0') << static_cast<int>(a_tile[off])
                    << ", B=0x" << std::setw(2)
                    << static_cast<int>(b_tile[off]) << std::dec
                    << std::setfill(' ') << ")\n";
          tile_same = false;
          break;
        }
      }
      if (tile_same) {
        ++identical;
      } else {
        ++different;
      }
    }

    std::cout << a.tile_count << " tiles compared, "
              << identical << " tiles identical, "
              << different << " tiles differ\n";
    if (different == 0) {
      std::cout << "OK: snapshots byte-identical\n";
      return 0;
    }
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 3;
  }
}
