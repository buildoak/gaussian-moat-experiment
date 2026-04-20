// include/campaign/constants.h
//
// Compile-time constants and enums for the cpp-campaign-v2 reference build.
//
// All values here are either `constexpr` (build-time) or `inline constexpr`
// derived from the build flag `-DK_SQ=N`. No runtime state lives in this
// header. Symbols match the canonical blueprint §1.1 (glossary) and §5.2
// (TileOp layout).
//
// Dependencies: <cstdint>, <type_traits>. No campaign header deps.
//
// SPDX-License-Identifier: (repo default)

#pragma once

#include <cstddef>
#include <cstdint>

namespace campaign {

// ---------------------------------------------------------------------------
// Build-time scalars
// ---------------------------------------------------------------------------

// Step-squared bound K. Must be injected via CMake cache var (-DK_SQ=N).
// Valid values in v2: 36 or 40. Other values compile but are outside the
// BZ-verified regime; `bz_check.py` acts as the pre-build gate.
#ifndef K_SQ
#error "K_SQ must be defined via -DK_SQ=36 or -DK_SQ=40 at CMake time"
#endif

static_assert(K_SQ > 0, "K_SQ must be positive");
static_assert(K_SQ < 1'000'000, "K_SQ wildly out of range");

// Namespaced constexpr shadow of the K_SQ macro. Tests and non-TU-local
// consumers prefer `campaign::k_sq_value` to the macro because macro
// expansion interferes with `campaign::K_SQ` lookups. The underlying
// value is identical.
inline constexpr int k_sq_value = K_SQ;

// Tile side length in lattice segments. Fixed at 256 for the v2 campaign
// (blueprint §2). Proper tile region has 257 lattice points per side
// (shared-boundary 257×257 convention, docs/supportive/2026-04-15).
inline constexpr int S = 256;

// TileOp packed wire size. Blueprint §5.2. Consumed as `bytes_per_tile`
// in the snapshot header; parity across CPU / future CUDA port hinges on
// this being exactly 256 bytes.
inline constexpr int TILEOP_SIZE = 256;
static_assert(TILEOP_SIZE == 256, "TileOp wire size is fixed at 256 bytes");

// Grid offset. (1, 1) per blueprint §4.1 — sidesteps BACKLOG B1 axis-boundary
// active-tile ambiguity at zero structural cost.
inline constexpr int OFFSET_X = 1;
inline constexpr int OFFSET_Y = 1;

// Per-tile compacted-prime capacity inherited from the CUDA worker.
// Upper bound on the number of Gaussian primes any tile's halo-expanded
// region can contribute to the local UF. Sized from empirical profiling at
// project parameters. Blueprint §1.1 glossary.
inline constexpr int MAX_PRIMES_GPU = 6144;

// UF label / group budget per tile. 128 groups fits in a 16-byte bit array
// per flag class (inner / outer) and is the wire-format hard cap for
// `inner_flags` / `outer_flags` in TileOp. Blueprint §5.2.
inline constexpr int MAX_GROUPS_PER_TILE = 128;

// Maximum total face ports per tile (sum of `n[4]`). Blueprint §5.2
// port-count budget. Overflow triggers `OVERFLOW_BIT` + zeroed payload.
inline constexpr int MAX_PORTS_PER_TILE = 192;

// ---------------------------------------------------------------------------
// Derived: floor_isqrt, collar, SIDE_EXP
// ---------------------------------------------------------------------------

// Integer floor-sqrt via pure integer loop — NO std::sqrt, no floating
// point. Forbidden-pattern rule §6.3 #1. Used at build-config time to
// derive C from K_SQ.
//
// Correctness: loop terminates at the unique non-negative integer r such
// that r*r <= n < (r+1)*(r+1). Linear in r = O(sqrt(n)). For K_SQ ≤ 40,
// fewer than 7 iterations.
constexpr std::int64_t floor_isqrt(std::int64_t n) noexcept {
  if (n < 0) return 0;
  std::int64_t r = 0;
  // Walk r up while (r+1)^2 <= n. Safe against overflow for any n < 2^62.
  while ((r + 1) * (r + 1) <= n) {
    ++r;
  }
  return r;
}

// Companion: ceil_isqrt. Used by the pre-filter in `geo_tests`
// (blueprint §2 "Integer-overflow pre-filter"). For non-square K this is
// strictly greater than floor_isqrt(K); using floor here is a canonical
// false-negative bug (plan §6.3 rule 10 and commit 46d73db).
constexpr std::int64_t ceil_isqrt(std::int64_t n) noexcept {
  std::int64_t f = floor_isqrt(n);
  return (f * f == n) ? f : (f + 1);
}

// Collar / face-strip depth / halo extension. C = floor_isqrt(K_SQ).
// Blueprint §2. At K_SQ ∈ {36, 40}: C = 6.
inline constexpr int C = static_cast<int>(floor_isqrt(K_SQ));
static_assert(C >= 1, "Collar must be at least 1");

// Halo / collar aliases. Kept distinct for grep-ability against the math
// doc even though all three equal `C` in v3.
inline constexpr int HALO = C;
inline constexpr int COLLAR = C;

// Halo-expanded lattice side. `S + 1 + 2 * C` per blueprint §1.1 glossary.
// At project parameters S=256, C=6 → SIDE_EXP = 269.
inline constexpr int SIDE_EXP = S + 1 + 2 * C;
static_assert(SIDE_EXP > 0, "SIDE_EXP derivation broken");

// ---------------------------------------------------------------------------
// Face enum
// ---------------------------------------------------------------------------

// TileOp face order (blueprint §5.2). Order is WIRE-STABLE; reordering
// breaks snapshot parity. Underlying type is uint8_t for direct indexing
// into `TileOp::n[4]` and the face_groups prefix-sum table.
enum class Face : std::uint8_t {
  I = 0,  // Inner (bottom) face, shared with T_{i, j-1}.face_O
  O = 1,  // Outer (top) face,    shared with T_{i, j+1}.face_I
  L = 2,  // Left face,           shared with T_{i-1, j}.face_R
  R = 3,  // Right face,          shared with T_{i+1, j}.face_L
};

inline constexpr int NUM_FACES = 4;

// ---------------------------------------------------------------------------
// TileOp flag bits (tile_flags byte, blueprint §5.2)
// ---------------------------------------------------------------------------

// bit 0: Port-count overflow (sum(n) > MAX_PORTS_PER_TILE) OR group-count
// overflow (max_label > MAX_GROUPS_PER_TILE). Compositor treats the tile as
// conservatively SPANNING (blueprint §10).
inline constexpr std::uint8_t OVERFLOW_BIT = 0x01;

// bit 1: Tile produced no Gaussian-prime UF components. Zero ports, zero
// groups. Compositor skips ingestion. Blueprint §7.2.
inline constexpr std::uint8_t EMPTY_BIT = 0x02;

// bit 2: Tower-closing tile (tower_height ≤ 1 at column near R_outer/√2).
// Currently informational; no compositor branch reads this in v3, but the
// bit is reserved for downstream audit tooling.
inline constexpr std::uint8_t TOWER_CLOSING_BIT = 0x04;

// Remaining bits [3..7] reserved. Must be zero on encode.

}  // namespace campaign
