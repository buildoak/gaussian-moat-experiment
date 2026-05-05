#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace moat_verify {

inline constexpr std::int64_t kTileSide = 256;
inline constexpr int kNumFaces = 4;
inline constexpr int kMaxPortsPerTile = 192;
inline constexpr int kMaxGroupsPerTile = 128;

enum class Face : int { I = 0, O = 1, L = 2, R = 3 };

struct Point {
  std::int64_t a = 0;
  std::int64_t b = 0;
};

struct Prime {
  std::int64_t a = 0;
  std::int64_t b = 0;
  std::uint64_t norm_sq = 0;
};

struct TileCoord {
  std::int32_t i = 0;
  std::int32_t j = 0;
  std::int64_t a_lo = 0;
  std::int64_t b_lo = 0;
};

struct TileOpLite {
  std::array<std::uint8_t, 4> n{};
  std::array<std::uint8_t, 192> face_groups{};
  std::array<std::uint8_t, 16> inner_flags{};
  std::array<std::uint8_t, 16> outer_flags{};
  std::uint8_t tile_flags = 0;
};

struct RowConfig {
  std::uint32_t k_sq = 36;
  std::uint64_t r_inner = 0;
  std::uint64_t r_outer = 0;
};

class Dsu {
 public:
  explicit Dsu(std::size_t n) : parent_(n) {
    for (std::size_t i = 0; i < n; ++i) parent_[i] = i;
  }

  std::size_t find(std::size_t x) {
    std::size_t root = x;
    while (parent_[root] != root) root = parent_[root];
    while (parent_[x] != x) {
      const std::size_t next = parent_[x];
      parent_[x] = root;
      x = next;
    }
    return root;
  }

  std::size_t unite(std::size_t a, std::size_t b) {
    std::size_t ra = find(a);
    std::size_t rb = find(b);
    if (ra == rb) return ra;
    if (rb < ra) std::swap(ra, rb);
    parent_[rb] = ra;
    return ra;
  }

