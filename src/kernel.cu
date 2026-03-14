// kernel.cu — GPU kernel for Gaussian prime generation
//
// Full pipeline:
//   1. Each thread takes a candidate norm n (stride of 4 from batch_start, n ≡ 1 mod 4)
//   2. miller_rabin(n) → if prime → cornacchia(n) → store to output buffer
//
// Optimizations:
//   - Warp-level output compaction using __ballot_sync + __popc to reduce
//     atomic contention. Instead of per-thread atomicAdd, each warp does
//     one atomicAdd for all its primes, then threads scatter into the
//     allocated slots using warp-prefix-sum.
//
// This kernel handles SPLIT primes only (norms p ≡ 1 mod 4).
// Inert primes (p ≡ 3 mod 4, norm = p²) and the ramified prime 2
// are handled host-side.

#include "types.h"
#include "modular_arith.cuh"
#include "miller_rabin.cuh"
#include "cornacchia.cuh"

__global__
void gaussian_prime_kernel(
    uint64_t batch_start,   // First candidate norm (must be ≡ 1 mod 4)
    uint64_t batch_end,     // Exclusive upper bound
    GaussianPrime* output,  // Output buffer (device memory)
    uint32_t max_output,    // Size of output buffer
    uint32_t* output_count  // Atomic counter (device memory, initialized to 0)
) {
    uint64_t idx = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    uint64_t n = batch_start + idx * 4;  // Stride of 4 to stay in ≡ 1 (mod 4)

    // --- Primality + Cornacchia ---
    bool is_valid = false;
    int32_t a = 0, b = 0;

    // batch_start is aligned ≡ 1 (mod 4) and stride is 4, so n ≡ 1 (mod 4)
    // by construction. Skip the redundant n % 4 == 1 check.
    if (n < batch_end && n >= 5) {
        if (is_prime(n)) {
            if (cornacchia(n, &a, &b)) {
                is_valid = true;
            }
        }
    }

    // --- Warp-level output compaction ---
    // Use ballot to find which threads in this warp have a prime
    unsigned mask = __ballot_sync(0xFFFFFFFF, is_valid);
    int warp_count = __popc(mask);

    if (warp_count == 0) return;

    // Lane 0 of each warp does the atomic reservation for the whole warp
    uint32_t warp_base = 0;
    int lane = threadIdx.x & 31;
    if (lane == 0) {
        warp_base = atomicAdd(output_count, warp_count);
    }
    // Broadcast warp_base from lane 0 to all lanes
    warp_base = __shfl_sync(0xFFFFFFFF, warp_base, 0);

    // Each thread computes its offset within the warp's allocation
    // by counting set bits below its lane in the ballot mask
    if (is_valid) {
        unsigned lower_mask = (1u << lane) - 1;  // bits below this lane
        int offset = __popc(mask & lower_mask);
        uint32_t slot = warp_base + offset;
        if (slot < max_output) {
            output[slot].a = a;
            output[slot].b = b;
            output[slot].norm = n;
        }
    }
}
