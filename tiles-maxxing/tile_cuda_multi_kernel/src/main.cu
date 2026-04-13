// Multi-kernel tile processing pipeline: K1 Sieve -> K2 MR -> K3 Compact -> K4 UF -> K5 FaceEncode
// Host orchestration: allocate inter-kernel buffers, upload FJ64 table, launch 5 kernels in sequence.

#include "gpu_constants.cuh"
#include "gpu_types.cuh"
#include "fj64_262k_table.h"

#include <cuda_runtime.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <unistd.h>
#include <vector>

// ---- Kernel declarations ----
__global__ void kernel_sieve(
    const TileCoord* __restrict__ coords,
    uint32_t* __restrict__ d_cand_list,
    uint32_t* __restrict__ d_total_cands,
    int num_tiles);

__global__ void kernel_mr(
    const TileCoord* __restrict__ coords,
    const uint32_t* __restrict__ d_cand_list,
    const uint32_t* __restrict__ d_total_cands,
    uint32_t* __restrict__ d_bitmap,
    const uint16_t* __restrict__ d_fj64_table,
    int num_tiles);

__global__ void kernel_compact(
    const uint32_t* __restrict__ d_bitmap,
    uint16_t* __restrict__ d_row_prefix,
    uint32_t* __restrict__ d_prime_pos,
    uint32_t* __restrict__ d_prime_count,
    int num_tiles);

__global__ void kernel_uf(
    const uint32_t* __restrict__ d_bitmap,
    const uint16_t* __restrict__ d_row_prefix,
    const uint32_t* __restrict__ d_prime_pos,
    const uint32_t* __restrict__ d_prime_count,
    uint16_t* __restrict__ d_parent,
    int num_tiles);

__global__ void kernel_face_encode(
    const uint32_t* __restrict__ d_prime_pos,
    const uint32_t* __restrict__ d_prime_count,
    const uint16_t* __restrict__ d_parent,
    TileOp* __restrict__ d_output,
    uint32_t* __restrict__ d_prime_counts_out,
    int num_tiles);

// External functions from kernel_sieve.cu
void upload_sieve_tables(const SieveTablesBarrett& tables);
void upload_backward_offsets(const int8_t* bk_dr, const int8_t* bk_dc, int count);
void upload_constants();

// External functions from kernel_compact.cu / kernel_face_encode.cu
size_t kernel_compact_shared_bytes();
size_t kernel_face_encode_shared_bytes();

namespace {

constexpr uint32_t HOST_SIEVE_LIMIT = 10000U;

inline void check_cuda(cudaError_t status, const char* what) {
    if (status != cudaSuccess) {
        std::fprintf(stderr, "%s failed: %s\n", what, cudaGetErrorString(status));
        std::abort();
    }
}

uint64_t mulmod_small(uint64_t a, uint64_t b, uint64_t m) {
    return (a * b) % m;
}

uint64_t fast_sqrt_neg1(uint64_t p) {
    for (uint64_t x = 1; x < p; ++x) {
        if (mulmod_small(x, x, p) == (p - 1ULL)) return x;
    }
    return std::numeric_limits<uint64_t>::max();
}

uint32_t barrett_host_mod(uint32_t x, uint32_t p, uint32_t mu) {
    uint32_t q = static_cast<uint32_t>((static_cast<uint64_t>(x) * mu) >> 32);
    uint32_t r = x - q * p;
    if (r >= p) r -= p;
    return r;
}

bool init_sieve_tables_host(SieveTablesBarrett& tables) {
    std::memset(&tables, 0, sizeof(tables));

    uint8_t is_prime_table[HOST_SIEVE_LIMIT + 1];
    std::memset(is_prime_table, 1, sizeof(is_prime_table));
    is_prime_table[0] = 0U;
    is_prime_table[1] = 0U;

    for (uint32_t p = 2U; p * p <= HOST_SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) continue;
        for (uint32_t multiple = p * p; multiple <= HOST_SIEVE_LIMIT; multiple += p) {
            is_prime_table[multiple] = 0U;
        }
    }

    for (uint32_t p = 2U; p <= HOST_SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) continue;

        const uint32_t mu = static_cast<uint32_t>((1ULL << 32) / static_cast<uint64_t>(p));

        if ((p & 3U) == 1U) {
            const uint64_t root_raw = fast_sqrt_neg1(static_cast<uint64_t>(p));
            if (root_raw == std::numeric_limits<uint64_t>::max()) return false;

            uint64_t root = root_raw;
            const uint64_t neg_root = static_cast<uint64_t>(p) - root;
            if (neg_root < root) root = neg_root;

            if (mulmod_small(root, root, static_cast<uint64_t>(p)) != static_cast<uint64_t>(p - 1U))
                return false;
            if (tables.split_count >= SPLIT_PRIMES_COUNT) return false;

            const uint32_t root32 = static_cast<uint32_t>(root);
            if (barrett_host_mod(root32 * root32, p, mu) != p - 1U) {
                std::fprintf(stderr, "Barrett validation failed for split prime %u\n", p);
                return false;
            }

            tables.split_table[tables.split_count++] = SplitPrimeBarrettGPU{
                static_cast<uint16_t>(p), static_cast<uint16_t>(root), mu};
        } else if ((p & 3U) == 3U) {
            if (tables.inert_count >= INERT_PRIMES_COUNT) return false;
            tables.inert_primes[tables.inert_count++] = InertPrimeBarrettGPU{
                static_cast<uint16_t>(p), 0, mu};
        }
    }

    return tables.split_count == SPLIT_PRIMES_COUNT &&
           tables.inert_count == INERT_PRIMES_COUNT;
}

struct BackwardOffsetsHost {
    int8_t dr[NUM_BACKWARD_OFFSETS];
    int8_t dc[NUM_BACKWARD_OFFSETS];
    int count;
};

