#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "campaign/campaign_constants.h"
#include "campaign/compositor.h"
#include "campaign/grid.h"
#include "campaign/tileop.h"
#include "cuda_campaign/constants.cuh"
#include "cuda_campaign/kernels.cuh"
#include "sha256.h"

namespace {

struct BatchSpec {
  const char* name = "";
  std::uint32_t k_sq = 0;
  std::uint64_t r_inner = 0;
  std::uint64_t r_outer = 0;
  std::size_t max_tiles = 0;
  const char* description = "";
};

const BatchSpec kBatches[] = {
    {"k36-small-r10m", 36, 10000000ULL, 10008192ULL, 256,
     "Small R quick validation centered tile batch"},
    {"k36-medium-r50m", 36, 50000000ULL, 50008192ULL, 512,
     "Medium R typical workload centered tile batch"},
    {"k36-large-r85m", 36, 85000000ULL, 85008192ULL, 1024,
     "Large R stress centered tile batch"},
    {"k36-edge-tsuchimura-r80015790", 36, 80000000ULL, 80015790ULL, 1024,
     "Anchored edge batch at the first validated MOAT radius"},
    {"k40-r100m", 40, 100000000ULL, 100008192ULL, 1024,
     "K=40 specific centered tile batch near R=100M"},
};

void append_bytes(std::vector<std::uint8_t>& out,
                  const void* data,
                  std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  out.insert(out.end(), bytes, bytes + size);
}

template <typename T>
void append_scalar(std::vector<std::uint8_t>& out, const T& value) {
  append_bytes(out, &value, sizeof(T));
}

template <typename T>
std::string hash_vector_raw(const std::vector<T>& values) {
  if (values.empty()) {
    return campaign::detail::sha256_hex(std::string{});
  }
  return campaign::detail::sha256_hex(
      reinterpret_cast<const std::uint8_t*>(values.data()),
      values.size() * sizeof(T));
}

std::string hash_connectivity(const cuda_campaign::K1K4DebugDownload& k1k4,
                              std::size_t tile_count) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(tile_count * 256);
  for (std::size_t tile = 0; tile < tile_count; ++tile) {
    const std::uint32_t prime_count = k1k4.prime_count[tile];
    const std::uint16_t max_label = k1k4.max_label[tile];
    append_scalar(bytes, k1k4.candidate_count[tile]);
    append_scalar(bytes, prime_count);
    append_scalar(bytes, max_label);
    append_scalar(bytes, k1k4.overflow[tile]);
    const std::size_t prime_base =
        tile * static_cast<std::size_t>(cuda_campaign::MAX_PRIMES_GPU);
    for (std::uint32_t p = 0; p < prime_count; ++p) {
      append_scalar(bytes, k1k4.prime_pos[prime_base + p]);
      append_scalar(bytes, k1k4.parent[prime_base + p]);
      append_scalar(bytes, k1k4.prime_geo_bits[prime_base + p]);
      append_scalar(bytes, k1k4.wire_label_by_raw_root[prime_base + p]);
    }
    const std::size_t flag_base = tile * 256U;
    for (std::uint16_t g = 0; g <= max_label && g < 256U; ++g) {
      append_scalar(bytes, k1k4.group_flags[flag_base + g]);
    }
  }
  return campaign::detail::sha256_hex(bytes.data(), bytes.size());
}

std::string hash_faces(const cuda_campaign::K1K5DebugDownload& gpu,
                       std::size_t tile_count) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(tile_count * 128);
  for (std::size_t tile = 0; tile < tile_count; ++tile) {
    for (std::size_t face = 0; face < cuda_campaign::NUM_FACES; ++face) {
      const std::size_t count_index =
          tile * cuda_campaign::NUM_FACES + face;
      const std::uint16_t face_count = gpu.face_counts[count_index];
      const std::uint16_t rep_count = gpu.face_rep_counts[count_index];
      append_scalar(bytes, face_count);
      append_scalar(bytes, rep_count);
      const std::size_t face_base =
          (tile * cuda_campaign::NUM_FACES + face) *
          static_cast<std::size_t>(cuda_campaign::MAX_PRIMES_GPU);
      for (std::uint16_t p = 0; p < face_count; ++p) {
        append_scalar(bytes, gpu.face_indices[face_base + p]);
        append_scalar(bytes, gpu.face_roots[face_base + p]);
      }
      for (std::uint16_t p = 0; p < rep_count; ++p) {
        append_scalar(bytes, gpu.face_reps[face_base + p]);
      }
    }
  }
  return campaign::detail::sha256_hex(bytes.data(), bytes.size());
}

