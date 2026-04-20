// include/campaign/primality.h
//
// Deterministic 64-bit rational primality API for campaign code.

#pragma once

#include <cstdint>

namespace campaign {

// Return true iff `n` is rational prime.
//
// Uses the Forisek-Jancina 2015 deterministic single-hash-table witness
// scheme for 64-bit Miller-Rabin: one base-2 round followed by one witness
// selected from the 262144-entry FJ64 table. This witness table is proven
// deterministic for all 64-bit `n`.
bool is_prime(std::uint64_t n);

}  // namespace campaign