void init_backward_offsets_host(BackwardOffsetsHost& offsets) {
    int count = 0;
    for (int dr = -COLLAR; dr <= 0; ++dr) {
        for (int dc = -COLLAR; dc <= COLLAR; ++dc) {
            if ((dr > 0) || (dr == 0 && dc >= 0)) continue;
            if ((dr * dr + dc * dc) > K_SQ) continue;
            if (count >= NUM_BACKWARD_OFFSETS) {
                std::fprintf(stderr, "backward offset overflow\n");
                std::abort();
            }
            offsets.dr[count] = static_cast<int8_t>(dr);
            offsets.dc[count] = static_cast<int8_t>(dc);
            ++count;
        }
    }
    offsets.count = count;
    if (offsets.count != NUM_BACKWARD_OFFSETS) {
        std::fprintf(stderr, "backward offset count mismatch: expected %d got %d\n",
                     NUM_BACKWARD_OFFSETS, offsets.count);
        std::abort();
    }
}

std::vector<TileCoord> default_tiles() {
    return {
        TileCoord{600000000LL, 600000000LL},
        TileCoord{699999744LL, 400000000LL},
    };
}

std::vector<TileCoord> generate_bench_tiles(int n) {
    std::vector<TileCoord> coords;
    coords.reserve(n);
    constexpr int64_t A_ORIGIN = 608000000LL;
    constexpr int64_t B_ORIGIN = 608000000LL;
    const int grid_w = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    for (int i = 0; i < n; ++i) {
        const int col = i % grid_w;
        const int row = i / grid_w;
        coords.push_back(TileCoord{
            A_ORIGIN + static_cast<int64_t>(row) * TILE_SIDE,
            B_ORIGIN + static_cast<int64_t>(col) * TILE_SIDE,
        });
    }
    return coords;
}

void print_device_info() {
    int device = 0;
    check_cuda(cudaGetDevice(&device), "cudaGetDevice");
    cudaDeviceProp prop{};
    check_cuda(cudaGetDeviceProperties(&prop, device), "cudaGetDeviceProperties");
    std::printf("device: %s\n", prop.name);
    std::printf("  SMs: %d\n", prop.multiProcessorCount);
    std::printf("  max threads/SM: %d\n", prop.maxThreadsPerMultiProcessor);
    std::printf("  max shared mem/SM: %zu bytes\n", prop.sharedMemPerMultiprocessor);
    std::printf("  max shared mem/block: %zu bytes\n", prop.sharedMemPerBlock);
    std::printf("  max shared mem/block (optin): %zu bytes\n", prop.sharedMemPerBlockOptin);
    std::printf("  clock rate: %d MHz\n", prop.clockRate / 1000);
    std::printf("  memory clock: %d MHz\n", prop.memoryClockRate / 1000);
}

void print_kernel_occupancy_generic(const char* name, const void* func, size_t shared_bytes) {
    int num_blocks = 0;
    cudaError_t occ_err = cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &num_blocks, func, BLOCK_THREADS, shared_bytes);
    if (occ_err == cudaSuccess) {
        std::printf("  %-20s blocks/SM=%d  occupancy=%.1f%%  shared=%zu\n",
                    name, num_blocks,
                    100.0 * static_cast<double>(num_blocks * BLOCK_THREADS) / 1536.0,
                    shared_bytes);
    } else {
        std::printf("  %-20s occupancy query failed: %s\n", name, cudaGetErrorString(occ_err));
    }

    cudaFuncAttributes attr{};
    cudaError_t attr_err = cudaFuncGetAttributes(&attr, func);
    if (attr_err == cudaSuccess) {
        std::printf("  %-20s regs/thread=%d  static_smem=%zu\n",
                    name, attr.numRegs, attr.sharedSizeBytes);
    }
}

struct TileBatchDeviceMemory {
    TileBatchBuffers buf;
    uint16_t* d_fj64_table;
    int num_tiles;

    void allocate(int n) {
        num_tiles = n;
        const size_t N = static_cast<size_t>(n);

        check_cuda(cudaMalloc(&buf.d_coords, sizeof(TileCoord) * N), "alloc d_coords");
        check_cuda(cudaMalloc(&buf.d_cand_list, sizeof(uint32_t) * N * MAX_CANDIDATES_GPU), "alloc d_cand_list");
        check_cuda(cudaMalloc(&buf.d_total_cands, sizeof(uint32_t) * N), "alloc d_total_cands");
        check_cuda(cudaMalloc(&buf.d_bitmap, sizeof(uint32_t) * N * BITMAP_WORDS), "alloc d_bitmap");
        check_cuda(cudaMalloc(&buf.d_row_prefix, sizeof(uint16_t) * N * (ACTIVE_ROWS + 1)), "alloc d_row_prefix");
        check_cuda(cudaMalloc(&buf.d_prime_pos, sizeof(uint32_t) * N * MAX_PRIMES_GPU), "alloc d_prime_pos");
        check_cuda(cudaMalloc(&buf.d_prime_count, sizeof(uint32_t) * N), "alloc d_prime_count");
        check_cuda(cudaMalloc(&buf.d_parent, sizeof(uint16_t) * N * MAX_PRIMES_GPU), "alloc d_parent");
        check_cuda(cudaMalloc(&buf.d_output, sizeof(TileOp) * N), "alloc d_output");
        check_cuda(cudaMalloc(&buf.d_prime_counts_out, sizeof(uint32_t) * N), "alloc d_prime_counts_out");

        // Upload FJ64_262K table to device
        check_cuda(cudaMalloc(&d_fj64_table, sizeof(uint16_t) * 262144), "alloc d_fj64_table");
        check_cuda(cudaMemcpy(d_fj64_table, FJ64_262K_TABLE, sizeof(uint16_t) * 262144,
                              cudaMemcpyHostToDevice), "upload fj64_table");
    }

