// src/tileop.cpp
//
// TileOp local-UF, face-strip-UF, canonical port ordering, and 256 B encode.

#include "tileop_internal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "campaign/constants.h"

namespace campaign {

static_assert(sizeof(TileOp) == 256, "TileOp must be exactly 256 bytes");

namespace internal {
namespace {

struct PrimeWithFlags {
  Prime prime;
  PrimeGeoFlags flags;
};

struct Port {
  std::int64_t h = 0;
  std::int64_t p_perp = 0;
  std::uint8_t global_wire_label = 0;
};

constexpr int face_index(Face face) noexcept {
  return static_cast<int>(face);
}

bool prime_less(const Prime& lhs, const Prime& rhs) noexcept {
  if (lhs.a != rhs.a) return lhs.a < rhs.a;
  if (lhs.b != rhs.b) return lhs.b < rhs.b;
  if (lhs.norm_sq != rhs.norm_sq) return lhs.norm_sq < rhs.norm_sq;
  return lhs.packed_pos < rhs.packed_pos;
}

bool within_k_sq(const Prime& lhs, const Prime& rhs) noexcept {
  const __int128 da = static_cast<__int128>(lhs.a) - static_cast<__int128>(rhs.a);
  const __int128 db = static_cast<__int128>(lhs.b) - static_cast<__int128>(rhs.b);
  return da * da + db * db <= static_cast<__int128>(k_sq_value);
}

std::int64_t rel_col(const Prime& p, const TileCoord& coord) noexcept {
  return p.a - coord.a_lo;
}

std::int64_t rel_row(const Prime& p, const TileCoord& coord) noexcept {
  return p.b - coord.b_lo;
}

std::int64_t face_h(const Prime& p, const TileCoord& coord, Face face) noexcept {
  switch (face) {
    case Face::I:
    case Face::O:
      return rel_col(p, coord);
    case Face::L:
    case Face::R:
      return rel_row(p, coord);
  }
  return 0;
}

std::int64_t face_perp(const Prime& p, const TileCoord& coord, Face face) noexcept {
  switch (face) {
    case Face::I:
      return rel_row(p, coord);
    case Face::O:
      return rel_row(p, coord) - S;
    case Face::L:
      return rel_col(p, coord);
    case Face::R:
      return rel_col(p, coord) - S;
  }
  return 0;
}

bool on_face_strip(const Prime& p, const TileCoord& coord, Face face) noexcept {
  const std::int64_t p_perp = face_perp(p, coord, face);
  return -static_cast<std::int64_t>(C) <= p_perp &&
         p_perp <= static_cast<std::int64_t>(C);
}

std::vector<Port> build_face_ports(const std::vector<Prime>& primes,
                                   DSU* local_dsu,
                                   const DenseRemap& remap,
                                   const TileCoord& coord,
                                   Face face) {
  std::vector<std::int32_t> face_indices;
  face_indices.reserve(primes.size());
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(primes.size()); ++i) {
    if (on_face_strip(primes[static_cast<std::size_t>(i)], coord, face)) {
      face_indices.push_back(i);
    }
  }

  DSU face_dsu(static_cast<std::int32_t>(face_indices.size()));
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(face_indices.size()); ++i) {
    for (std::int32_t j = i + 1; j < static_cast<std::int32_t>(face_indices.size()); ++j) {
      const Prime& lhs = primes[static_cast<std::size_t>(face_indices[static_cast<std::size_t>(i)])];
      const Prime& rhs = primes[static_cast<std::size_t>(face_indices[static_cast<std::size_t>(j)])];
      if (within_k_sq(lhs, rhs)) {
        face_dsu.unite(i, j);
      }
    }
  }

  std::vector<Port> ports;
  const std::vector<std::int32_t> roots = face_dsu.roots();
  ports.reserve(roots.size());
  for (const std::int32_t root : roots) {
    bool have_rep = false;
    std::int64_t best_h = 0;
    std::int64_t best_perp = 0;
    std::uint8_t label = 0;

    for (std::int32_t k = 0; k < static_cast<std::int32_t>(face_indices.size()); ++k) {
      if (face_dsu.find(k) != root) continue;
      const std::int32_t prime_idx = face_indices[static_cast<std::size_t>(k)];
      const Prime& prime = primes[static_cast<std::size_t>(prime_idx)];
      const std::int64_t h = face_h(prime, coord, face);
      const std::int64_t p_perp = face_perp(prime, coord, face);
      if (!have_rep || h < best_h || (h == best_h && p_perp < best_perp)) {
        have_rep = true;
        best_h = h;
        best_perp = p_perp;
        const std::int32_t raw_root = local_dsu->find(prime_idx);
        label = remap.wire_label_by_raw_root[static_cast<std::size_t>(raw_root)];
      }
    }

    ports.push_back(Port{best_h, best_perp, label});
  }

