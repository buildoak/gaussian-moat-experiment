// kernel.cu — GPU kernel for Gaussian prime generation
//
// Full pipeline:
//   1. Each thread takes a candidate norm n (stride of 4 from batch_start, n ≡ 1 mod 4)
//   2. miller_rabin(n) → if prime → cornacchia(n) → store to output buffer
//
// This kernel handles SPLIT primes only (norms p ≡ 1 mod 4).
// Inert primes (p ≡ 3 mod 4, norm = p²) and the ramified prime 2
// are handled host-side.

#include "types.h"
#include "modular_arith.cuh"
#include "miller_rabin.cuh"
#include "cornacchia.cuh"

// Maximum output primes per kernel launch. The host must ensure this is large enough.
// If the buffer fills, the kernel stops writing (no UB).

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

    if (n >= batch_end) return;
    if (n < 5) return;  // Split primes start at 5
    if (n % 4 != 1) return;  // Safety check

    // Step 1: Primality test
    if (!is_prime(n)) return;

    // Step 2: Cornacchia decomposition
    int32_t a, b;
    if (!cornacchia(n, &a, &b)) return;

    // Step 3: Atomic append to output
    uint32_t slot = atomicAdd(output_count, 1);
    if (slot < max_output) {
        output[slot].a = a;
        output[slot].b = b;
        output[slot].norm = n;
    }
}