    void free() {
        cudaFree(buf.d_coords);
        cudaFree(buf.d_cand_list);
        cudaFree(buf.d_total_cands);
        cudaFree(buf.d_bitmap);
        cudaFree(buf.d_row_prefix);
        cudaFree(buf.d_prime_pos);
        cudaFree(buf.d_prime_count);
        cudaFree(buf.d_parent);
        cudaFree(buf.d_output);
        cudaFree(buf.d_prime_counts_out);
        cudaFree(d_fj64_table);
    }
};

struct KernelTimings {
    float sieve_ms;
    float mr_ms;
    float compact_ms;
    float uf_ms;
    float face_encode_ms;
    float total_ms;
};

void launch_pipeline(TileBatchDeviceMemory& mem, int num_tiles,
                     KernelTimings* timings = nullptr) {
    const dim3 grid(static_cast<unsigned int>(num_tiles));
    const dim3 block(BLOCK_THREADS);
    const size_t compact_smem = kernel_compact_shared_bytes();
    const size_t face_smem = kernel_face_encode_shared_bytes();

    // Set max dynamic shared memory for face encode kernel
    check_cuda(cudaFuncSetAttribute(kernel_face_encode,
                                    cudaFuncAttributeMaxDynamicSharedMemorySize,
                                    static_cast<int>(face_smem)),
               "setattr face_encode smem");

    if (timings) {
        cudaEvent_t e[6];
        for (int i = 0; i < 6; ++i) {
            check_cuda(cudaEventCreate(&e[i]), "event create");
        }

        check_cuda(cudaEventRecord(e[0]), "record e0");
        kernel_sieve<<<grid, block>>>(
            mem.buf.d_coords, mem.buf.d_cand_list, mem.buf.d_total_cands, num_tiles);
        check_cuda(cudaEventRecord(e[1]), "record e1");

        kernel_mr<<<grid, block>>>(
            mem.buf.d_coords, mem.buf.d_cand_list, mem.buf.d_total_cands,
            mem.buf.d_bitmap, mem.d_fj64_table, num_tiles);
        check_cuda(cudaEventRecord(e[2]), "record e2");

        kernel_compact<<<grid, block, compact_smem>>>(
            mem.buf.d_bitmap, mem.buf.d_row_prefix, mem.buf.d_prime_pos,
            mem.buf.d_prime_count, num_tiles);
        check_cuda(cudaEventRecord(e[3]), "record e3");

        kernel_uf<<<grid, block>>>(
            mem.buf.d_bitmap, mem.buf.d_row_prefix, mem.buf.d_prime_pos,
            mem.buf.d_prime_count, mem.buf.d_parent, num_tiles);
        check_cuda(cudaEventRecord(e[4]), "record e4");

        kernel_face_encode<<<grid, block, face_smem>>>(
            mem.buf.d_prime_pos, mem.buf.d_prime_count, mem.buf.d_parent,
            mem.buf.d_output, mem.buf.d_prime_counts_out, num_tiles);
        check_cuda(cudaEventRecord(e[5]), "record e5");

        check_cuda(cudaGetLastError(), "pipeline launch");
        check_cuda(cudaEventSynchronize(e[5]), "sync pipeline");

        cudaEventElapsedTime(&timings->sieve_ms, e[0], e[1]);
        cudaEventElapsedTime(&timings->mr_ms, e[1], e[2]);
        cudaEventElapsedTime(&timings->compact_ms, e[2], e[3]);
        cudaEventElapsedTime(&timings->uf_ms, e[3], e[4]);
        cudaEventElapsedTime(&timings->face_encode_ms, e[4], e[5]);
        cudaEventElapsedTime(&timings->total_ms, e[0], e[5]);

        for (int i = 0; i < 6; ++i) cudaEventDestroy(e[i]);
    } else {
        kernel_sieve<<<grid, block>>>(
            mem.buf.d_coords, mem.buf.d_cand_list, mem.buf.d_total_cands, num_tiles);
        kernel_mr<<<grid, block>>>(
            mem.buf.d_coords, mem.buf.d_cand_list, mem.buf.d_total_cands,
            mem.buf.d_bitmap, mem.d_fj64_table, num_tiles);
        kernel_compact<<<grid, block, compact_smem>>>(
            mem.buf.d_bitmap, mem.buf.d_row_prefix, mem.buf.d_prime_pos,
            mem.buf.d_prime_count, num_tiles);
        kernel_uf<<<grid, block>>>(
            mem.buf.d_bitmap, mem.buf.d_row_prefix, mem.buf.d_prime_pos,
            mem.buf.d_prime_count, mem.buf.d_parent, num_tiles);
        kernel_face_encode<<<grid, block, face_smem>>>(
            mem.buf.d_prime_pos, mem.buf.d_prime_count, mem.buf.d_parent,
            mem.buf.d_output, mem.buf.d_prime_counts_out, num_tiles);
        check_cuda(cudaGetLastError(), "pipeline launch");
        check_cuda(cudaDeviceSynchronize(), "pipeline sync");
    }
}

enum class Mode { TEST, BENCH };

struct Args {
    Mode mode;
    int tile_count;
};

Args parse_args(int argc, char** argv) {
    if (argc < 2) return Args{Mode::TEST, 2};
    if (std::strcmp(argv[1], "test") == 0) return Args{Mode::TEST, 2};
    const int n = std::atoi(argv[1]);
    if (n <= 0) {
        std::fprintf(stderr, "usage: %s [test | <tile_count>]\n", argv[0]);
        std::fprintf(stderr, "  test          - 2-tile smoke test (default)\n");
        std::fprintf(stderr, "  <tile_count>  - benchmark N tiles\n");
        std::exit(1);
    }
    return Args{Mode::BENCH, n};
}

