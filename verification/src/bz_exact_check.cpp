#include "independent_moat.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using moat_verify::gaussian_prime_norm;
using moat_verify::u128;

namespace {

struct Args {
  std::uint64_t r_inner = 0;
  std::uint64_t r_outer = 0;
  std::uint32_t k_sq = 0;
  bool json = false;
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
    if (take("--r-inner", value)) {
      args.r_inner = value;
    } else if (take("--r-outer", value)) {
      args.r_outer = value;
    } else if (take("--k-sq", value)) {
      args.k_sq = static_cast<std::uint32_t>(value);
    } else if (a == "--json") {
      args.json = true;
    } else if (a == "--help" || a == "-h") {
      std::cout << "Usage: bz_exact_check --k-sq N --r-inner R --r-outer R [--json]\n";
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + a);
    }
  }
  if (args.k_sq == 0 || args.r_inner == 0 || args.r_outer <= args.r_inner) {
    throw std::runtime_error("--k-sq, --r-inner, and --r-outer are required");
  }
  return args;
}

bool is_square_u32(std::uint32_t n, std::uint64_t& root) {
  root = moat_verify::isqrt_u64(n);
  return root * root == n;
}

bool in_bz_i(std::uint64_t n, std::uint64_t r, std::uint64_t s) {
  const unsigned __int128 upper = u128(r + s) * u128(r + s);
  if (u128(n) > upper) return false;

  const unsigned __int128 base = u128(r) * u128(r) - 1 + u128(s) * u128(s);
  if (u128(n) <= base) return false;
  const unsigned __int128 x = u128(n) - base;
  return x * x > 4 * u128(s) * u128(s) * (u128(r) * u128(r) - 1);
}

bool in_bz_o(std::uint64_t n, std::uint64_t r, std::uint64_t s) {
  const unsigned __int128 lower = u128(r - s) * u128(r - s);
  if (u128(n) < lower) return false;

  const unsigned __int128 base = u128(r) * u128(r) + 1 + u128(s) * u128(s);
  if (u128(n) >= base) return false;
  const unsigned __int128 x = base - u128(n);
  return x * x > 4 * u128(s) * u128(s) * (u128(r) * u128(r) + 1);
}

std::vector<std::uint64_t> candidate_i(std::uint64_t r, std::uint64_t s) {
  const std::uint64_t upper = (r + s) * (r + s);
  std::vector<std::uint64_t> out;
  for (std::uint64_t n = upper - 8; n <= upper; ++n) {
    if (in_bz_i(n, r, s)) out.push_back(n);
    if (n == std::numeric_limits<std::uint64_t>::max()) break;
  }
  return out;
}

std::vector<std::uint64_t> candidate_o(std::uint64_t r, std::uint64_t s) {
  const std::uint64_t lower = (r - s) * (r - s);
  std::vector<std::uint64_t> out;
  for (std::uint64_t n = lower; n <= lower + 8; ++n) {
    if (in_bz_o(n, r, s)) out.push_back(n);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    std::uint64_t s = 0;
    if (!is_square_u32(args.k_sq, s)) {
      std::cerr << "ERROR: exact integer BZ checker currently requires square K; got "
                << args.k_sq << "\n";
      return 2;
    }

    const std::vector<std::uint64_t> cand_i = candidate_i(args.r_inner, s);
    const std::vector<std::uint64_t> cand_o = candidate_o(args.r_outer, s);
    std::vector<std::pair<std::string, std::uint64_t>> bad;
    for (const std::uint64_t n : cand_i) {
      if (gaussian_prime_norm(n)) bad.push_back({"BZ_I", n});
    }
    for (const std::uint64_t n : cand_o) {
      if (gaussian_prime_norm(n)) bad.push_back({"BZ_O", n});
    }

    if (args.json) {
      std::cout << "{"
                << "\"k_sq\":" << args.k_sq
                << ",\"r_inner\":" << args.r_inner
                << ",\"r_outer\":" << args.r_outer
                << ",\"sqrt_k\":" << s
                << ",\"bz_clean\":" << (bad.empty() ? "true" : "false")
                << ",\"bz_i_candidate_count\":" << cand_i.size()
                << ",\"bz_o_candidate_count\":" << cand_o.size()
                << ",\"bad_norm_count\":" << bad.size()
                << "}\n";
    } else {
      std::cout << "BZ exact check: R_inner=" << args.r_inner
                << " R_outer=" << args.r_outer
                << " K=" << args.k_sq
                << " sqrt_K=" << s << "\n";
      std::cout << "BZ_I: candidate_count=" << cand_i.size() << " candidates=[";
      for (std::size_t i = 0; i < cand_i.size(); ++i) {
        if (i != 0) std::cout << ",";
        std::cout << cand_i[i];
      }
      std::cout << "]\n";
      std::cout << "BZ_O: candidate_count=" << cand_o.size() << " candidates=[";
      for (std::size_t i = 0; i < cand_o.size(); ++i) {
        if (i != 0) std::cout << ",";
        std::cout << cand_o[i];
      }
      std::cout << "]\n";
      if (bad.empty()) {
        std::cout << "PASS: no Gaussian-prime norms found in BZ_I union BZ_O\n";
      } else {
        std::cout << "FAIL: Gaussian-prime norm(s) found in bad zone:\n";
        for (const auto& [zone, n] : bad) {
          std::cout << "  " << zone << ": " << n << "\n";
        }
      }
    }
    return bad.empty() ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 2;
  }
}
