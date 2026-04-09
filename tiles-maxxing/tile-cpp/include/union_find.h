#pragma once

#include "constants.h"
#include <cstdint>

// Precomputed backward offsets for neighbor scan.
// All (dr, dc) with dr^2 + dc^2 <= K_SQ and (dr < 0 or (dr == 0 and dc < 0)).
struct BackwardOffsets {
    int dr[64];
    int dc[64];
    int count;  // should be 64
};

// Compute backward offset table (call once at init).
void init_backward_offsets(BackwardOffsets& offsets);

// Phase 3: Build connected components over all primes in the sieve domain.
// Uses backward-offset neighbor scan + union-find with path halving.
//
// bitmap: prime bitmap (BITMAP_WORDS words)
// prefix: prefix popcount table from compact phase
// prime_pos: dense prime positions from compact phase
// prime_count: number of primes
// parent: output, uint16_t[prime_count], flattened component roots
void build_components(const uint32_t* bitmap, const uint32_t* prefix,
                      const uint32_t* prime_pos, int prime_count,
                      uint16_t* parent);