// Dump mode: process tiles from a binary coords file and write results to output file.
// Input format (little-endian):
//   uint32_t num_tiles
//   Repeated num_tiles times: int64_t a_lo, int64_t b_lo
// Output format (little-endian):
//   uint32_t num_tiles
//   Repeated num_tiles times: int64_t a_lo, int64_t b_lo, uint32_t prime_count, uint8_t tileop[128]
int run_dump(const char* coords_path, const char* output_path) {
    constexpr int CHUNK_SIZE = 20000;

    // --- Read coords file ---
    FILE* fin = std::fopen(coords_path, "rb");
    if (!fin) {
        std::fprintf(stderr, "dump: cannot open coords file: %s\n", coords_path);
        return 1;
    }

    uint32_t num_tiles = 0;
    if (std::fread(&num_tiles, sizeof(uint32_t), 1, fin) != 1) {
        std::fprintf(stderr, "dump: failed to read num_tiles from %s\n", coords_path);
        std::fclose(fin);
        return 1;
    }
    std::printf("dump: reading %u tiles from %s\n", num_tiles, coords_path);

    std::vector<TileCoord> all_coords(num_tiles);
    for (uint32_t i = 0; i < num_tiles; ++i) {
        int64_t a_lo = 0, b_lo = 0;
        if (std::fread(&a_lo, sizeof(int64_t), 1, fin) != 1 ||
            std::fread(&b_lo, sizeof(int64_t), 1, fin) != 1) {
            std::fprintf(stderr, "dump: truncated coords file at tile %u\n", i);
            std::fclose(fin);
            return 1;
        }
        all_coords[i] = TileCoord{a_lo, b_lo};
    }
    std::fclose(fin);

    // --- Open output file ---
    FILE* fout = std::fopen(output_path, "wb");
    if (!fout) {
        std::fprintf(stderr, "dump: cannot open output file: %s\n", output_path);
        return 1;
    }

    // Write header: total tile count
    if (std::fwrite(&num_tiles, sizeof(uint32_t), 1, fout) != 1) {
        std::fprintf(stderr, "dump: failed to write output header\n");
        std::fclose(fout);
        return 1;
    }

    // --- Wall-clock timer ---
    const auto wall_start = std::chrono::steady_clock::now();

    const int total = static_cast<int>(num_tiles);
    const int num_chunks = (total + CHUNK_SIZE - 1) / CHUNK_SIZE;
    int tiles_done = 0;

    // Allocate GPU buffers once for max chunk size — reuse across all chunks
    TileBatchDeviceMemory mem{};
    mem.allocate(CHUNK_SIZE);

    // Host-side result buffers — allocated once at max chunk size
    std::vector<TileOp> tileops(CHUNK_SIZE);
    std::vector<uint32_t> prime_counts(CHUNK_SIZE, 0U);

    for (int chunk_idx = 0; chunk_idx < num_chunks; ++chunk_idx) {
        const int chunk_start = chunk_idx * CHUNK_SIZE;
        const int chunk_tiles = std::min(CHUNK_SIZE, total - chunk_start);

        // Upload coords for this chunk (only chunk_tiles worth)
        check_cuda(cudaMemcpy(mem.buf.d_coords, all_coords.data() + chunk_start,
                              sizeof(TileCoord) * static_cast<size_t>(chunk_tiles),
                              cudaMemcpyHostToDevice), "dump: upload coords");

        // Launch full pipeline (no timing overhead needed for dump)
        launch_pipeline(mem, chunk_tiles, nullptr);

        // Copy results back (only chunk_tiles worth)
        check_cuda(cudaMemcpy(tileops.data(), mem.buf.d_output,
                              sizeof(TileOp) * static_cast<size_t>(chunk_tiles),
                              cudaMemcpyDeviceToHost), "dump: dl tileops");
        check_cuda(cudaMemcpy(prime_counts.data(), mem.buf.d_prime_counts_out,
                              sizeof(uint32_t) * static_cast<size_t>(chunk_tiles),
                              cudaMemcpyDeviceToHost), "dump: dl prime_counts");

        // Write chunk results to output file
        for (int i = 0; i < chunk_tiles; ++i) {
            const TileCoord& tc = all_coords[static_cast<size_t>(chunk_start + i)];
            const uint32_t pc = prime_counts[static_cast<size_t>(i)];
            if (std::fwrite(&tc.a_lo, sizeof(int64_t), 1, fout) != 1 ||
                std::fwrite(&tc.b_lo, sizeof(int64_t), 1, fout) != 1 ||
                std::fwrite(&pc, sizeof(uint32_t), 1, fout) != 1 ||
                std::fwrite(tileops[static_cast<size_t>(i)].bytes, 1, sizeof(TileOp), fout) != sizeof(TileOp)) {
                std::fprintf(stderr, "dump: write error at tile %d\n", chunk_start + i);
                mem.free();
                std::fclose(fout);
                return 1;
            }
        }

        tiles_done += chunk_tiles;
        std::printf("Chunk %d/%d: %d tiles processed\n", chunk_idx + 1, num_chunks, chunk_tiles);
    }

    // Free GPU buffers after all chunks are done
    mem.free();

    std::fclose(fout);

    const auto wall_end = std::chrono::steady_clock::now();
    const double wall_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(wall_end - wall_start).count()) / 1000.0;

    std::printf("dump: %d tiles -> %s  (%.1f ms total, %.3f ms/tile)\n",
                tiles_done, output_path, wall_ms,
                wall_ms / static_cast<double>(tiles_done > 0 ? tiles_done : 1));
    return 0;
}

