#pragma once

#include "gpu_math.cuh"

// FJ64_16k: Forisek/Jancina deterministic Miller-Rabin for n < 2^64.
// Base-2 test first (catches >99.99% composites), then hash-based
// lookup for 2 additional witnesses. 3 MR rounds total vs 7 Sinclair.

// Register-minimal design: hash inlined with scope-killed temporaries.
// Uses the same loop-based structure as is_prime_norm_gpu to minimize
// nvcc codegen perturbation.
__device__ __forceinline__ bool is_prime_norm_fj64_gpu(uint64_t n, const uint32_t* __restrict__ fj64_table) {
    uint64_t d = n - 1ULL;
    const uint32_t s = ctz64_gpu(d);
    d >>= s;

    const MontCtxGPU ctx = mont_init_gpu(n);

    // Witness 1: base-2 (catches >99.99% of composites)
    if (!miller_rabin_witness_mont_gpu(ctx, d, s, 2ULL)) {
        return false;
    }

    // Hash and table lookup
    uint32_t entry;
    {
        uint64_t h = n;
        h = ((h >> 32) ^ h) * 0x45d9f3b3335b369ULL;
        h = ((h >> 32) ^ h) * 0x3335b36945d9f3bULL;
        h = ((h >> 32) ^ h);
        entry = fj64_table[h & 0x3FFF];
    }

    // Witness 2
    if (!miller_rabin_witness_mont_gpu(ctx, d, s, static_cast<uint64_t>(entry & 0xFFFu))) {
        return false;
    }

    // Witness 3
    if (!miller_rabin_witness_mont_gpu(ctx, d, s, static_cast<uint64_t>(entry >> 12))) {
        return false;
    }

    return true;
}
