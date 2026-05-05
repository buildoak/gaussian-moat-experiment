#include "independent_moat.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>

using namespace moat_verify;

namespace {

struct Args {
  RowConfig row;
  std::string expect;
  std::uint64_t max_primes = 2000000;
};

bool parse_u64(const std::string& s, std::uint64_t& out) {
  char* end = nullptr;
  const unsigned long long value = std::strtoull(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  out = static_cast<std::uint64_t>(value);
  return true;
}

Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto take = [&](const char* flag, std::uint64_t& dst) -> bool {
      const std::string f(flag);
      std::string v;
      if (a == f) {
        if (i + 1 >= argc) throw std::runtime_error(f + " needs a value");
        v = argv[++i];
      } else if (a.rfind(f + "=", 0) == 0) {
        v = a.substr(f.size() + 1);
      } else {
        return false;
      }
      if (!parse_u64(v, dst)) throw std::runtime_error("invalid integer for " + f);
      return true;
    };
    std::uint64_t value = 0;
    if (take("--k-sq", value)) {
      args.row.k_sq = static_cast<std::uint32_t>(value);
    } else if (take("--r-inner", value)) {
      args.row.r_inner = value;
    } else if (take("--r-outer", value)) {
      args.row.r_outer = value;
    } else if (take("--max-primes", value)) {
      args.max_primes = value;
    } else if (a == "--expect") {
      if (i + 1 >= argc) throw std::runtime_error("--expect needs a value");
      args.expect = argv[++i];
    } else if (a.rfind("--expect=", 0) == 0) {
      args.expect = a.substr(9);
    } else if (a == "--help" || a == "-h") {
      std::cout << "Usage: exact_global_uf --k-sq N --r-inner R --r-outer R [--expect SPANNING|MOAT]\n";
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + a);
    }
  }
  if (args.row.k_sq == 0 || args.row.r_inner == 0 ||
      args.row.r_outer <= args.row.r_inner) {
    throw std::runtime_error("--k-sq, --r-inner, and --r-outer are required");
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    std::vector<Prime> primes = enumerate_annulus_primes(args.row);
    if (primes.size() > args.max_primes) {
      std::cerr << "ERROR: prime count " << primes.size()
                << " exceeds --max-primes=" << args.max_primes << "\n";
      return 2;
    }

    Dsu dsu(primes.size());
    std::unordered_map<std::uint64_t, std::size_t> index;
    index.reserve(primes.size() * 2 + 1);
    for (std::size_t i = 0; i < primes.size(); ++i) {
      index.emplace(point_key(Point{primes[i].a, primes[i].b}), i);
    }

    const std::vector<Point> offsets = offset_palette(args.row.k_sq);
    std::uint64_t edge_count = 0;
    for (std::size_t i = 0; i < primes.size(); ++i) {
      const Point p{primes[i].a, primes[i].b};
      for (const Point off : offsets) {
        const Point q{p.a + off.a, p.b + off.b};
        if (q.a < 0 || q.b < q.a) continue;
        const auto it = index.find(point_key(q));
        if (it == index.end()) continue;
        if (it->second <= i) continue;
        dsu.unite(i, it->second);
        ++edge_count;
      }
    }

    std::vector<std::uint8_t> reach(primes.size(), 0);
    for (std::size_t i = 0; i < primes.size(); ++i) {
      const std::size_t root = dsu.find(i);
      if (geo_inner(primes[i].norm_sq, args.row)) reach[root] |= 0x1;
      if (geo_outer(primes[i].norm_sq, args.row)) reach[root] |= 0x2;
    }

    bool spanning = false;
    std::size_t component_count = 0;
    for (std::size_t i = 0; i < primes.size(); ++i) {
      if (dsu.find(i) == i) {
        ++component_count;
        if ((reach[i] & 0x3) == 0x3) spanning = true;
      }
    }
    const std::string verdict = spanning ? "SPANNING" : "MOAT";
    std::cout << "{"
              << "\"schema_version\":1"
              << ",\"tool\":\"exact_global_uf\""
              << ",\"k_sq\":" << args.row.k_sq
              << ",\"r_inner\":" << args.row.r_inner
              << ",\"r_outer\":" << args.row.r_outer
              << ",\"prime_count\":" << primes.size()
              << ",\"edge_count\":" << edge_count
              << ",\"component_count\":" << component_count
              << ",\"verdict\":\"" << verdict << "\""
              << "}\n";
    if (!args.expect.empty() && args.expect != verdict) {
      std::cerr << "ERROR: verdict mismatch: got " << verdict
                << ", expected " << args.expect << "\n";
      return 1;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 2;
  }
}