// Campaign mode: process tiles from burst_index + coords files, write raw TileOp bytes.
// burst_index.bin format (little-endian):
//   uint32_t  num_towers
//   uint32_t  total_tiles
//   uint32_t  tiles_per_tower[num_towers]
// coords.bin format (little-endian):
//   uint32_t  num_tiles
//   TileCoord coords[num_tiles]    (int64_t a_lo, int64_t b_lo each)
// Output: raw TileOp bytes only — total_tiles * 128 bytes, tower-major order.
int run_campaign(const char* burst_index_path, const char* coords_path, const char* output_path) {
    constexpr int CHUNK_SIZE = 20000;

    // --- Read burst_index file ---
    FILE* fburst = std::fopen(burst_index_path, "rb");
    if (!fburst) {
        std::fprintf(stderr, "campaign: cannot open burst index file: %s\n", burst_index_path);
        return 1;
    }

    uint32_t num_towers = 0;
    uint32_t total_tiles = 0;
    if (std::fread(&num_towers, sizeof(uint32_t), 1, fburst) != 1 ||
        std::fread(&total_tiles, sizeof(uint32_t), 1, fburst) != 1) {
        std::fprintf(stderr, "campaign: failed to read burst index header from %s\n", burst_index_path);
        std::fclose(fburst);
        return 1;
    }

    std::vector<uint32_t> tiles_per_tower(num_towers);
    if (num_towers > 0 &&
        std::fread(tiles_per_tower.data(), sizeof(uint32_t), num_towers, fburst) != num_towers) {
        std::fprintf(stderr, "campaign: truncated tiles_per_tower in %s\n", burst_index_path);
        std::fclose(fburst);
        return 1;
    }
    std::fclose(fburst);

    // Validate sum(tiles_per_tower) == total_tiles
    uint64_t sum_tpt = 0;
    for (uint32_t i = 0; i < num_towers; ++i) {
        sum_tpt += tiles_per_tower[i];
    }
    if (sum_tpt != static_cast<uint64_t>(total_tiles)) {
        std::fprintf(stderr, "campaign: tiles_per_tower sum (%" PRIu64 ") != total_tiles (%u)\n",
                     sum_tpt, total_tiles);
        return 1;
    }

    // --- Read coords file ---
    FILE* fin = std::fopen(coords_path, "rb");
    if (!fin) {
        std::fprintf(stderr, "campaign: cannot open coords file: %s\n", coords_path);
        return 1;
    }

    uint32_t num_tiles_coord = 0;
    if (std::fread(&num_tiles_coord, sizeof(uint32_t), 1, fin) != 1) {
        std::fprintf(stderr, "campaign: failed to read num_tiles from %s\n", coords_path);
        std::fclose(fin);
        return 1;
    }

    // Validate coords.num_tiles == burst_index.total_tiles
    if (num_tiles_coord != total_tiles) {
        std::fprintf(stderr, "campaign: coords num_tiles (%u) != burst_index total_tiles (%u)\n",
                     num_tiles_coord, total_tiles);
        std::fclose(fin);
        return 1;
    }

    std::vector<TileCoord> all_coords(total_tiles);
    for (uint32_t i = 0; i < total_tiles; ++i) {
        int64_t a_lo = 0, b_lo = 0;
        if (std::fread(&a_lo, sizeof(int64_t), 1, fin) != 1 ||
            std::fread(&b_lo, sizeof(int64_t), 1, fin) != 1) {
            std::fprintf(stderr, "campaign: truncated coords file at tile %u\n", i);
            std::fclose(fin);
            return 1;
        }
        all_coords[i] = TileCoord{a_lo, b_lo};
    }
    std::fclose(fin);

    // --- Open output file ---
    FILE* fout = std::fopen(output_path, "wb");
    if (!fout) {
        std::fprintf(stderr, "campaign: cannot open output file: %s\n", output_path);
        return 1;
    }

    // --- Wall-clock timer ---
    const auto wall_start = std::chrono::steady_clock::now();

    const int total = static_cast<int>(total_tiles);
    const int num_chunks = (total + CHUNK_SIZE - 1) / CHUNK_SIZE;
    int tiles_done = 0;

    // Allocate GPU buffers once for max chunk size — reuse across all chunks
    TileBatchDeviceMemory mem{};
    mem.allocate(CHUNK_SIZE);

    // Host-side result buffers — allocated once at max chunk size
    std::vector<TileOp> tileops(CHUNK_SIZE);

    for (int chunk_idx = 0; chunk_idx < num_chunks; ++chunk_idx) {
        const int chunk_start = chunk_idx * CHUNK_SIZE;
        const int chunk_tiles = std::min(CHUNK_SIZE, total - chunk_start);

        // Upload coords for this chunk
        check_cuda(cudaMemcpy(mem.buf.d_coords, all_coords.data() + chunk_start,
                              sizeof(TileCoord) * static_cast<size_t>(chunk_tiles),
                              cudaMemcpyHostToDevice), "campaign: upload coords");

        // Launch full pipeline
        launch_pipeline(mem, chunk_tiles, nullptr);

        // Copy TileOp results back (no prime counts needed)
        check_cuda(cudaMemcpy(tileops.data(), mem.buf.d_output,
                              sizeof(TileOp) * static_cast<size_t>(chunk_tiles),
                              cudaMemcpyDeviceToHost), "campaign: dl tileops");

        // Write raw TileOp bytes only — no header, no metadata
        for (int i = 0; i < chunk_tiles; ++i) {
            if (std::fwrite(&tileops[i], TILEOP_SIZE, 1, fout) != 1) {
                std::fprintf(stderr, "campaign: write error at tile %d\n", chunk_start + i);
                mem.free();
                std::fclose(fout);
                return 1;
            }
        }

        tiles_done += chunk_tiles;
    }

    mem.free();
    std::fclose(fout);

    const auto wall_end = std::chrono::steady_clock::now();
    const double wall_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(wall_end - wall_start).count()) / 1000.0;

    // Summary to stderr (not stdout)
    std::fprintf(stderr, "campaign: %d tiles (%u towers) -> %s  (%.1f ms total, %.3f ms/tile)\n",
                 tiles_done, num_towers, output_path, wall_ms,
                 wall_ms / static_cast<double>(tiles_done > 0 ? tiles_done : 1));
    return 0;
}

