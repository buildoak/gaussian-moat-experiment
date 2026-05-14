// src/snapshot.cpp
//
// Binary snapshot writer for cpp-campaign-v2. The on-disk format is explicit
// little-endian and treats TileOp as opaque fixed-offset fields.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "campaign/snapshot.h"

#include "sha256.h"

namespace campaign {
namespace {

constexpr std::size_t kHashBytes = 32;

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

void write_u32_le(std::ostream& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.put(static_cast<char>((v >> (8 * i)) & 0xffu));
  }
}

void write_u64_le(std::ostream& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.put(static_cast<char>((v >> (8 * i)) & 0xffu));
  }
}

std::uint32_t read_u32_le(const std::array<std::uint8_t, kSnapshotHeaderSize>& b,
                          std::size_t off) {
  return static_cast<std::uint32_t>(b[off + 0]) |
         (static_cast<std::uint32_t>(b[off + 1]) << 8) |
         (static_cast<std::uint32_t>(b[off + 2]) << 16) |
         (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::uint64_t read_u64_le(const std::array<std::uint8_t, kSnapshotHeaderSize>& b,
                          std::size_t off) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(b[off + static_cast<std::size_t>(i)])
         << (8 * i);
  }
  return v;
}

std::string grid_canonical_string(const Grid& grid) {
  std::ostringstream ss;
  ss << "R_inner=" << grid.R_inner
     << ";R_outer=" << grid.R_outer
     << ";K=" << grid.K_SQ_value
     << ";S=" << grid.S_value
     << ";C=" << grid.C_value
     << ";offset=" << grid.o_x << "," << grid.o_y
     << ";tile_count=" << grid.total_tiles
     << ";tiles=";
  auto coords = grid.enumerate_active_tiles();
  std::sort(coords.begin(), coords.end(), [](const TileCoord& a, const TileCoord& b) {
    if (a.i != b.i) return a.i < b.i;
    return a.j < b.j;
  });
  for (std::size_t k = 0; k < coords.size(); ++k) {
    if (k != 0) ss << "|";
    ss << coords[k].i << "," << coords[k].j;
  }
  return ss.str();
}

std::string grid_tower_table_canonical_string(const Grid& grid) {
  std::ostringstream ss;
  ss << "grid_hash_schema=tower_table_v1"
     << ";R_inner=" << grid.R_inner
     << ";R_outer=" << grid.R_outer
     << ";R_inner_sq=" << grid.R_inner_sq
     << ";R_outer_sq=" << grid.R_outer_sq
     << ";K=" << grid.K_SQ_value
     << ";S=" << grid.S_value
     << ";C=" << grid.C_value
     << ";offset=" << grid.o_x << "," << grid.o_y
     << ";i_min=" << grid.i_min
     << ";i_max=" << grid.i_max
     << ";tile_count=" << grid.total_tiles
     << ";j_low=";
  for (std::size_t k = 0; k < grid.j_low.size(); ++k) {
    if (k != 0) ss << ",";
    ss << grid.j_low[k];
  }
  ss << ";j_high=";
  for (std::size_t k = 0; k < grid.j_high.size(); ++k) {
    if (k != 0) ss << ",";
    ss << grid.j_high[k];
  }
  ss << ";tower_offset=";
  for (std::size_t k = 0; k < grid.tower_offset.size(); ++k) {
    if (k != 0) ss << ",";
    ss << grid.tower_offset[k];
  }
  if (!grid.explicit_tiles.empty()) {
    ss << ";explicit_tiles=";
    for (std::size_t k = 0; k < grid.explicit_tiles.size(); ++k) {
      if (k != 0) ss << "|";
      const TileCoord& coord = grid.explicit_tiles[k];
      ss << coord.i << "," << coord.j << "," << coord.a_lo << ","
         << coord.b_lo;
    }
  }
  return ss.str();
}

std::array<std::uint8_t, kHashBytes> digest_from_hex(const std::string& hex,
                                                     const char* field_name) {
  if (hex.size() != kHashBytes * 2) {
    throw std::runtime_error(std::string(field_name) + ": expected 64 hex chars");
  }
  std::array<std::uint8_t, kHashBytes> out{};
  for (std::size_t i = 0; i < kHashBytes; ++i) {
    const auto parse_nibble = [field_name](char c) -> std::uint8_t {
      if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
      }
      if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(10 + c - 'a');
      }
      if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(10 + c - 'A');
      }
      throw std::runtime_error(std::string(field_name) + ": non-hex character");
    };
    out[i] = static_cast<std::uint8_t>((parse_nibble(hex[i * 2]) << 4) |
                                       parse_nibble(hex[i * 2 + 1]));
  }
  return out;
}

