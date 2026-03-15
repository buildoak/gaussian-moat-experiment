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

// CPU Sieve of Eratosthenes
inline std::vector<uint32_t> simple_sieve_cpu(uint64_t limit) {
    if (limit < 2) return {};
    std::vector<bool> composite(limit + 1, false);
    composite[0] = composite[1] = true;
    for (uint64_t p = 2; p * p <= limit; p++) {
        if (!composite[p]) {
            for (uint64_t m = p * p; m <= limit; m += p) {
                composite[m] = true;
            }
        }
    }
    std::vector<uint32_t> primes;
    for (uint64_t i = 2; i <= limit; i++) {
        if (!composite[i]) primes.push_back((uint32_t)i);
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
