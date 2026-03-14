#include "sieve_kernel.cuh"

#include <climits>

#include <cuda_runtime.h>

#include "cornacchia.cuh"
#include "sieve_base.cuh"

namespace {

constexpr uint32_t kWarpSize = 32u;
constexpr uint32_t kWarpsPerBlock = THREADS_PER_BLK / kWarpSize;
constexpr uint32_t kWordsPerThread = BITMAP_WORDS / THREADS_PER_BLK;

__device__ __forceinline__
uint32_t valid_mask_for_word(uint32_t word_idx, uint32_t valid_odd_count) {
    const uint32_t bit_start = word_idx * 32u;
    if (bit_start >= valid_odd_count) {
        return 0u;
    }
    const uint32_t remaining = valid_odd_count - bit_start;
    if (remaining >= 32u) {
        return 0xFFFFFFFFu;
    }
    return (1u << remaining) - 1u;
}

__device__ __forceinline__
bool first_odd_multiple_in_segment(uint32_t p, uint64_t seg_lo, uint64_t seg_hi, uint64_t* first_multiple) {
    if ((p & 1u) == 0u) {
        return false;
    }

    const uint64_t p64 = static_cast<uint64_t>(p);
    const uint64_t seg_start_odd = seg_lo + 1u;

    uint64_t first;
    const uint64_t p2 = p64 * p64;
    if (p2 >= seg_start_odd) {
        first = p2;
    } else {
        const uint64_t rem = seg_start_odd % p64;
        first = (rem == 0u) ? seg_start_odd : (seg_start_odd + (p64 - rem));
    }

    if ((first & 1u) == 0u) {
        first += p64;
    }

    if (first >= seg_hi) {
        return false;
    }

    *first_multiple = first;
    return true;
}

} // namespace