std::string utc_timestamp() {
  if (const char* frozen = std::getenv(kSnapshotTimestampEnv)) {
    return frozen;
  }
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::string generated_at(const SnapshotOptions& options) {
  if (options.generated_at_utc.has_value()) {
    return *options.generated_at_utc;
  }
  return utc_timestamp();
}

void write_hash(std::ostream& out, const std::array<std::uint8_t, kHashBytes>& h) {
  out.write(reinterpret_cast<const char*>(h.data()),
            static_cast<std::streamsize>(h.size()));
}

void write_tileop(std::ostream& out, const TileOp& op) {
  out.write(reinterpret_cast<const char*>(op.n), sizeof(op.n));
  out.write(reinterpret_cast<const char*>(op.face_groups), sizeof(op.face_groups));
  out.write(reinterpret_cast<const char*>(op.inner_flags), sizeof(op.inner_flags));
  out.write(reinterpret_cast<const char*>(op.outer_flags), sizeof(op.outer_flags));
  out.put(static_cast<char>(op.tile_flags));
  out.write(reinterpret_cast<const char*>(op.reserved), sizeof(op.reserved));
}

}  // namespace

std::string grid_params_hash(const Grid& grid) {
  return detail::sha256_hex(grid_canonical_string(grid));
}

std::string grid_tower_table_hash(const Grid& grid) {
  return detail::sha256_hex(grid_tower_table_canonical_string(grid));
}

SnapshotWriter::SnapshotWriter(const std::filesystem::path& path,
                               const Grid& grid,
                               const CampaignConstants& constants,
                               SnapshotOptions options)
    : path_(path),
      grid_(&grid),
      constants_(&constants),
      options_(std::move(options)),
      next_i_(grid.i_min) {
  if (grid.total_tiles < 0) {
    throw std::runtime_error("SnapshotWriter: grid.total_tiles is negative");
  }
  tile_count_ = static_cast<std::uint64_t>(grid.total_tiles);

  grid_hash_hex_ = grid_params_hash(grid);
  constants_hash_hex_ = constants.canonical_hash();
  mr_hash_hex_ = CampaignConstants::mr_witness_set_sha256();
  const auto grid_hash = digest_from_hex(grid_hash_hex_, "grid_params_hash");
  const auto constants_hash = digest_from_hex(constants_hash_hex_, "constants_hash");
  const auto mr_hash = digest_from_hex(mr_hash_hex_, "mr_witness_set_sha256");

  out_.open(path_, std::ios::binary | std::ios::trunc);
  if (!out_.is_open()) {
    throw std::runtime_error("SnapshotWriter: could not open output file " +
                             path_.string());
  }

  out_.write(kSnapshotMagic, sizeof(kSnapshotMagic));
  write_u32_le(out_, kSnapshotVersion);
  write_hash(out_, grid_hash);
  write_hash(out_, constants_hash);
  write_hash(out_, mr_hash);
  write_u64_le(out_, tile_count_);
  write_u32_le(out_, static_cast<std::uint32_t>(TILEOP_SIZE));
  write_u32_le(out_, 0);
}

void SnapshotWriter::append_column(std::int32_t i,
                                   std::span<const TileOp> tileops) {
  if (closed_) {
    throw std::runtime_error("SnapshotWriter::append_column: writer is closed");
  }
  if (i < next_i_) {
    throw std::runtime_error("SnapshotWriter::append_column: non-canonical column order");
  }
  while (next_i_ < i) {
    if (!grid_->enumerate_column_tiles(next_i_).empty()) {
      throw std::runtime_error(
          "SnapshotWriter::append_column: skipped non-empty column");
    }
    ++next_i_;
  }

  const auto column_tiles = grid_->enumerate_column_tiles(i);
  if (tileops.size() != column_tiles.size()) {
    throw std::runtime_error("SnapshotWriter::append_column: tile count does not match column");
  }

  for (const TileOp& op : tileops) {
    write_tileop(out_, op);
  }
  written_tiles_ += static_cast<std::uint64_t>(tileops.size());
  ++next_i_;
}

