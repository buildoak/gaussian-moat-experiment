// include/campaign/tileop.h
//
// TileOp 256-byte packed wire format + tile-processing pipeline API for
// the cpp-campaign-v2 reference build.
//
// TileOp layout is LOCKED at byte offsets per blueprint §5.2:
//
//     offset 0   : uint8_t n[4]             (4 B)   — port counts per face
//     offset 4   : uint8_t face_groups[192] (192 B) — group labels
//     offset 196 : uint8_t inner_flags[16]  (16 B)  — bit-packed flags 1..128
//     offset 212 : uint8_t outer_flags[16]  (16 B)  — bit-packed flags 1..128
//     offset 228 : uint8_t tile_flags       (1 B)   — OVERFLOW / EMPTY / TC
//     offset 229 : uint8_t reserved[27]     (27 B)  — zero-init
//
//     sizeof(TileOp) == 256
//
// Snapshot bit-parity across CPU / future CUDA port hinges on this
// layout being byte-identical. Changing any offset invalidates every
// committed golden.
//
// Pipeline (blueprint §5 + §6.2–6.3):
//   1) Local UF  : Union Gaussian primes at squared distance ≤ K.
//   2) Dense-remap: raw DSU root in [0, MAX_PRIMES_GPU) → dense label in
//                  [0, max_label). Overflow when max_label > 128.
//   3) Face-strip UF: per face F, sub-UF over F-face strip primes. Ports
//                    are the components.
//   4) Canonical positional port sort: lex (h, p⊥) primary, (p⊥, h)
//                                     secondary tiebreak.
//   5) 256 B encode: fixed offsets; zero reserved[27].
//
// See docs/tileop-design.md for the full Phase 2 spec.
//
// Dependencies: constants.h, grid.h, campaign_constants.h, sieve.h.

#pragma once

#include <cstdint>

#include "campaign/campaign_constants.h"
#include "campaign/constants.h"
#include "campaign/grid.h"
#include "campaign/sieve.h"

namespace campaign {

// TileOp packed wire format (256 B). Standard-layout POD; memcpy-safe.
//
// Field order here MUST match the offsets declared at file-top. The
// `static_assert`s at bottom lock each offset; touching them requires
// regenerating every golden.
struct TileOp {
  std::uint8_t n[4];                  // offset   0, size   4
  std::uint8_t face_groups[192];      // offset   4, size 192
  std::uint8_t inner_flags[16];       // offset 196, size  16
  std::uint8_t outer_flags[16];       // offset 212, size  16
  std::uint8_t tile_flags;            // offset 228, size   1
  std::uint8_t reserved[27];          // offset 229, size  27
};

static_assert(sizeof(TileOp) == TILEOP_SIZE,
              "TileOp must be exactly 256 bytes");
static_assert(sizeof(TileOp) == 256,
              "TileOp wire size is locked at 256");
static_assert(offsetof(TileOp, n) == 0, "n[4] offset must be 0");
static_assert(offsetof(TileOp, face_groups) == 4, "face_groups offset must be 4");
static_assert(offsetof(TileOp, inner_flags) == 196, "inner_flags offset must be 196");
static_assert(offsetof(TileOp, outer_flags) == 212, "outer_flags offset must be 212");
static_assert(offsetof(TileOp, tile_flags) == 228, "tile_flags offset must be 228");
static_assert(offsetof(TileOp, reserved) == 229, "reserved offset must be 229");

// -----------------------------------------------------------------------------
// Pipeline entry point
// -----------------------------------------------------------------------------

// Run the per-tile pipeline on one TileCoord and produce a 256 B TileOp.
//
// Preconditions:
//   * grid has been built; coord was emitted by grid.enumerate_active_tiles().
//   * constants was built via CampaignConstants::from_radii().
//
// Postconditions:
//   * out is zero-initialized then populated per blueprint §5.2.
//   * If sum(n) > 192 or max_label > 128: out.tile_flags |= OVERFLOW_BIT
//     and face_groups / inner_flags / outer_flags are all zero.
//   * If the tile contributes no UF components: out.tile_flags |= EMPTY_BIT
//     and all other fields are zero.
//
// Determinism:
//   * Output bytes are a deterministic function of (coord, constants) —
//     no thread-ID dependence, no PRNG, no pointer-equality sorting.
//   * Re-running on identical inputs produces identical bytes (plan M6
//     determinism gate).
//
// STUB in Phase 1: writes an EMPTY_BIT TileOp and returns. Full impl in M4.
TileOp process_tile(const TileCoord& coord,
                    const CampaignConstants& constants,
                    const Grid& grid);

void process_tile(const TileCoord& coord,
                  const Grid& grid,
                  const CampaignConstants& constants,
                  TileOp* out);

// -----------------------------------------------------------------------------
// Flag helpers (inline for both CPU reference and future test assertions)
// -----------------------------------------------------------------------------

// Test bit (g - 1) of the 128-bit flag mask stored in `flags[16]`.
// Group labels are 1-indexed per blueprint §5.3 — labels 1..128.
inline bool bit_test(const std::uint8_t* flags, int group_label_1based) noexcept {
  const int g0 = group_label_1based - 1;
  return (flags[g0 >> 3] >> (g0 & 7)) & 0x1;
}

// Set bit (g - 1) of the 128-bit flag mask stored in `flags[16]`.
inline void bit_set(std::uint8_t* flags, int group_label_1based) noexcept {
  const int g0 = group_label_1based - 1;
  flags[g0 >> 3] |= static_cast<std::uint8_t>(1u << (g0 & 7));
}

// Compute face_groups prefix-sum offset for face f.
// f=I -> 0; f=O -> n[0]; f=L -> n[0]+n[1]; f=R -> n[0]+n[1]+n[2].
inline int face_offset(const TileOp& op, Face f) noexcept {
  const int idx = static_cast<int>(f);
  int off = 0;
  for (int k = 0; k < idx; ++k) off += op.n[k];
  return off;
}

}  // namespace campaign