__global__ void segmented_sieve_kernel(
    uint64_t norm_lo,
    uint64_t norm_hi,
    const uint32_t* __restrict__ base_primes,
    uint32_t base_prime_count,
    uint32_t tiny_prime_count,
    uint32_t small_prime_count,
    const uint64_t* __restrict__ bucket_hits,
    const uint32_t* __restrict__ bucket_offsets,
    uint64_t* __restrict__ output_primes,
    uint32_t max_output,
    uint32_t* output_count
) {
    if (blockDim.x != THREADS_PER_BLK) {
        return;
    }

    // Bucketed large-prime hits are reserved for the follow-up optimization path.
    (void)bucket_hits;
    (void)bucket_offsets;

    if (norm_hi <= norm_lo || output_primes == nullptr || output_count == nullptr) {
        return;
    }

    __shared__ uint32_t bitmap[BITMAP_WORDS];
    __shared__ uint32_t warp_totals[kWarpsPerBlock];
    __shared__ uint32_t block_total;
    __shared__ uint32_t block_base;

    const uint32_t tid = threadIdx.x;
    const uint64_t aligned_lo = norm_lo & ~1ULL;
    if (norm_hi <= aligned_lo) {
        return;
    }

    const uint64_t total_span = norm_hi - aligned_lo;
    const uint64_t segment_count = (total_span + SEGMENT_SPAN - 1u) / SEGMENT_SPAN;
    const uint32_t tiny_limit = (tiny_prime_count > base_prime_count) ? base_prime_count : tiny_prime_count;
    uint32_t small_limit = small_prime_count;
    if (small_limit < tiny_limit) {
        small_limit = tiny_limit;
    }
    if (small_limit > base_prime_count) {
        small_limit = base_prime_count;
    }

    for (uint64_t seg_idx = blockIdx.x; seg_idx < segment_count; seg_idx += gridDim.x) {
        const uint64_t seg_lo = aligned_lo + seg_idx * static_cast<uint64_t>(SEGMENT_SPAN);
        uint64_t seg_hi = seg_lo + static_cast<uint64_t>(SEGMENT_SPAN);
        if (seg_hi > norm_hi) {
            seg_hi = norm_hi;
        }

        const uint32_t valid_odd_count = static_cast<uint32_t>((seg_hi - seg_lo) >> 1);

        for (uint32_t w = tid; w < BITMAP_WORDS; w += blockDim.x) {
            bitmap[w] = 0u;
        }
        __syncthreads();

        // Phase 2A: tiny primes p < 256 use cooperative marking across the block.
        for (uint32_t i = 0; i < tiny_limit; ++i) {
            const uint32_t p = base_primes[i];
            if ((p & 1u) == 0u) {
                continue;
            }

            uint64_t first_multiple = 0u;
            if (!first_odd_multiple_in_segment(p, seg_lo, seg_hi, &first_multiple)) {
                continue;
            }

            const uint64_t first_bit = (first_multiple - seg_lo - 1u) >> 1;
            const uint64_t step_bits = static_cast<uint64_t>(p);
            const uint64_t lane_start = first_bit + static_cast<uint64_t>(tid) * step_bits;
            const uint64_t lane_step = static_cast<uint64_t>(blockDim.x) * step_bits;

            for (uint64_t bit = lane_start; bit < valid_odd_count; bit += lane_step) {
                const uint32_t word = static_cast<uint32_t>(bit >> 5);
                const uint32_t mask = 1u << (bit & 31u);
                atomicOr(&bitmap[word], mask);
            }
        }
        __syncthreads();

        // Phase 2B: medium primes 256 <= p <= SEGMENT_SPAN, round-robin over threads.
        for (uint32_t i = tiny_limit + tid; i < small_limit; i += blockDim.x) {
            const uint32_t p = base_primes[i];
            if ((p & 1u) == 0u) {
                continue;
            }

            uint64_t first_multiple = 0u;
            if (!first_odd_multiple_in_segment(p, seg_lo, seg_hi, &first_multiple)) {
                continue;
            }

            const uint64_t step_bits = static_cast<uint64_t>(p);
            for (uint64_t bit = (first_multiple - seg_lo - 1u) >> 1; bit < valid_odd_count; bit += step_bits) {
                const uint32_t word = static_cast<uint32_t>(bit >> 5);
                const uint32_t mask = 1u << (bit & 31u);
                atomicOr(&bitmap[word], mask);
            }
        }
        __syncthreads();

        // Phase 2C: large primes p > SEGMENT_SPAN, at most one odd hit per segment.
        for (uint32_t i = small_limit + tid; i < base_prime_count; i += blockDim.x) {
            const uint32_t p = base_primes[i];
            if ((p & 1u) == 0u) {
                continue;
            }

            uint64_t first_multiple = 0u;
            if (!first_odd_multiple_in_segment(p, seg_lo, seg_hi, &first_multiple)) {
                continue;
            }

            const uint64_t bit = (first_multiple - seg_lo - 1u) >> 1;
            if (bit < valid_odd_count) {
                const uint32_t word = static_cast<uint32_t>(bit >> 5);
                const uint32_t mask = 1u << (bit & 31u);
                atomicOr(&bitmap[word], mask);
            }
        }
        __syncthreads();

        if (tid == 0 && seg_lo == 0u) {
            bitmap[0] |= 1u;  // Number 1 is not prime.
        }
        __syncthreads();

        uint32_t local_prime_words[kWordsPerThread];
        uint32_t local_word_indices[kWordsPerThread];
        uint32_t local_count = 0u;

#pragma unroll
        for (uint32_t i = 0; i < kWordsPerThread; ++i) {
            const uint32_t word_idx = tid + i * blockDim.x;
            local_word_indices[i] = word_idx;

            const uint32_t valid_mask = valid_mask_for_word(word_idx, valid_odd_count);
            const uint32_t prime_mask = (~bitmap[word_idx]) & valid_mask;
            local_prime_words[i] = prime_mask;
            local_count += static_cast<uint32_t>(__popc(prime_mask));
        }

        const uint32_t lane = tid & (kWarpSize - 1u);
        const uint32_t warp = tid / kWarpSize;

        uint32_t inclusive = local_count;
#pragma unroll
        for (uint32_t offset = 1; offset < kWarpSize; offset <<= 1u) {
            const uint32_t n = __shfl_up_sync(0xFFFFFFFFu, inclusive, offset);
            if (lane >= offset) {
                inclusive += n;
            }
        }

        if (lane == kWarpSize - 1u) {
            warp_totals[warp] = inclusive;
        }
        __syncthreads();

        if (warp == 0u) {
            uint32_t warp_val = (lane < kWarpsPerBlock) ? warp_totals[lane] : 0u;
            uint32_t warp_inclusive = warp_val;
#pragma unroll
            for (uint32_t offset = 1; offset < kWarpSize; offset <<= 1u) {
                const uint32_t n = __shfl_up_sync(0xFFFFFFFFu, warp_inclusive, offset);
                if (lane >= offset) {
                    warp_inclusive += n;
                }
            }

            if (lane < kWarpsPerBlock) {
                warp_totals[lane] = warp_inclusive - warp_val;
            }
            if (lane == kWarpsPerBlock - 1u) {
                block_total = warp_inclusive;
            }
        }
        __syncthreads();

        const uint32_t thread_offset = warp_totals[warp] + (inclusive - local_count);

        if (tid == 0u) {
            block_base = atomicAdd(output_count, block_total);
        }
        __syncthreads();

        uint32_t out_slot = block_base + thread_offset;
#pragma unroll
        for (uint32_t i = 0; i < kWordsPerThread; ++i) {
            uint32_t bits = local_prime_words[i];
            const uint32_t word_idx = local_word_indices[i];

            while (bits != 0u) {
                const uint32_t bit = static_cast<uint32_t>(__ffs(bits) - 1);
                const uint64_t odd_index = static_cast<uint64_t>(word_idx) * 32u + bit;
                const uint64_t value = seg_lo + (odd_index << 1u) + 1u;
                if (value >= norm_lo && value < norm_hi) {
                    if (out_slot < max_output) {
                        output_primes[out_slot] = value;
                    }
                    out_slot++;
                }
                bits &= (bits - 1u);
            }
        }
        __syncthreads();
    }
}

__global__ void cornacchia_dispatch_kernel(
    const uint64_t* __restrict__ primes,
    uint32_t prime_count,
    GaussianPrime* output,
    uint32_t max_output,
    uint32_t* output_count,
    uint64_t norm_lo,
    uint64_t norm_hi
) {
    const uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= prime_count || primes == nullptr || output == nullptr || output_count == nullptr) {
        return;
    }

    const uint64_t p = primes[idx];
    if (p == 2u) {
        return;
    }

    if ((p & 3u) == 1u) {
        int32_t a = 0;
        int32_t b = 0;
        if (cornacchia(p, &a, &b)) {
            const uint32_t slot = atomicAdd(output_count, 1u);
            if (slot < max_output) {
                output[slot].a = a;
                output[slot].b = b;
                output[slot].norm = p;
            }
        }
        return;
    }

    if ((p & 3u) == 3u) {
        const __uint128_t inert_norm = static_cast<__uint128_t>(p) * static_cast<__uint128_t>(p);
        if (inert_norm >= norm_lo && inert_norm < norm_hi && p <= static_cast<uint64_t>(INT32_MAX)) {
            const uint32_t slot = atomicAdd(output_count, 1u);
            if (slot < max_output) {
                output[slot].a = static_cast<int32_t>(p);
                output[slot].b = 0;
                output[slot].norm = static_cast<uint64_t>(inert_norm);
            }
        }
    }
}
