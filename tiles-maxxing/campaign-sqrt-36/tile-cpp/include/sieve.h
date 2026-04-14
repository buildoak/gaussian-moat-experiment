#pragma once

#include "types.h"

#include <cstdint>

struct SieveTables {
    uint32_t split_table[SPLIT_PRIMES_COUNT];
    uint16_t inert_primes[INERT_PRIMES_COUNT];
    int split_count;
    int inert_count;
};

// Call once at startup. Computes sqrt(-1) mod p for all split primes,
// packs sieve table. Returns false on error.
bool init_sieve_tables(SieveTables& tables);

// Sieve the 271x271 domain for tile at (coord.a_lo, coord.b_lo).
// Output: bitmap[BITMAP_WORDS] with 1 = Gaussian prime, 0 = not prime.
void sieve_tile(const TileCoord& coord, const SieveTables& tables, uint32_t* bitmap);
