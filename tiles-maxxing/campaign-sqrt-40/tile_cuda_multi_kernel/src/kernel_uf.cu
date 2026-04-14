// K4 UF — Parallel union-find with all 288 threads.
// 288 threads/block, target <=40 registers.
// Cooperative load of bitmap + row_prefix + prime_pos into shared memory.
// All threads participate in neighbor discovery + atomic union operations.
// Multi-pass: union pass -> path compression pass -> repeat until convergence.

#include "gpu_constants.cuh"
#include "gpu_types.cuh"
#include "gpu_math.cuh"

#include <cstdint>

// ---- Bitmap test ----

static __device__ __forceinline__ bool gpu_bitmap_test_uf(const uint32_t* bitmap, int row, int col) {
    return ((bitmap[row * BITMAP_WORDS_PER_ROW + (col >> 5)] >> (col & 31)) & 1u) != 0u;
}

// ---- Optimized prime index lookup ----

static __device__ __forceinline__ int gpu_uf_index(
    int row, int col, const uint32_t* bitmap, const uint16_t* row_prefix) {
    uint32_t idx = row_prefix[row];
    const int base = row * BITMAP_WORDS_PER_ROW;
    const int full_words = col >> 5;

    if (full_words > 0) idx += __popc(bitmap[base + 0]);
    if (full_words > 1) idx += __popc(bitmap[base + 1]);
    if (full_words > 2) idx += __popc(bitmap[base + 2]);
    if (full_words > 3) idx += __popc(bitmap[base + 3]);
    if (full_words > 4) idx += __popc(bitmap[base + 4]);
    if (full_words > 5) idx += __popc(bitmap[base + 5]);
    if (full_words > 6) idx += __popc(bitmap[base + 6]);
    if (full_words > 7) idx += __popc(bitmap[base + 7]);

    const uint32_t bit_mask = (col & 31) == 0 ? 0u : ((1u << (col & 31)) - 1u);
    idx += __popc(bitmap[base + full_words] & bit_mask);
    return static_cast<int>(idx);
}

// ---- Atomic lock-free union-find ----

static __device__ __forceinline__ uint16_t atomic_find_root(uint16_t* parent, uint16_t x) {
    uint16_t r = x;
    while (true) {
        uint16_t p = parent[r];
        if (p == r) break;
        // Path splitting: point to grandparent
        uint16_t gp = parent[p];
        // Use atomicCAS for safe path compression
        atomicCAS(reinterpret_cast<unsigned short*>(&parent[r]), r, gp);
        r = p;
    }
    return r;
}

static __device__ __forceinline__ bool atomic_union(uint16_t* parent, uint16_t x, uint16_t y) {
    while (true) {
        uint16_t rx = atomic_find_root(parent, x);
        uint16_t ry = atomic_find_root(parent, y);
        if (rx == ry) return false;  // already same component

        // Always make smaller root the parent (deterministic)
        if (rx > ry) {
            uint16_t tmp = rx; rx = ry; ry = tmp;
        }

        // Try to set parent[ry] = rx
        uint16_t old = atomicCAS(reinterpret_cast<unsigned short*>(&parent[ry]),
                                 static_cast<unsigned short>(ry),
                                 static_cast<unsigned short>(rx));
        if (old == ry) return true;  // success
        // If failed, someone else modified parent[ry]; retry
    }
}

// ---- K4 UF kernel ----

__global__ void kernel_uf(
    const uint32_t* __restrict__ d_bitmap,
    const uint16_t* __restrict__ d_row_prefix,
    const uint32_t* __restrict__ d_prime_pos,
    const uint32_t* __restrict__ d_prime_count,
    uint16_t* __restrict__ d_parent,
    int num_tiles) {
    const int tile_idx = static_cast<int>(blockIdx.x);
    if (tile_idx >= num_tiles) return;

    const int tid = static_cast<int>(threadIdx.x);

    // Per-tile pointers in global memory
    const uint32_t* tile_bitmap = d_bitmap + static_cast<size_t>(tile_idx) * BITMAP_WORDS;
    const uint16_t* tile_row_prefix = d_row_prefix + static_cast<size_t>(tile_idx) * (ACTIVE_ROWS + 1);
    const uint32_t* tile_prime_pos = d_prime_pos + static_cast<size_t>(tile_idx) * MAX_PRIMES_GPU;
    uint16_t* tile_parent = d_parent + static_cast<size_t>(tile_idx) * MAX_PRIMES_GPU;

    const int prime_count = static_cast<int>(d_prime_count[tile_idx]);
    if (prime_count > MAX_PRIMES_GPU) {
        // Overflow tile — skip
        return;
    }
    const int bounded = prime_count < MAX_PRIMES_GPU ? prime_count : MAX_PRIMES_GPU;

    // Initialize parent array: each element is its own root
    for (int i = tid; i < bounded; i += BLOCK_THREADS) {
        tile_parent[i] = static_cast<uint16_t>(i);
    }
    __syncthreads();

    // Union pass: all 288 threads scan primes in stride
    for (int i = tid; i < bounded; i += BLOCK_THREADS) {
        const uint32_t packed = tile_prime_pos[i];
        const int row = static_cast<int>(packed / SIDE_EXP);
        const int col = static_cast<int>(packed % SIDE_EXP);

        for (int k = 0; k < NUM_BACKWARD_OFFSETS; ++k) {
            const int nr = row + static_cast<int>(c_bk_dr[k]);
            const int nc = col + static_cast<int>(c_bk_dc[k]);
            if (nr < 0 || nr >= SIDE_EXP || nc < 0 || nc >= SIDE_EXP) {
                continue;
            }
            if (!gpu_bitmap_test_uf(tile_bitmap, nr, nc)) {
                continue;
            }

            const int j = gpu_uf_index(nr, nc, tile_bitmap, tile_row_prefix);
            if (j >= 0 && j < bounded) {
                atomic_union(tile_parent, static_cast<uint16_t>(i), static_cast<uint16_t>(j));
            }
        }
    }
    __syncthreads();

    // Final path compression pass
    for (int i = tid; i < bounded; i += BLOCK_THREADS) {
        tile_parent[i] = atomic_find_root(tile_parent, static_cast<uint16_t>(i));
    }
}
