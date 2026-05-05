#include "independent_moat.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace moat_verify;

namespace {

TileOpLite parse_tileop(const nlohmann::json& j) {
  TileOpLite op{};
  for (int i = 0; i < 4; ++i) op.n[static_cast<std::size_t>(i)] = j.at("n").at(i).get<std::uint8_t>();
  for (std::size_t i = 0; i < op.face_groups.size(); ++i) {
    op.face_groups[i] = j.at("face_groups").at(i).get<std::uint8_t>();
  }
  for (std::size_t i = 0; i < op.inner_flags.size(); ++i) {
    op.inner_flags[i] = j.at("inner_flags").at(i).get<std::uint8_t>();
    op.outer_flags[i] = j.at("outer_flags").at(i).get<std::uint8_t>();
  }
  op.tile_flags = j.at("tile_flags").get<std::uint8_t>();
  return op;
}

int port_total(const TileOpLite& op) {
  return static_cast<int>(op.n[0]) + static_cast<int>(op.n[1]) +
         static_cast<int>(op.n[2]) + static_cast<int>(op.n[3]);
}

bool flag(const std::array<std::uint8_t, 16>& flags, int label) {
  const int bit = label - 1;
  return ((flags[static_cast<std::size_t>(bit >> 3)] >> (bit & 7)) & 1U) != 0;
}

std::vector<std::string> semantic_signatures(const TileOpLite& op) {
  std::array<std::string, 129> sig{};
  const int total = port_total(op);
  for (int pos = 0; pos < total; ++pos) {
    const int label = op.face_groups[static_cast<std::size_t>(pos)];
    if (label > 0 && label <= 128) {
      sig[static_cast<std::size_t>(label)] += "p" + std::to_string(pos) + ";";
    }
  }
  for (int label = 1; label <= 128; ++label) {
    if (flag(op.inner_flags, label)) sig[static_cast<std::size_t>(label)] += "I;";
    if (flag(op.outer_flags, label)) sig[static_cast<std::size_t>(label)] += "O;";
  }

  std::vector<std::string> out;
  for (int label = 1; label <= 128; ++label) {
    const std::string& s = sig[static_cast<std::size_t>(label)];
    if (!s.empty()) out.push_back(s);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void require_equal(const TileOpLite& actual, const TileOpLite& expected) {
  if (actual.n != expected.n) {
    throw std::runtime_error("n mismatch actual=[" +
                             std::to_string(actual.n[0]) + "," +
                             std::to_string(actual.n[1]) + "," +
                             std::to_string(actual.n[2]) + "," +
                             std::to_string(actual.n[3]) + "] expected=[" +
                             std::to_string(expected.n[0]) + "," +
                             std::to_string(expected.n[1]) + "," +
                             std::to_string(expected.n[2]) + "," +
                             std::to_string(expected.n[3]) + "]");
  }
  if (semantic_signatures(actual) != semantic_signatures(expected)) {
    throw std::runtime_error("semantic label signature mismatch");
  }
  if (actual.tile_flags != expected.tile_flags) {
    throw std::runtime_error("tile_flags mismatch");
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string samples_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--samples" && i + 1 < argc) {
      samples_path = argv[++i];
    } else if (a.rfind("--samples=", 0) == 0) {
      samples_path = a.substr(10);
    } else if (a == "--manifest" && i + 1 < argc) {
      ++i;
    } else if (a.rfind("--manifest=", 0) == 0) {
      continue;
    } else {
      std::cerr << "Usage: tile_sample_check --samples samples.jsonl [--manifest manifest.json]\n";
      return 2;
    }
  }
  if (samples_path.empty()) {
    std::cerr << "ERROR: --samples is required\n";
    return 2;
  }

  try {
    std::ifstream in(samples_path);
    if (!in.is_open()) throw std::runtime_error("could not open samples");
    std::string line;
    std::uint64_t checked = 0;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      const nlohmann::json item = nlohmann::json::parse(line);
      RowConfig row{
          item.at("k_sq").get<std::uint32_t>(),
          item.at("r_inner").get<std::uint64_t>(),
          item.at("r_outer").get<std::uint64_t>()};
      const auto& t = item.at("tile");
      TileCoord tile{
          t.at("i").get<std::int32_t>(),
          t.at("j").get<std::int32_t>(),
          t.at("a_lo").get<std::int64_t>(),
          t.at("b_lo").get<std::int64_t>()};
      const TileOpLite actual = build_tileop(tile, row);
      const TileOpLite expected = parse_tileop(item.at("tileop"));
      try {
        require_equal(actual, expected);
      } catch (const std::exception& e) {
        throw std::runtime_error("tile (" + std::to_string(tile.i) + "," +
                                 std::to_string(tile.j) + "): " + e.what());
      }
      ++checked;
    }
    std::cout << "tile sample check PASS: checked=" << checked << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
