// apps/compare_snapshots.cpp
//
// Byte-level diff tool for snapshot.bin + manifest.json pairs.
//
// Phase 1: parses two paths, opens both, prints size comparison, exits 0.
// Phase 2 implements the real header-first diff + payload comparison.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {
std::int64_t file_size(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) return -1;
  return static_cast<std::int64_t>(f.tellg());
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " <snapshot_a.bin> <snapshot_b.bin>\n";
    std::cerr << "Phase 1 stub: reports file sizes; Phase 2 does the real diff.\n";
    return 2;
  }
  const std::string a = argv[1];
  const std::string b = argv[2];
  const std::int64_t sa = file_size(a);
  const std::int64_t sb = file_size(b);
  std::cout << "A: " << a << " size=" << sa << "\n";
  std::cout << "B: " << b << " size=" << sb << "\n";
  if (sa < 0 || sb < 0) {
    std::cerr << "Error: could not open one or both files.\n";
    return 3;
  }
  std::cout << "(Phase 2) Real diff not yet implemented; returning 0.\n";
  return 0;
}
