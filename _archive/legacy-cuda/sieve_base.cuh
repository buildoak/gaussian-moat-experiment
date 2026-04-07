#ifndef GM_SIEVE_BASE_CUH
#define GM_SIEVE_BASE_CUH

#include <vector>
#include <cstdint>
#include <cmath>

#include "device_config.cuh"

// Sieve constants — derived from device_config.cuh
#define SEGMENT_SPAN    DEVICE_SEGMENT_SPAN
#define BITMAP_WORDS    DEVICE_BITMAP_WORDS
#define BITMAP_BYTES    DEVICE_BITMAP_BYTES
#define THREADS_PER_BLK 256u
#define TINY_THRESHOLD  256u            // Primes below this use cooperative marking

// CPU segmented Sieve of Eratosthenes.
// Uses L1-sized blocks (~16KB working set) to avoid cache thrashing.
// The naive vector<bool> version uses ~125MB at 10^18 scale; this stays under 32KB.
inline std::vector<uint32_t> simple_sieve_cpu(uint64_t limit) {
    if (limit < 2) return {};

    // Phase 1: small sieve up to sqrt(limit) using classic method
    const uint64_t sqrt_limit = (uint64_t)sqrt((double)limit) + 1;
    std::vector<bool> small_composite(sqrt_limit + 1, false);
    small_composite[0] = small_composite[1] = true;
    for (uint64_t p = 2; p * p <= sqrt_limit; p++) {
        if (!small_composite[p]) {
            for (uint64_t m = p * p; m <= sqrt_limit; m += p) {
                small_composite[m] = true;
            }
        }
    }
    std::vector<uint32_t> small_primes;
    for (uint64_t i = 2; i <= sqrt_limit; i++) {
        if (!small_composite[i]) small_primes.push_back((uint32_t)i);
    }

    // Phase 2: segmented sieve in L1-friendly blocks
    // 16KB = 131072 bits; each bit represents an odd number → covers 262144 values.
    const uint64_t SEG_SIZE = 262144ULL;  // values per segment (fits in 16KB as bit-per-odd)
    const uint64_t SEG_BYTES = SEG_SIZE / 16; // bytes for odd-only bitmap

    std::vector<uint32_t> primes;
    // Add 2 first
    if (limit >= 2) primes.push_back(2);

    // Add small odd primes found in phase 1
    for (uint32_t p : small_primes) {
        if (p == 2) continue;
        primes.push_back(p);
    }

    // Segment from sqrt_limit+1 onward, processing only odd numbers.
    // Each segment covers SEG_SIZE consecutive integers (SEG_SIZE/2 odd slots).
    uint64_t seg_lo = sqrt_limit + 1;
    if (seg_lo % 2 == 0) seg_lo++; // start odd

    std::vector<uint8_t> sieve_block(SEG_BYTES, 0);

    while (seg_lo <= limit) {
        // seg_hi is the largest odd number in this segment
        uint64_t seg_hi = seg_lo + SEG_SIZE - 2; // covers SEG_SIZE/2 odd numbers
        if (seg_hi > limit) seg_hi = limit;

        // Clear bitmap (bit=0 means prime candidate)
        std::fill(sieve_block.begin(), sieve_block.end(), 0);

        // Mark composites using small primes
        for (uint32_t sp : small_primes) {
            if (sp < 3) continue;
            uint64_t p = sp;
            // Find first odd multiple of p >= seg_lo
            uint64_t start = p * p;
            if (start < seg_lo) {
                uint64_t rem = seg_lo % p;
                start = (rem == 0) ? seg_lo : (seg_lo + p - rem);
            }
            if (start % 2 == 0) start += p;  // ensure odd

            for (uint64_t m = start; m <= seg_hi; m += 2 * p) {
                uint64_t idx = (m - seg_lo) / 2;
                sieve_block[idx / 8] |= (1u << (idx % 8));
            }
        }

        // Collect surviving primes from this segment
        for (uint64_t n = seg_lo; n <= seg_hi; n += 2) {
            uint64_t idx = (n - seg_lo) / 2;
            if (!(sieve_block[idx / 8] & (1u << (idx % 8)))) {
                primes.push_back((uint32_t)n);
            }
        }

        // Advance to next segment: seg_lo moves by SEG_SIZE (even), keeping it odd
        seg_lo += SEG_SIZE;
    }

    return primes;
}

// Returns index of first prime > threshold in sorted primes array
inline uint32_t partition_primes(const std::vector<uint32_t>& primes, uint32_t threshold) {
    for (uint32_t i = 0; i < primes.size(); i++) {
        if (primes[i] > threshold) return i;
    }
    return (uint32_t)primes.size();
}

#endif // GM_SIEVE_BASE_CUH