// Stream mode: persistent subprocess, reads bursts from stdin, writes results to stdout.
// Binary protocol (little-endian):
//   Per-burst input (from campaign):
//     uint32_t num_tiles
//     uint32_t burst_idx_bytes
//     uint32_t coords_bytes
//     [burst_idx data: uint32_t num_towers, uint32_t total_tiles, uint32_t tpt[N]]
//     [coords data: uint32_t num_tiles, TileCoord[N]]
//   Per-burst output (to campaign):
//     uint32_t num_tiles
//     uint32_t output_bytes
//     [raw TileOp data: num_tiles * 128 bytes]
//   Termination: stdin EOF → clean exit.

bool stream_read_all(void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::read(STDIN_FILENO, p, remaining);
        if (n == 0) return false;  // EOF
        if (n < 0) {
            std::fprintf(stderr, "stream: read error: %s\n", std::strerror(errno));
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool stream_write_all(const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::write(STDOUT_FILENO, p, remaining);
        if (n <= 0) {
            std::fprintf(stderr, "stream: write error: %s\n",
                         n == 0 ? "EOF" : std::strerror(errno));
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

int run_stream() {
    constexpr int MAX_STREAM_TILES = 400000;  // ~50KB/tile * 400K = 19.1GB — fits 24GB GPU

    // Allocate GPU buffers once at max capacity — reuse across all bursts
    TileBatchDeviceMemory mem{};
    mem.allocate(MAX_STREAM_TILES);

    // Host-side result buffer — allocated once at max capacity
    std::vector<TileOp> tileops(MAX_STREAM_TILES);

    // Host-side coord read buffer
    std::vector<TileCoord> all_coords(MAX_STREAM_TILES);

    uint64_t total_tiles_processed = 0;
    int burst_count = 0;
    const auto session_start = std::chrono::steady_clock::now();

    std::fprintf(stderr, "stream: ready, max_tiles_per_burst=%d\n", MAX_STREAM_TILES);

    for (;;) {
        // Read burst header: [num_tiles, burst_idx_bytes, coords_bytes]
        uint32_t header[3];
        if (!stream_read_all(header, sizeof(header))) {
            // EOF or error — clean exit
            break;
        }

        const uint32_t num_tiles = header[0];
        const uint32_t burst_idx_bytes = header[1];
        const uint32_t coords_bytes = header[2];

        if (num_tiles == 0 || num_tiles > static_cast<uint32_t>(MAX_STREAM_TILES)) {
            std::fprintf(stderr, "stream: invalid num_tiles=%u (max=%d)\n",
                         num_tiles, MAX_STREAM_TILES);
            mem.free();
            return 1;
        }

        // Read and discard burst_index data (we only need coords for GPU processing;
        // the burst_index structure is used by the campaign side for compositor ingestion)
        {
            std::vector<uint8_t> burst_idx_buf(burst_idx_bytes);
            if (!stream_read_all(burst_idx_buf.data(), burst_idx_bytes)) {
                std::fprintf(stderr, "stream: failed to read burst_idx data\n");
                mem.free();
                return 1;
            }
            // Validate: first 8 bytes are num_towers + total_tiles
            if (burst_idx_bytes >= 2 * sizeof(uint32_t)) {
                uint32_t total_tiles_check;
                std::memcpy(&total_tiles_check, burst_idx_buf.data() + sizeof(uint32_t),
                            sizeof(uint32_t));
                if (total_tiles_check != num_tiles) {
                    std::fprintf(stderr, "stream: burst_index total_tiles=%u != header num_tiles=%u\n",
                                 total_tiles_check, num_tiles);
                    mem.free();
                    return 1;
                }
            }
        }

        // Read coords data
        {
            const uint32_t expected_coords_bytes = static_cast<uint32_t>(
                sizeof(uint32_t) + static_cast<size_t>(num_tiles) * sizeof(TileCoord));
            if (coords_bytes != expected_coords_bytes) {
                std::fprintf(stderr, "stream: coords_bytes=%u != expected=%u\n",
                             coords_bytes, expected_coords_bytes);
                mem.free();
                return 1;
            }

            // Read the num_tiles prefix from coords
            uint32_t coords_num_tiles;
            if (!stream_read_all(&coords_num_tiles, sizeof(uint32_t))) {
                std::fprintf(stderr, "stream: failed to read coords header\n");
                mem.free();
                return 1;
            }
            if (coords_num_tiles != num_tiles) {
                std::fprintf(stderr, "stream: coords num_tiles=%u != header num_tiles=%u\n",
                             coords_num_tiles, num_tiles);
                mem.free();
                return 1;
            }

            // Read TileCoord array
            if (!stream_read_all(all_coords.data(),
                                 static_cast<size_t>(num_tiles) * sizeof(TileCoord))) {
                std::fprintf(stderr, "stream: failed to read coords data\n");
                mem.free();
                return 1;
            }
        }

        // Process on GPU — same chunking as campaign mode for large bursts
        const int total = static_cast<int>(num_tiles);
        constexpr int CHUNK_SIZE = 20000;
        const int num_chunks = (total + CHUNK_SIZE - 1) / CHUNK_SIZE;

        for (int chunk_idx = 0; chunk_idx < num_chunks; ++chunk_idx) {
            const int chunk_start = chunk_idx * CHUNK_SIZE;
            const int chunk_tiles = std::min(CHUNK_SIZE, total - chunk_start);

            check_cuda(cudaMemcpy(mem.buf.d_coords,
                                  all_coords.data() + chunk_start,
                                  sizeof(TileCoord) * static_cast<size_t>(chunk_tiles),
                                  cudaMemcpyHostToDevice), "stream: upload coords");

            launch_pipeline(mem, chunk_tiles, nullptr);

            check_cuda(cudaMemcpy(tileops.data() + chunk_start,
                                  mem.buf.d_output,
                                  sizeof(TileOp) * static_cast<size_t>(chunk_tiles),
                                  cudaMemcpyDeviceToHost), "stream: dl tileops");
        }

        // Write response: [num_tiles, output_bytes, raw TileOp data]
        const uint32_t output_bytes = num_tiles * TILEOP_SIZE;
        uint32_t resp_header[2] = {num_tiles, output_bytes};
        if (!stream_write_all(resp_header, sizeof(resp_header))) {
            std::fprintf(stderr, "stream: failed to write response header\n");
            mem.free();
            return 1;
        }
        if (!stream_write_all(tileops.data(),
                              static_cast<size_t>(num_tiles) * sizeof(TileOp))) {
            std::fprintf(stderr, "stream: failed to write response data\n");
            mem.free();
            return 1;
        }

        total_tiles_processed += num_tiles;
        burst_count++;
    }

    mem.free();

    const auto session_end = std::chrono::steady_clock::now();
    const double session_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(session_end - session_start).count()) / 1000.0;
    std::fprintf(stderr, "stream: %d bursts, %" PRIu64 " tiles, %.1f ms total\n",
                 burst_count, total_tiles_processed, session_ms);
    return 0;
}

void run_test() {
    std::vector<TileCoord> coords = default_tiles();
    const int num_tiles = static_cast<int>(coords.size());

    TileBatchDeviceMemory mem{};
    mem.allocate(num_tiles);

    check_cuda(cudaMemcpy(mem.buf.d_coords, coords.data(),
                          sizeof(TileCoord) * coords.size(),
                          cudaMemcpyHostToDevice), "upload coords");

    KernelTimings timings{};
    launch_pipeline(mem, num_tiles, &timings);

    std::vector<TileOp> output(num_tiles);
    std::vector<uint32_t> prime_counts(num_tiles, 0U);
    check_cuda(cudaMemcpy(output.data(), mem.buf.d_output,
                          sizeof(TileOp) * num_tiles, cudaMemcpyDeviceToHost), "dl output");
    check_cuda(cudaMemcpy(prime_counts.data(), mem.buf.d_prime_counts_out,
                          sizeof(uint32_t) * num_tiles, cudaMemcpyDeviceToHost), "dl prime_counts");

    std::printf("=== SMOKE TEST (multi-kernel) ===\n");
    std::printf("processed %d tile(s) in %.3f ms (%.3f ms/tile)\n",
                num_tiles, timings.total_ms,
                timings.total_ms / static_cast<float>(num_tiles));

    std::printf("\n--- per-kernel timing ---\n");
    std::printf("K1 Sieve:       %8.3f ms  (%.1f%%)\n", timings.sieve_ms,
                100.0 * timings.sieve_ms / timings.total_ms);
    std::printf("K2 MR:          %8.3f ms  (%.1f%%)\n", timings.mr_ms,
                100.0 * timings.mr_ms / timings.total_ms);
    std::printf("K3 Compact:     %8.3f ms  (%.1f%%)\n", timings.compact_ms,
                100.0 * timings.compact_ms / timings.total_ms);
    std::printf("K4 UF:          %8.3f ms  (%.1f%%)\n", timings.uf_ms,
                100.0 * timings.uf_ms / timings.total_ms);
    std::printf("K5 FaceEncode:  %8.3f ms  (%.1f%%)\n", timings.face_encode_ms,
                100.0 * timings.face_encode_ms / timings.total_ms);

    for (int i = 0; i < num_tiles; ++i) {
        std::printf("tile[%d] coord=(%lld,%lld) prime_count=%u tileop[0..3]=%02x %02x %02x %02x\n",
                    i,
                    static_cast<long long>(coords[i].a_lo),
                    static_cast<long long>(coords[i].b_lo),
                    prime_counts[i],
                    output[i].bytes[0],
                    output[i].bytes[1],
                    output[i].bytes[2],
                    output[i].bytes[3]);
    }

    mem.free();
}

void run_bench(int tile_count) {
    std::printf("=== BENCHMARK (multi-kernel): %d tiles ===\n", tile_count);
    print_device_info();

    std::printf("\nkernel occupancy:\n");
    print_kernel_occupancy_generic("K1_sieve", reinterpret_cast<const void*>(kernel_sieve), 0);
    print_kernel_occupancy_generic("K2_mr", reinterpret_cast<const void*>(kernel_mr), 0);
    print_kernel_occupancy_generic("K3_compact", reinterpret_cast<const void*>(kernel_compact),
                                   kernel_compact_shared_bytes());
    print_kernel_occupancy_generic("K4_uf", reinterpret_cast<const void*>(kernel_uf), 0);
    print_kernel_occupancy_generic("K5_face_encode", reinterpret_cast<const void*>(kernel_face_encode),
                                   kernel_face_encode_shared_bytes());

    std::vector<TileCoord> coords = generate_bench_tiles(tile_count);
    std::printf("\ntile grid: %d tiles, origin=(%lld,%lld)\n",
                tile_count,
                static_cast<long long>(coords[0].a_lo),
                static_cast<long long>(coords[0].b_lo));

    TileBatchDeviceMemory mem{};
    mem.allocate(tile_count);

    check_cuda(cudaMemcpy(mem.buf.d_coords, coords.data(),
                          sizeof(TileCoord) * tile_count,
                          cudaMemcpyHostToDevice), "upload coords");

    // Need to set shared memory attribute before warmup too
    const size_t face_smem = kernel_face_encode_shared_bytes();
    check_cuda(cudaFuncSetAttribute(kernel_face_encode,
                                    cudaFuncAttributeMaxDynamicSharedMemorySize,
                                    static_cast<int>(face_smem)),
               "setattr face_encode smem");

    // Warmup
    {
        const int warmup_tiles = (tile_count < 10) ? tile_count : 10;
        // Use a subset for warmup
        launch_pipeline(mem, warmup_tiles, nullptr);
        std::printf("warmup: %d tiles done\n", warmup_tiles);
    }

    // Benchmark
    KernelTimings timings{};
    launch_pipeline(mem, tile_count, &timings);

    const double tiles_per_sec = static_cast<double>(tile_count) /
                                 (static_cast<double>(timings.total_ms) / 1000.0);
    const double ms_per_tile = static_cast<double>(timings.total_ms) /
                               static_cast<double>(tile_count);

    std::printf("\n--- per-kernel timing ---\n");
    std::printf("K1 Sieve:       %8.3f ms  (%.1f%%)\n", timings.sieve_ms,
                100.0 * timings.sieve_ms / timings.total_ms);
    std::printf("K2 MR:          %8.3f ms  (%.1f%%)\n", timings.mr_ms,
                100.0 * timings.mr_ms / timings.total_ms);
    std::printf("K3 Compact:     %8.3f ms  (%.1f%%)\n", timings.compact_ms,
                100.0 * timings.compact_ms / timings.total_ms);
    std::printf("K4 UF:          %8.3f ms  (%.1f%%)\n", timings.uf_ms,
                100.0 * timings.uf_ms / timings.total_ms);
    std::printf("K5 FaceEncode:  %8.3f ms  (%.1f%%)\n", timings.face_encode_ms,
                100.0 * timings.face_encode_ms / timings.total_ms);

    std::printf("\n--- results ---\n");
    std::printf("total:      %.3f ms\n", timings.total_ms);
    std::printf("tiles:      %d\n", tile_count);
    std::printf("ms/tile:    %.4f\n", ms_per_tile);
    std::printf("tiles/sec:  %.1f\n", tiles_per_sec);

    // Spot-check
    std::vector<uint32_t> prime_counts_host(tile_count, 0U);
    check_cuda(cudaMemcpy(prime_counts_host.data(), mem.buf.d_prime_counts_out,
                          sizeof(uint32_t) * tile_count, cudaMemcpyDeviceToHost), "dl prime_counts");

    std::vector<TileOp> output_host(tile_count);
    check_cuda(cudaMemcpy(output_host.data(), mem.buf.d_output,
                          sizeof(TileOp) * tile_count, cudaMemcpyDeviceToHost), "dl output");

    int overflow_count = 0;
    for (int i = 0; i < tile_count; ++i) {
        if (output_host[i].bytes[0] == OVERFLOW_SENTINEL) ++overflow_count;
    }
    if (overflow_count > 0) {
        std::printf("WARNING: %d/%d tiles hit overflow sentinel\n", overflow_count, tile_count);
    } else {
        std::printf("all %d tiles completed without overflow\n", tile_count);
    }

    // Print first few tile results for verification
    const int show_tiles = tile_count < 8 ? tile_count : 8;
    std::printf("\nfirst %d tiles:\n", show_tiles);
    for (int i = 0; i < show_tiles; ++i) {
        std::printf("  tile[%d] coord=(%lld,%lld) primes=%u tileop[0..3]=%02x %02x %02x %02x\n",
                    i,
                    static_cast<long long>(coords[i].a_lo),
                    static_cast<long long>(coords[i].b_lo),
                    prime_counts_host[i],
                    output_host[i].bytes[0],
                    output_host[i].bytes[1],
                    output_host[i].bytes[2],
                    output_host[i].bytes[3]);
    }

    mem.free();
}

}  // namespace

int main(int argc, char** argv) {
    // Early dispatch for campaign mode
    if (argc >= 2 && std::strcmp(argv[1], "campaign") == 0) {
        if (argc != 5) {
            std::fprintf(stderr, "Usage: %s campaign <burst_index.bin> <coords.bin> <output.bin>\n", argv[0]);
            return 1;
        }
        // GPU init (sieve tables, offsets, constants)
        SieveTablesBarrett tables{};
        if (!init_sieve_tables_host(tables)) {
            std::fprintf(stderr, "failed to initialize sieve tables\n");
            return 1;
        }
        BackwardOffsetsHost offsets{};
        init_backward_offsets_host(offsets);
        upload_sieve_tables(tables);
        upload_backward_offsets(offsets.dr, offsets.dc, offsets.count);
        upload_constants();
        return run_campaign(argv[2], argv[3], argv[4]);
    }

    // Early dispatch for stream mode — persistent subprocess with pipe protocol
    if (argc >= 2 && std::strcmp(argv[1], "stream") == 0) {
        // GPU init (sieve tables, offsets, constants) — done once for entire session
        SieveTablesBarrett tables{};
        if (!init_sieve_tables_host(tables)) {
            std::fprintf(stderr, "failed to initialize sieve tables\n");
            return 1;
        }
        BackwardOffsetsHost offsets{};
        init_backward_offsets_host(offsets);
        upload_sieve_tables(tables);
        upload_backward_offsets(offsets.dr, offsets.dc, offsets.count);
        upload_constants();
        return run_stream();
    }

    // Early dispatch for dump mode (before parse_args, which doesn't understand it)
    if (argc >= 2 && std::strcmp(argv[1], "dump") == 0) {
        if (argc < 4) {
            std::fprintf(stderr, "Usage: %s dump <coords.bin> <output.bin>\n", argv[0]);
            return 1;
        }
        // GPU init (sieve tables, offsets, constants) required before any kernel launch
        SieveTablesBarrett tables{};
        if (!init_sieve_tables_host(tables)) {
            std::fprintf(stderr, "failed to initialize sieve tables\n");
            return 1;
        }
        BackwardOffsetsHost offsets{};
        init_backward_offsets_host(offsets);
        upload_sieve_tables(tables);
        upload_backward_offsets(offsets.dr, offsets.dc, offsets.count);
        upload_constants();
        return run_dump(argv[2], argv[3]);
    }

    Args args = parse_args(argc, argv);

    SieveTablesBarrett tables{};
    if (!init_sieve_tables_host(tables)) {
        std::fprintf(stderr, "failed to initialize sieve tables\n");
        return 1;
    }

    BackwardOffsetsHost offsets{};
    init_backward_offsets_host(offsets);

    upload_sieve_tables(tables);
    upload_backward_offsets(offsets.dr, offsets.dc, offsets.count);
    upload_constants();

    if (args.mode == Mode::TEST) {
        run_test();
    } else {
        run_bench(args.tile_count);
    }

    return 0;
}
