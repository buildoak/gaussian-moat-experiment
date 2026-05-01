#pragma once
#include "constants.h"
#include <cstdint>

// Phase 2: Compact the sparse bitmap into dense arrays.
// Builds prefix popcount table and dense prime_pos array.
// Returns total prime count.
int compact_primes(const uint32_t* bitmap, uint32_t* prefix, uint32_t* prime_pos);

// Convert a bitmap position to the corresponding union-find index
// using the prefix popcount table.
inline int bitmap_pos_to_uf_index(uint32_t pos, const uint32_t* bitmap, const uint32_t* prefix) {
    const uint32_t word = pos >> 5;
    const uint32_t bit = pos & 31U;
    return static_cast<int>(
        prefix[word] + __builtin_popcount(bitmap[word] & ((1U << bit) - 1U))
    );
}