void SnapshotWriter::close() {
  if (closed_) return;
  if (written_tiles_ != tile_count_) {
    throw std::runtime_error("SnapshotWriter::close: wrote wrong tile count");
  }
  if (!out_.good()) {
    throw std::runtime_error("SnapshotWriter::close: failed while writing " +
                             path_.string());
  }
  out_.close();
  if (!out_) {
    throw std::runtime_error("SnapshotWriter::close: failed to close " +
                             path_.string());
  }

  nlohmann::json manifest;
  manifest["schema_version"] = 1;
  manifest["grid_params_hash"] = grid_hash_hex_;
  manifest["constants_hash"] = constants_hash_hex_;
  manifest["mr_witness_set_sha256"] = mr_hash_hex_;
  manifest["tile_count"] = tile_count_;
  manifest["bytes_per_tile"] = TILEOP_SIZE;
  manifest["k_sq"] = constants_->K_SQ_value;
  manifest["r_inner"] = constants_->R_inner;
  manifest["r_outer"] = constants_->R_outer;
  manifest["generated_at"] = generated_at(options_);

  const auto manifest_path = manifest_path_for(path_);
  std::ofstream manifest_out(manifest_path, std::ios::binary | std::ios::trunc);
  if (!manifest_out.is_open()) {
    throw std::runtime_error("SnapshotWriter::close: could not open manifest " +
                             manifest_path.string());
  }
  manifest_out << manifest.dump(2) << '\n';
  if (!manifest_out.good()) {
    throw std::runtime_error("SnapshotWriter::close: failed while writing manifest " +
                             manifest_path.string());
  }
  closed_ = true;
}

void write_snapshot(const std::filesystem::path& path,
                    const Grid& grid,
                    const std::vector<TileOp>& tileops,
                    const CampaignConstants& constants,
                    SnapshotOptions options) {
  if (grid.total_tiles < 0) {
    throw std::runtime_error("write_snapshot: grid.total_tiles is negative");
  }
  const auto tile_count = static_cast<std::uint64_t>(grid.total_tiles);
  if (tileops.size() != tile_count) {
    throw std::runtime_error("write_snapshot: tileops.size() != grid.total_tiles");
  }

  SnapshotWriter writer(path, grid, constants, std::move(options));
  for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
    auto column_tiles = grid.enumerate_column_tiles(i);
    std::sort(column_tiles.begin(), column_tiles.end(),
              [](const TileCoord& a, const TileCoord& b) {
                if (a.i != b.i) return a.i < b.i;
                return a.j < b.j;
              });
    std::vector<TileOp> column_ops;
    column_ops.reserve(column_tiles.size());
    for (const TileCoord& coord : column_tiles) {
      const std::int64_t idx = grid.flat_index(coord.i, coord.j);
      if (idx < 0 || static_cast<std::uint64_t>(idx) >= tile_count) {
        throw std::runtime_error("write_snapshot: grid.flat_index out of range");
      }
      column_ops.push_back(tileops[static_cast<std::size_t>(idx)]);
    }
    writer.append_column(i, std::span<const TileOp>(column_ops.data(),
                                                    column_ops.size()));
  }
  writer.close();
}

SnapshotHeader read_snapshot_header(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("read_snapshot_header: could not open " +
                             path.string());
  }

  std::array<std::uint8_t, kSnapshotHeaderSize> b{};
  in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
  if (in.gcount() != static_cast<std::streamsize>(b.size())) {
    throw std::runtime_error("read_snapshot_header: short header in " +
                             path.string());
  }

  SnapshotHeader h{};
  h.magic = read_u32_le(b, 0);
  h.version = read_u32_le(b, 4);
  std::memcpy(h.grid_params_hash, b.data() + 8, kHashBytes);
  std::memcpy(h.constants_hash, b.data() + 40, kHashBytes);
  std::memcpy(h.mr_witness_set_sha256, b.data() + 72, kHashBytes);
  h.tile_count = read_u64_le(b, 104);
  h.bytes_per_tile = read_u32_le(b, 112);
  h.reserved_padding = read_u32_le(b, 116);

  if (h.magic != kSnapshotMagicLe) {
    throw std::runtime_error("read_snapshot_header: bad magic in " + path.string());
  }
  if (h.version != kSnapshotVersion) {
    throw std::runtime_error("read_snapshot_header: unsupported version in " +
                             path.string());
  }
  if (h.reserved_padding != 0) {
    throw std::runtime_error("read_snapshot_header: non-zero header padding in " +
                             path.string());
  }
  if (h.bytes_per_tile != TILEOP_SIZE) {
    throw std::runtime_error("read_snapshot_header: bytes_per_tile is not 256 in " +
                             path.string());
  }
  return h;
}

}  // namespace campaign