  std::vector<std::size_t> roots() {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < parent_.size(); ++i) {
      const std::size_t root = find(i);
      if (std::find(out.begin(), out.end(), root) == out.end()) {
        out.push_back(root);
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }

 private:
  std::vector<std::size_t> parent_;
};

inline unsigned __int128 u128(std::uint64_t v) {
  return static_cast<unsigned __int128>(v);
}

inline __int128 i128(std::int64_t v) {
  return static_cast<__int128>(v);
}

inline std::uint64_t isqrt_u64(std::uint64_t n) {
  std::uint64_t lo = 0;
  std::uint64_t hi = std::uint64_t{1} << 32;
  while (lo + 1 < hi) {
    const std::uint64_t mid = lo + ((hi - lo) >> 1);
    if (u128(mid) * u128(mid) <= u128(n)) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

inline std::uint64_t ceil_isqrt_u64(std::uint64_t n) {
  const std::uint64_t r = isqrt_u64(n);
  return (u128(r) * u128(r) == u128(n)) ? r : r + 1;
}

inline std::uint64_t norm_sq(Point p) {
  const unsigned __int128 n =
      u128(static_cast<std::uint64_t>(p.a)) * u128(static_cast<std::uint64_t>(p.a)) +
      u128(static_cast<std::uint64_t>(p.b)) * u128(static_cast<std::uint64_t>(p.b));
  if (n > u128(std::numeric_limits<std::uint64_t>::max())) {
    throw std::overflow_error("norm_sq exceeds uint64");
  }
  return static_cast<std::uint64_t>(n);
}

inline std::uint64_t mul_mod_u64(std::uint64_t a,
                                 std::uint64_t b,
                                 std::uint64_t mod) {
  return static_cast<std::uint64_t>((u128(a) * u128(b)) % u128(mod));
}

inline std::uint64_t pow_mod_u64(std::uint64_t a,
                                 std::uint64_t e,
                                 std::uint64_t mod) {
  std::uint64_t result = 1;
  a %= mod;
  while (e != 0) {
    if ((e & 1ULL) != 0) result = mul_mod_u64(result, a, mod);
    e >>= 1;
    if (e != 0) a = mul_mod_u64(a, a, mod);
  }
  return result;
}

inline bool is_prime_u64(std::uint64_t n) {
  if (n < 2) return false;
  constexpr std::array<std::uint64_t, 12> small = {
      2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
  for (const std::uint64_t p : small) {
    if (n == p) return true;
    if (n % p == 0) return false;
  }

  std::uint64_t d = n - 1;
  std::uint32_t s = 0;
  while ((d & 1ULL) == 0) {
    d >>= 1;
    ++s;
  }

  constexpr std::array<std::uint64_t, 7> bases = {
      2ULL, 325ULL, 9375ULL, 28178ULL, 450775ULL, 9780504ULL,
      1795265022ULL};
  for (const std::uint64_t raw_a : bases) {
    const std::uint64_t a = raw_a % n;
    if (a == 0) continue;
    std::uint64_t x = pow_mod_u64(a, d, n);
    if (x == 1 || x == n - 1) continue;
    bool probably_prime = false;
    for (std::uint32_t r = 1; r < s; ++r) {
      x = mul_mod_u64(x, x, n);
      if (x == n - 1) {
        probably_prime = true;
        break;
      }
    }
    if (!probably_prime) return false;
  }
  return true;
}

inline bool gaussian_prime_point(Point p) {
  if (p.a < 0 || p.b < p.a) return false;
  if (p.a == 0) {
    const std::uint64_t q = static_cast<std::uint64_t>(p.b);
    return (q & 3ULL) == 3ULL && is_prime_u64(q);
  }
  const std::uint64_t n = norm_sq(p);
  return (n & 3ULL) == 1ULL && is_prime_u64(n);
}

inline bool gaussian_prime_norm(std::uint64_t n) {
  if (n == 2) return true;
  if (is_prime_u64(n)) return (n & 3ULL) == 1ULL;
  const std::uint64_t q = isqrt_u64(n);
  return u128(q) * u128(q) == u128(n) &&
         (q & 3ULL) == 3ULL &&
         is_prime_u64(q);
}

inline bool in_annulus(Point p, const RowConfig& row) {
  const std::uint64_t n = norm_sq(p);
  return n >= row.r_inner * row.r_inner && n <= row.r_outer * row.r_outer;
}

inline bool within_k(Point lhs, Point rhs, std::uint32_t k_sq) {
  const __int128 da = i128(lhs.a) - i128(rhs.a);
  const __int128 db = i128(lhs.b) - i128(rhs.b);
  return da * da + db * db <= static_cast<__int128>(k_sq);
}

inline bool geo_inner(std::uint64_t n, const RowConfig& row) {
  const __int128 norm = static_cast<__int128>(n);
  const __int128 r_sq = static_cast<__int128>(row.r_inner) *
                        static_cast<__int128>(row.r_inner);
  if (norm < r_sq) return false;
  const __int128 eps = norm - r_sq - static_cast<__int128>(row.k_sq);
  return eps * eps <=
         4 * r_sq * static_cast<__int128>(row.k_sq);
}

inline bool geo_outer(std::uint64_t n, const RowConfig& row) {
  const __int128 norm = static_cast<__int128>(n);
  const __int128 r_sq = static_cast<__int128>(row.r_outer) *
                        static_cast<__int128>(row.r_outer);
  if (norm > r_sq) return false;
  const __int128 eps = norm - r_sq - static_cast<__int128>(row.k_sq);
  return eps * eps <=
         4 * r_sq * static_cast<__int128>(row.k_sq);
}

inline int collar(std::uint32_t k_sq) {
  return static_cast<int>(isqrt_u64(k_sq));
}

inline std::int64_t rel_col(const Prime& p, const TileCoord& tile) {
  return p.a - tile.a_lo;
}

inline std::int64_t rel_row(const Prime& p, const TileCoord& tile) {
  return p.b - tile.b_lo;
}

inline std::int64_t face_h(const Prime& p, const TileCoord& tile, Face face) {
  switch (face) {
    case Face::I:
    case Face::O:
      return rel_col(p, tile);
    case Face::L:
    case Face::R:
      return rel_row(p, tile);
  }
  return 0;
}

inline std::int64_t face_perp(const Prime& p, const TileCoord& tile, Face face) {
  switch (face) {
    case Face::I:
      return rel_row(p, tile);
    case Face::O:
      return rel_row(p, tile) - kTileSide;
    case Face::L:
      return rel_col(p, tile);
    case Face::R:
      return rel_col(p, tile) - kTileSide;
  }
  return 0;
}

inline bool on_face_strip(const Prime& p,
                          const TileCoord& tile,
                          Face face,
                          std::uint32_t k_sq) {
  const std::int64_t d = face_perp(p, tile, face);
  const std::int64_t c = collar(k_sq);
  return -c <= d && d <= c;
}

inline bool on_any_face_strip(const Prime& p,
                              const TileCoord& tile,
                              std::uint32_t k_sq) {
  return on_face_strip(p, tile, Face::I, k_sq) ||
         on_face_strip(p, tile, Face::O, k_sq) ||
         on_face_strip(p, tile, Face::L, k_sq) ||
         on_face_strip(p, tile, Face::R, k_sq);
}

inline void bit_set(std::array<std::uint8_t, 16>& flags, int label) {
  const int g0 = label - 1;
  flags[static_cast<std::size_t>(g0 >> 3)] |=
      static_cast<std::uint8_t>(1U << (g0 & 7));
}

inline bool bit_test(const std::array<std::uint8_t, 16>& flags, int label) {
  const int g0 = label - 1;
  return ((flags[static_cast<std::size_t>(g0 >> 3)] >> (g0 & 7)) & 1U) != 0;
}

inline std::vector<Prime> enumerate_primes_in_box(const TileCoord& tile,
                                                  const RowConfig& row) {
  const std::int64_t c = collar(row.k_sq);
  const std::int64_t a_begin = std::max<std::int64_t>(0, tile.a_lo - c);
  const std::int64_t a_end = tile.a_lo + kTileSide + c;
  const std::int64_t b_begin = tile.b_lo - c;
  const std::int64_t b_end = tile.b_lo + kTileSide + c;
  std::vector<Prime> out;
  for (std::int64_t a = a_begin; a <= a_end; ++a) {
    const std::int64_t lo = std::max<std::int64_t>({a, 0, b_begin});
    for (std::int64_t b = lo; b <= b_end; ++b) {
      const Point p{a, b};
      const std::uint64_t n = norm_sq(p);
      const std::uint64_t r_in_sq = row.r_inner * row.r_inner;
      const std::uint64_t r_out_sq = row.r_outer * row.r_outer;
      if (n < r_in_sq || n > r_out_sq) continue;
      if (!gaussian_prime_point(p)) continue;
      out.push_back(Prime{a, b, n});
    }
  }
  std::sort(out.begin(), out.end(), [](const Prime& lhs, const Prime& rhs) {
    if (lhs.a != rhs.a) return lhs.a < rhs.a;
    if (lhs.b != rhs.b) return lhs.b < rhs.b;
    return lhs.norm_sq < rhs.norm_sq;
  });
  return out;
}

inline std::vector<Prime> enumerate_annulus_primes(const RowConfig& row) {
  const std::uint64_t r_in_sq = row.r_inner * row.r_inner;
  const std::uint64_t r_out_sq = row.r_outer * row.r_outer;
  std::vector<Prime> out;
  for (std::uint64_t a_u = 0; a_u <= row.r_outer; ++a_u) {
    const std::uint64_t a_sq = a_u * a_u;
    if (a_sq > r_out_sq) break;
    std::uint64_t b_lo = a_u;
    if (a_sq < r_in_sq) {
      b_lo = std::max<std::uint64_t>(b_lo, ceil_isqrt_u64(r_in_sq - a_sq));
    }
    const std::uint64_t b_hi = isqrt_u64(r_out_sq - a_sq);
    for (std::uint64_t b_u = b_lo; b_u <= b_hi; ++b_u) {
      const Point p{static_cast<std::int64_t>(a_u),
                    static_cast<std::int64_t>(b_u)};
      if (!gaussian_prime_point(p)) continue;
      out.push_back(Prime{p.a, p.b, norm_sq(p)});
      if (b_u == std::numeric_limits<std::uint64_t>::max()) break;
    }
  }
  return out;
}

inline TileOpLite build_tileop(const TileCoord& tile, const RowConfig& row) {
  TileOpLite op{};
  std::vector<Prime> primes = enumerate_primes_in_box(tile, row);
  if (primes.empty()) {
    op.tile_flags = 0x2;
    return op;
  }

  Dsu local(primes.size());
  for (std::size_t i = 0; i < primes.size(); ++i) {
    for (std::size_t j = i + 1; j < primes.size(); ++j) {
      if (within_k({primes[i].a, primes[i].b},
                   {primes[j].a, primes[j].b}, row.k_sq)) {
        local.unite(i, j);
      }
    }
  }

  std::vector<int> label_by_root(primes.size(), 0);
  int max_label = 0;
  std::vector<std::uint8_t> visible(primes.size(), 0);
  for (std::size_t i = 0; i < primes.size(); ++i) {
    if (geo_inner(primes[i].norm_sq, row) ||
        geo_outer(primes[i].norm_sq, row) ||
        on_any_face_strip(primes[i], tile, row.k_sq)) {
      visible[local.find(i)] = 1;
    }
  }
  for (std::size_t i = 0; i < primes.size(); ++i) {
    const std::size_t root = local.find(i);
    if (!visible[root]) continue;
    if (label_by_root[root] != 0) continue;
    if (max_label >= kMaxGroupsPerTile) {
      op = TileOpLite{};
      op.tile_flags = 0x1;
      return op;
    }
    label_by_root[root] = ++max_label;
  }

  for (std::size_t i = 0; i < primes.size(); ++i) {
    const int label = label_by_root[local.find(i)];
    if (label == 0) continue;
    if (geo_inner(primes[i].norm_sq, row)) bit_set(op.inner_flags, label);
    if (geo_outer(primes[i].norm_sq, row)) bit_set(op.outer_flags, label);
  }

  struct Port {
    std::int64_t h = 0;
    std::int64_t perp = 0;
    std::uint8_t label = 0;
  };

  std::array<std::vector<Port>, 4> ports_by_face;
  int total_ports = 0;
  for (int f = 0; f < 4; ++f) {
    const Face face = static_cast<Face>(f);
    std::vector<std::size_t> face_indices;
    for (std::size_t i = 0; i < primes.size(); ++i) {
      if (on_face_strip(primes[i], tile, face, row.k_sq)) {
        face_indices.push_back(i);
      }
    }
    Dsu face_dsu(face_indices.size());
    for (std::size_t i = 0; i < face_indices.size(); ++i) {
      for (std::size_t j = i + 1; j < face_indices.size(); ++j) {
        if (within_k({primes[face_indices[i]].a, primes[face_indices[i]].b},
                     {primes[face_indices[j]].a, primes[face_indices[j]].b},
                     row.k_sq)) {
          face_dsu.unite(i, j);
        }
      }
    }
    for (const std::size_t root : face_dsu.roots()) {
      bool have = false;
      Port best{};
      for (std::size_t k = 0; k < face_indices.size(); ++k) {
        if (face_dsu.find(k) != root) continue;
        const Prime& p = primes[face_indices[k]];
        const std::int64_t h = face_h(p, tile, face);
        const std::int64_t perp = face_perp(p, tile, face);
        const int label = label_by_root[local.find(face_indices[k])];
        if (!have || h < best.h || (h == best.h && perp < best.perp)) {
          have = true;
          best = Port{h, perp, static_cast<std::uint8_t>(label)};
        }
      }
      ports_by_face[static_cast<std::size_t>(f)].push_back(best);
    }
    std::sort(ports_by_face[static_cast<std::size_t>(f)].begin(),
              ports_by_face[static_cast<std::size_t>(f)].end(),
              [](const Port& lhs, const Port& rhs) {
                if (lhs.h != rhs.h) return lhs.h < rhs.h;
                if (lhs.perp != rhs.perp) return lhs.perp < rhs.perp;
                return lhs.label < rhs.label;
              });
    if (ports_by_face[static_cast<std::size_t>(f)].size() > 255) {
      op = TileOpLite{};
      op.tile_flags = 0x1;
      return op;
    }
    op.n[static_cast<std::size_t>(f)] =
        static_cast<std::uint8_t>(ports_by_face[static_cast<std::size_t>(f)].size());
    total_ports += static_cast<int>(ports_by_face[static_cast<std::size_t>(f)].size());
  }
  if (total_ports > kMaxPortsPerTile) {
    op = TileOpLite{};
    op.tile_flags = 0x1;
    return op;
  }

  int offset = 0;
  for (int f = 0; f < 4; ++f) {
    for (const Port& port : ports_by_face[static_cast<std::size_t>(f)]) {
      op.face_groups[static_cast<std::size_t>(offset++)] = port.label;
    }
  }
  return op;
}

inline std::uint64_t point_key(Point p) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.a)) << 32) |
         static_cast<std::uint32_t>(p.b);
}

inline std::vector<Point> offset_palette(std::uint32_t k_sq) {
  std::vector<Point> offsets;
  const std::int64_t c = static_cast<std::int64_t>(isqrt_u64(k_sq));
  for (std::int64_t da = -c; da <= c; ++da) {
    for (std::int64_t db = -c; db <= c; ++db) {
      if (da == 0 && db == 0) continue;
      if (da * da + db * db <= static_cast<std::int64_t>(k_sq)) {
        offsets.push_back(Point{da, db});
      }
    }
  }
  return offsets;
}

}  // namespace moat_verify
