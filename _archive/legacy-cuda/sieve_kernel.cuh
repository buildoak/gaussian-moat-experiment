#ifndef GM_SIEVE_KERNEL_CUH
#define GM_SIEVE_KERNEL_CUH

#include <cstdint>

#include "types.h"

__global__ void segmented_sieve_kernel(
    uint64_t norm_lo,
    uint64_t norm_hi,
    const uint32_t* __restrict__ base_primes,
    uint32_t base_prime_count,
    uint32_t tiny_prime_count,
    uint32_t small_prime_count,
    const uint64_t* __restrict__ bucket_hits,
    const uint64_t* __restrict__ bucket_offsets,
    uint64_t* __restrict__ output_primes,
    uint32_t max_output,
    uint32_t* output_count
);

__global__ void cornacchia_dispatch_kernel(
    const uint64_t* __restrict__ primes,
    uint32_t prime_count,
    GaussianPrime* output,
    uint32_t max_output,
    uint32_t* output_count,
    uint64_t norm_lo,
    uint64_t norm_hi
);

#endif // GM_SIEVE_KERNEL_CUH