  std::sort(ports.begin(), ports.end(), [](const Port& lhs, const Port& rhs) {
    if (lhs.h != rhs.h) return lhs.h < rhs.h;
    if (lhs.p_perp != rhs.p_perp) return lhs.p_perp < rhs.p_perp;
    return lhs.global_wire_label < rhs.global_wire_label;
  });
  return ports;
}

TileOp overflow_tileop() noexcept {
  TileOp out{};
  out.tile_flags = OVERFLOW_BIT;
  return out;
}

}  // namespace

DSU build_local_dsu(const std::vector<Prime>& primes) {
  DSU dsu(static_cast<std::int32_t>(primes.size()));
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(primes.size()); ++i) {
    for (std::int32_t j = i + 1; j < static_cast<std::int32_t>(primes.size()); ++j) {
      if (within_k_sq(primes[static_cast<std::size_t>(i)],
                      primes[static_cast<std::size_t>(j)])) {
        dsu.unite(i, j);
      }
    }
  }
  return dsu;
}

DenseRemap dense_remap_roots(DSU* dsu, std::int32_t prime_count) {
  std::vector<std::int32_t> raw_roots;
  raw_roots.reserve(static_cast<std::size_t>(prime_count));
  for (std::int32_t i = 0; i < prime_count; ++i) {
    raw_roots.push_back(dsu->find(i));
  }
  return dense_remap_raw_roots_for_test(raw_roots, prime_count);
}

DenseRemap dense_remap_raw_roots_for_test(const std::vector<std::int32_t>& raw_roots,
                                          std::int32_t raw_root_bound) {
  DenseRemap remap;
  remap.zero_based_by_raw_root.assign(static_cast<std::size_t>(raw_root_bound), -1);
  remap.wire_label_by_raw_root.assign(static_cast<std::size_t>(raw_root_bound), 0);

  for (const std::int32_t raw_root : raw_roots) {
    auto& zero_based =
        remap.zero_based_by_raw_root[static_cast<std::size_t>(raw_root)];
    if (zero_based >= 0) continue;

    if (remap.max_label >= MAX_GROUPS_PER_TILE) {
      remap.overflow = true;
      return remap;
    }

    zero_based = remap.max_label;
    remap.wire_label_by_raw_root[static_cast<std::size_t>(raw_root)] =
        static_cast<std::uint8_t>(remap.max_label + 1);
    ++remap.max_label;
  }

  return remap;
}

TileOp build_tileop_for_primes(std::vector<Prime> primes,
                               std::vector<PrimeGeoFlags> prime_flags,
                               const TileCoord& coord,
                               const CampaignConstants& constants) {
  (void)constants;
  if (primes.size() != prime_flags.size()) {
    throw std::invalid_argument("prime_flags size must match primes size");
  }

  std::vector<PrimeWithFlags> zipped;
  zipped.reserve(primes.size());
  for (std::size_t i = 0; i < primes.size(); ++i) {
    zipped.push_back(PrimeWithFlags{primes[i], prime_flags[i]});
  }
  std::sort(zipped.begin(), zipped.end(), [](const PrimeWithFlags& lhs,
                                             const PrimeWithFlags& rhs) {
    return prime_less(lhs.prime, rhs.prime);
  });
  for (std::size_t i = 0; i < zipped.size(); ++i) {
    primes[i] = zipped[i].prime;
    prime_flags[i] = zipped[i].flags;
  }

  TileOp out{};
  if (primes.empty()) {
    out.tile_flags = EMPTY_BIT;
    return out;
  }

  DSU local_dsu = build_local_dsu(primes);
  DenseRemap remap =
      dense_remap_roots(&local_dsu, static_cast<std::int32_t>(primes.size()));
  if (remap.overflow) {
    return overflow_tileop();
  }

  for (std::int32_t i = 0; i < static_cast<std::int32_t>(primes.size()); ++i) {
    const std::int32_t raw_root = local_dsu.find(i);
    const std::uint8_t label =
        remap.wire_label_by_raw_root[static_cast<std::size_t>(raw_root)];
    if (prime_flags[static_cast<std::size_t>(i)].inner) {
      bit_set(out.inner_flags, label);
    }
    if (prime_flags[static_cast<std::size_t>(i)].outer) {
      bit_set(out.outer_flags, label);
    }
  }

  const std::array<Face, NUM_FACES> faces = {
      Face::I, Face::O, Face::L, Face::R};
  std::array<std::vector<Port>, NUM_FACES> ports_by_face;
  int total_ports = 0;
  for (const Face face : faces) {
    auto ports = build_face_ports(primes, &local_dsu, remap, coord, face);
    if (ports.size() > 255u) {
      return overflow_tileop();
    }
    out.n[face_index(face)] = static_cast<std::uint8_t>(ports.size());
    total_ports += static_cast<int>(ports.size());
    ports_by_face[face_index(face)] = std::move(ports);
  }

  if (total_ports > MAX_PORTS_PER_TILE) {
    return overflow_tileop();
  }

  int write_offset = 0;
  for (const Face face : faces) {
    for (const Port& port : ports_by_face[face_index(face)]) {
      out.face_groups[write_offset] = port.global_wire_label;
      ++write_offset;
    }
  }

  return out;
}

}  // namespace internal
}  // namespace campaign
