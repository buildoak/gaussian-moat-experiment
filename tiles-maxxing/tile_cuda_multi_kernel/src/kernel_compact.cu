// K3 Compact — bitmap -> row_prefix + prime_pos + prime_count.
// 288 threads/block, target <=32 registers.
// Reads bitmap, does popcount per row, prefix scan, scatter prime positions.

#include "gpu_constants.cuh"
#include "gpu_types.cuh"
#include "gpu_math.cuh"

#include <cstdint>

// ---- Exclusive prefix scan on uint16_t in shared memory ----

__device__ void block_exclusive_scan_u16_k3(uint16_t* data, int n, int tid) {
    const uint16_t original = tid < n ? data[tid] : static_cast<uint16_t>(0u);
    for (int offset = 1; offset < n; offset <<= 1) {
        uint16_t addend = 0u;
        if (tid < n && tid >= offset) {
            addend = data[tid - offset];
        }
        __syncthreads();
        if (tid < n) {
            data[tid] = static_cast<uint16_t>(data[tid] + addend);
        }
        __syncthreads();
    }

    if (tid < n) {
        data[tid] = static_cast<uint16_t>(data[tid] - original);
    }
    __syncthreads();
}

// ---- K3 Compact kernel ----

__global__ void kernel_compact(
    const uint32_t* __restrict__ d_bitmap,
    uint16_t* __restrict__ d_row_prefix,
    uint32_t* __restrict__ d_prime_pos,
    uint32_t* __restrict__ d_prime_count,
    int num_tiles) {
    const int tile_idx = static_cast<int>(blockIdx.x);
    if (tile_idx >= num_tiles) return;

    const int tid = static_cast<int>(threadIdx.x);

    // Per-tile pointers
    const uint32_t* tile_bitmap = d_bitmap + static_cast<size_t>(tile_idx) * BITMAP_WORDS;
    uint16_t* tile_row_prefix = d_row_prefix + static_cast<size_t>(tile_idx) * (ACTIVE_ROWS + 1);
    uint32_t* tile_prime_pos = d_prime_pos + static_cast<size_t>(tile_idx) * MAX_PRIMES_GPU;

    // Shared memory for row_prefix during scan
    extern __shared__ uint16_t smem_row_prefix[];

    uint32_t row_count = 0;
    if (tid < ACTIVE_ROWS) {
        #pragma unroll
        for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
            uint32_t word = tile_bitmap[tid * BITMAP_WORDS_PER_ROW + w];
            if (w == (BITMAP_WORDS_PER_ROW - 1)) {
                word &= LAST_WORD_MASK;
            }
            row_count += __popc(word);
        }
        smem_row_prefix[tid] = static_cast<uint16_t>(row_count);
    }
    __syncthreads();

    block_exclusive_scan_u16_k3(smem_row_prefix, ACTIVE_ROWS, tid);
    __syncthreads();

    if (tid == (ACTIVE_ROWS - 1)) {
        smem_row_prefix[ACTIVE_ROWS] = static_cast<uint16_t>(smem_row_prefix[ACTIVE_ROWS - 1] + row_count);
    }
    __syncthreads();

    // Write row_prefix to global memory
    if (tid <= ACTIVE_ROWS) {
        tile_row_prefix[tid] = smem_row_prefix[tid];
    }
    __syncthreads();

    // Scatter prime positions
    if (tid < ACTIVE_ROWS) {
        uint16_t offset = smem_row_prefix[tid];
        #pragma unroll
        for (int w = 0; w < BITMAP_WORDS_PER_ROW; ++w) {
            uint32_t word = tile_bitmap[tid * BITMAP_WORDS_PER_ROW + w];
            if (w == (BITMAP_WORDS_PER_ROW - 1)) {
                word &= LAST_WORD_MASK;
            }
            while (word != 0u) {
                const int bit = __ffs(word) - 1;
                if (offset < MAX_PRIMES_GPU) {
                    tile_prime_pos[offset] =
                        static_cast<uint32_t>(tid * SIDE_EXP + (w * 32 + bit));
                }
                offset = static_cast<uint16_t>(offset + 1u);
                word &= (word - 1u);
            }
        }
    }
    __syncthreads();

    // Write total prime count
    if (tid == 0) {
        d_prime_count[tile_idx] = static_cast<uint32_t>(smem_row_prefix[ACTIVE_ROWS]);
    }
}

size_t kernel_compact_shared_bytes() {
    return sizeof(uint16_t) * (ACTIVE_ROWS + 1);
}
