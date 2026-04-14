// K2 MR — FJ64_262k Miller-Rabin primality test.
// 288 threads/block, NO register cap (let nvcc use 48-64 for max ILP).
// 2 MR rounds total: base-2 first, then hash(n)->table->1 witness.
// FJ64 table is uint16_t[262144] = 512 KB in global memory, cached in L2.

#include "gpu_constants.cuh"
#include "gpu_types.cuh"
#include "gpu_math.cuh"

#include <cstdint>

// ---- Bitmap set via atomicOr in global memory ----

static __device__ __forceinline__ void gpu_bitmap_set_global(
    uint32_t* bitmap, int row, int col) {
    atomicOr(&bitmap[row * BITMAP_WORDS_PER_ROW + (col >> 5)], 1u << (col & 31));
}

// ---- K2 MR kernel ----

__global__ void kernel_mr(
    const TileCoord* __restrict__ coords,
    const uint32_t* __restrict__ d_cand_list,
    const uint32_t* __restrict__ d_total_cands,
    uint32_t* __restrict__ d_bitmap,
    const uint16_t* __restrict__ d_fj64_table,
    int num_tiles) {
    const int tile_idx = static_cast<int>(blockIdx.x);
    if (tile_idx >= num_tiles) return;

    const int tid = static_cast<int>(threadIdx.x);
    const TileCoord coord = coords[tile_idx];
    const int32_t a_start = static_cast<int32_t>(coord.a_lo - static_cast<int64_t>(COLLAR));
    const int32_t b_start = static_cast<int32_t>(coord.b_lo - static_cast<int64_t>(COLLAR));

    // Per-tile buffers in global memory
    const uint32_t* tile_cand_list = d_cand_list + static_cast<size_t>(tile_idx) * MAX_CANDIDATES_GPU;
    uint32_t* tile_bitmap = d_bitmap + static_cast<size_t>(tile_idx) * BITMAP_WORDS;

    // Zero bitmap cooperatively
    for (int i = tid; i < BITMAP_WORDS; i += BLOCK_THREADS) {
        tile_bitmap[i] = 0U;
    }
    __syncthreads();

    const int total_cands = static_cast<int>(d_total_cands[tile_idx]);

    // MR test each candidate
    for (int i = tid; i < total_cands; i += BLOCK_THREADS) {
        const uint32_t packed = tile_cand_list[i];
        const int cand_row = static_cast<int>(packed >> 16);
        const int cand_col = static_cast<int>(packed & 0xFFFFu);
        const int32_t ca = a_start + cand_row;
        const int32_t cb = b_start + cand_col;

        if (ca == 0 || cb == 0) {
            if (is_axis_gaussian_prime_gpu(ca, cb)) {
                gpu_bitmap_set_global(tile_bitmap, cand_row, cand_col);
            }
            continue;
        }

        const uint64_t ua = static_cast<uint64_t>(ca < 0 ? -static_cast<int64_t>(ca) : ca);
        const uint64_t ub = static_cast<uint64_t>(cb < 0 ? -static_cast<int64_t>(cb) : cb);
        const uint64_t norm = ua * ua + ub * ub;

        if (is_prime_norm_fj64_262k_gpu(norm, d_fj64_table)) {
            gpu_bitmap_set_global(tile_bitmap, cand_row, cand_col);
        }
    }
}