std::string verdict_name(campaign::Verdict verdict) {
  switch (verdict) {
    case campaign::Verdict::kMoat:
      return "MOAT";
    case campaign::Verdict::kSpanning:
      return "SPANNING";
    case campaign::Verdict::kUnknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

campaign::Verdict compose_verdict(const campaign::Grid& grid,
                                  const std::vector<campaign::TileOp>& tileops) {
  campaign::Compositor compositor;
  compositor.init(grid);
  for (std::int32_t i = grid.i_min; i <= grid.i_max; ++i) {
    const auto column_tiles = grid.enumerate_column_tiles(i);
    if (column_tiles.empty()) continue;
    const std::int64_t offset = grid.flat_index(i, column_tiles.front().j);
    if (offset < 0) {
      throw std::runtime_error("active column has no flat-index base");
    }
    compositor.ingest_column(
        i, tileops.data() + static_cast<std::size_t>(offset));
  }
  return compositor.finalize();
}

campaign::Grid centered_tile_batch_grid(const campaign::Grid& full_grid,
                                        std::size_t max_tiles) {
  if (max_tiles == 0) {
    throw std::invalid_argument("max_tiles must be positive");
  }
  if (full_grid.total_tiles <= 0) {
    return full_grid;
  }

  campaign::Grid grid{};
  grid.R_inner = full_grid.R_inner;
  grid.R_outer = full_grid.R_outer;
  grid.R_inner_sq = full_grid.R_inner_sq;
  grid.R_outer_sq = full_grid.R_outer_sq;
  grid.K_SQ_value = full_grid.K_SQ_value;
  grid.S_value = full_grid.S_value;
  grid.C_value = full_grid.C_value;
  grid.o_x = full_grid.o_x;
  grid.o_y = full_grid.o_y;

  const std::int32_t full_cols = full_grid.i_max - full_grid.i_min + 1;
  std::int32_t i = full_grid.i_min + full_cols / 2;
  grid.i_min = i;
  std::size_t remaining = max_tiles;
  std::int64_t running = 0;

  while (i <= full_grid.i_max && remaining > 0) {
    const auto [lo, hi] = full_grid.column_bounds(i);
    if (lo <= hi) {
      const std::size_t height = static_cast<std::size_t>(hi - lo + 1);
      const std::size_t take = std::min(height, remaining);
      grid.j_low.push_back(lo);
      grid.j_high.push_back(static_cast<std::int32_t>(lo + take - 1));
      grid.tower_offset.push_back(running);
      running += static_cast<std::int64_t>(take);
      remaining -= take;
      grid.i_max = i;
    }
    ++i;
  }

  if (grid.j_low.empty()) {
    grid.i_min = 0;
    grid.i_max = -1;
    grid.total_tiles = 0;
    grid.tower_offset = {0};
    return grid;
  }

  grid.tower_offset.push_back(running);
  grid.total_tiles = running;
  return grid;
}

std::string bitmap_sample_json(const std::vector<std::uint32_t>& bitmap) {
  std::ostringstream out;
  out << "[";
  int emitted = 0;
  for (std::size_t idx = 0; idx < bitmap.size() && emitted < 16; ++idx) {
    if (bitmap[idx] == 0) continue;
    const std::size_t tile = idx / cuda_campaign::BITMAP_WORDS;
    const std::size_t word = idx % cuda_campaign::BITMAP_WORDS;
    if (emitted > 0) out << ",";
    out << "{\"tile\":" << tile << ",\"word\":" << word << ",\"value\":\"0x"
        << std::hex << std::setw(8) << std::setfill('0') << bitmap[idx]
        << std::dec << std::setfill(' ') << "\"}";
    ++emitted;
  }
  out << "]";
  return out.str();
}

std::string prime_count_sample_json(
    const std::vector<campaign::TileCoord>& tiles,
    const std::vector<std::uint32_t>& prime_count) {
  std::ostringstream out;
  out << "[";
  const std::size_t n = std::min<std::size_t>(tiles.size(), 16);
  for (std::size_t i = 0; i < n; ++i) {
    if (i > 0) out << ",";
    out << "{\"tile\":" << i << ",\"coord\":[" << tiles[i].i << ","
        << tiles[i].j << "],\"prime_count\":" << prime_count[i] << "}";
  }
  out << "]";
  return out.str();
}

std::uint64_t sum_u32(const std::vector<std::uint32_t>& values) {
  std::uint64_t total = 0;
  for (std::uint32_t value : values) total += value;
  return total;
}

std::uint64_t count_overflow_tiles(const std::vector<std::uint8_t>& overflow) {
  return static_cast<std::uint64_t>(
      std::count_if(overflow.begin(), overflow.end(),
                    [](std::uint8_t value) { return value != 0; }));
}

std::filesystem::path output_path(const std::filesystem::path& out_dir,
                                  const BatchSpec& spec) {
  return out_dir / (std::string(spec.name) + ".json");
}

void write_batch(const BatchSpec& spec, const std::filesystem::path& out_dir) {
  if (spec.k_sq != static_cast<std::uint32_t>(campaign::k_sq_value)) {
    throw std::runtime_error("batch K does not match this build");
  }

  campaign::CampaignConstants constants =
      campaign::CampaignConstants::from_radii(spec.r_inner, spec.r_outer,
                                              spec.k_sq);
  campaign::Grid full_grid = campaign::Grid::build(spec.r_inner, spec.r_outer,
                                                   spec.k_sq);
  const std::string invariant_error = full_grid.verify_invariants();
  if (!invariant_error.empty()) {
    throw std::runtime_error("grid invariants failed: " + invariant_error);
  }
  campaign::Grid grid = centered_tile_batch_grid(full_grid, spec.max_tiles);

  std::vector<campaign::TileCoord> tiles = grid.enumerate_active_tiles();
  cuda_campaign::K1K5DebugDownload gpu =
      cuda_campaign::run_k1_to_k5_debug(tiles, constants);
  if (gpu.tileops.size() != tiles.size()) {
    throw std::runtime_error("CUDA debug download returned wrong tile count");
  }
  const campaign::Verdict verdict = compose_verdict(grid, gpu.tileops);

  const std::string bitmap_hash = hash_vector_raw(gpu.k1k4.bitmap);
  const std::string face_hash = hash_faces(gpu, tiles.size());
  const std::string connectivity_hash = hash_connectivity(gpu.k1k4,
                                                          tiles.size());
  const std::string tileop_hash = hash_vector_raw(gpu.tileops);

  std::ostringstream json;
  json << "{\n"
       << "  \"schema\": \"cuda-golden-v1\",\n"
       << "  \"baseline_commit\": \"c40fbd1\",\n"
       << "  \"batch\": \"" << spec.name << "\",\n"
       << "  \"description\": \"" << spec.description << "\",\n"
       << "  \"k_sq\": " << spec.k_sq << ",\n"
       << "  \"r_inner\": " << spec.r_inner << ",\n"
       << "  \"r_outer\": " << spec.r_outer << ",\n"
       << "  \"region\": \"centered-contiguous-tile-batch\",\n"
       << "  \"max_tiles\": " << spec.max_tiles << ",\n"
       << "  \"tile_count\": " << tiles.size() << ",\n"
       << "  \"i_min\": " << grid.i_min << ",\n"
       << "  \"i_max\": " << grid.i_max << ",\n"
       << "  \"candidate_count_total\": " << sum_u32(gpu.k1k4.candidate_count)
       << ",\n"
       << "  \"prime_count_total\": " << sum_u32(gpu.k1k4.prime_count)
       << ",\n"
       << "  \"overflow_tile_count\": "
       << count_overflow_tiles(gpu.k1k4.overflow) << ",\n"
       << "  \"verdict\": \"" << verdict_name(verdict) << "\",\n"
       << "  \"constants_hash\": \"" << constants.canonical_hash() << "\",\n"
       << "  \"hashes\": {\n"
       << "    \"prime_bitmap_sha256\": \"" << bitmap_hash << "\",\n"
       << "    \"face_encoding_sha256\": \"" << face_hash << "\",\n"
       << "    \"connectivity_labels_sha256\": \"" << connectivity_hash
       << "\",\n"
       << "    \"tileop_sha256\": \"" << tileop_hash << "\"\n"
       << "  },\n"
       << "  \"samples\": {\n"
       << "    \"prime_bitmap_nonzero_words\": "
       << bitmap_sample_json(gpu.k1k4.bitmap) << ",\n"
       << "    \"prime_counts\": "
       << prime_count_sample_json(tiles, gpu.k1k4.prime_count) << "\n"
       << "  }\n"
       << "}\n";

  std::filesystem::create_directories(out_dir);
  const std::filesystem::path path = output_path(out_dir, spec);
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    throw std::runtime_error("could not open output " + path.string());
  }
  const std::string data = json.str();
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  std::cout << path << "\n";
}

const BatchSpec* find_batch(std::string_view name) {
  for (const BatchSpec& spec : kBatches) {
    if (name == spec.name) return &spec;
  }
  return nullptr;
}

void print_usage(const char* argv0) {
  std::cerr << "usage: " << argv0 << " --out DIR [--batch NAME|--all]\n"
            << "available batches for this build:\n";
  for (const BatchSpec& spec : kBatches) {
    if (spec.k_sq == static_cast<std::uint32_t>(campaign::k_sq_value)) {
      std::cerr << "  " << spec.name << "\n";
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path out_dir;
    std::string batch_name;
    bool all = false;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help" || arg == "-h") {
        print_usage(argv[0]);
        return 0;
      }
      if (arg == "--all") {
        all = true;
        continue;
      }
      if (arg == "--out" && i + 1 < argc) {
        out_dir = argv[++i];
        continue;
      }
      if (arg == "--batch" && i + 1 < argc) {
        batch_name = argv[++i];
        continue;
      }
      std::cerr << "unknown or incomplete argument: " << arg << "\n";
      print_usage(argv[0]);
      return 2;
    }

    if (out_dir.empty() || (all && !batch_name.empty()) ||
        (!all && batch_name.empty())) {
      print_usage(argv[0]);
      return 2;
    }

    if (all) {
      for (const BatchSpec& spec : kBatches) {
        if (spec.k_sq == static_cast<std::uint32_t>(campaign::k_sq_value)) {
          write_batch(spec, out_dir);
        }
      }
      return 0;
    }

    const BatchSpec* spec = find_batch(batch_name);
    if (spec == nullptr) {
      throw std::runtime_error("unknown batch: " + batch_name);
    }
    write_batch(*spec, out_dir);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
