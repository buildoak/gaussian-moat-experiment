// cuda_dump: Process tiles through the CUDA kernel and dump TileOps + prime counts
// to a binary file for cross-validation against the C++ reference.
//
// Output format (binary): same as cpp_dump.cpp
//   uint32_t num_tiles
//   For each tile:
//     int64_t a_lo, b_lo          (16 bytes)
//     uint32_t prime_count         (4 bytes)
//     uint8_t tileop[128]          (128 bytes)

#include "gpu_constants.cuh"
#include "gpu_types.cuh"
#include "test_coords.h"

#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

// Declarations from tile_kernel.cu
__global__ void process_tiles_kernel(const TileCoord* coords,
                                     TileOp* output,
                                     uint32_t* prime_counts,
                                     int num_tiles);

void upload_sieve_tables(const SieveTablesBarrett& tables);
void upload_backward_offsets(const int8_t* bk_dr, const int8_t* bk_dc, int count);
void upload_constants();
size_t tile_kernel_shared_bytes();

namespace {

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
        if (mulmod_small(x, x, p) == (p - 1ULL)) {
            return x;
        }
    }
    return std::numeric_limits<uint64_t>::max();
}

bool init_sieve_tables_host(SieveTablesBarrett& tables) {
    std::memset(&tables, 0, sizeof(tables));

    uint8_t is_prime_table[SIEVE_LIMIT + 1];
    std::memset(is_prime_table, 1, sizeof(is_prime_table));
    is_prime_table[0] = 0U;
    is_prime_table[1] = 0U;

    for (uint32_t p = 2U; p * p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) continue;
        for (uint32_t multiple = p * p; multiple <= SIEVE_LIMIT; multiple += p) {
            is_prime_table[multiple] = 0U;
        }
    }

    for (uint32_t p = 2U; p <= SIEVE_LIMIT; ++p) {
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

            tables.split_table[tables.split_count++] = SplitPrimeBarrettGPU{
                static_cast<uint16_t>(p),
                static_cast<uint16_t>(root),
                mu
            };
        } else if ((p & 3U) == 3U) {
            if (tables.inert_count >= INERT_PRIMES_COUNT) return false;
            tables.inert_primes[tables.inert_count++] = InertPrimeBarrettGPU{
                static_cast<uint16_t>(p),
                0,
                mu
            };
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
    for (int dr = -6; dr <= 0; ++dr) {
        for (int dc = -6; dc <= 6; ++dc) {
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

}  // namespace

int main(int argc, char** argv) {
    int num_tiles = 1000;
    const char* output_path = "cuda_tileops.bin";
    int batch_size = 64;  // Process tiles in batches to limit GPU memory

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            num_tiles = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            batch_size = std::atoi(argv[++i]);
        }
    }

    std::fprintf(stderr, "cuda_dump: processing %d tiles (batch=%d)\n", num_tiles, batch_size);

    // Init sieve tables
    SieveTablesBarrett tables{};
    if (!init_sieve_tables_host(tables)) {
        std::fprintf(stderr, "error: init_sieve_tables_host failed\n");
        return 1;
    }

    BackwardOffsetsHost offsets{};
    init_backward_offsets_host(offsets);

    upload_sieve_tables(tables);
    upload_backward_offsets(offsets.dr, offsets.dc, offsets.count);
    upload_constants();

    // Generate coordinates
    auto test_coords = generate_test_coords(num_tiles);

    // Convert to TileCoord
    std::vector<TileCoord> coords(num_tiles);
    for (int i = 0; i < num_tiles; ++i) {
        coords[i].a_lo = test_coords[i].a_lo;
        coords[i].b_lo = test_coords[i].b_lo;
    }

    // Allocate GPU buffers for a batch
    TileCoord* d_coords = nullptr;
    TileOp* d_output = nullptr;
    uint32_t* d_prime_counts = nullptr;

    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_coords), sizeof(TileCoord) * batch_size),
               "cudaMalloc(d_coords)");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_output), sizeof(TileOp) * batch_size),
               "cudaMalloc(d_output)");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_prime_counts), sizeof(uint32_t) * batch_size),
               "cudaMalloc(d_prime_counts)");

    const size_t shared_bytes = tile_kernel_shared_bytes();
    check_cuda(cudaFuncSetAttribute(process_tiles_kernel,
                                    cudaFuncAttributeMaxDynamicSharedMemorySize,
                                    static_cast<int>(shared_bytes)),
               "cudaFuncSetAttribute");

    // Open output file
    FILE* fp = std::fopen(output_path, "wb");
    if (!fp) {
        std::fprintf(stderr, "error: cannot open %s for writing\n", output_path);
        return 1;
    }

    uint32_t n = static_cast<uint32_t>(num_tiles);
    std::fwrite(&n, sizeof(uint32_t), 1, fp);

    auto t_start = std::chrono::steady_clock::now();

    // Process in batches
    for (int offset = 0; offset < num_tiles; offset += batch_size) {
        int this_batch = batch_size;
        if (offset + this_batch > num_tiles) {
            this_batch = num_tiles - offset;
        }

        check_cuda(cudaMemcpy(d_coords, &coords[offset], sizeof(TileCoord) * this_batch,
                              cudaMemcpyHostToDevice),
                   "cudaMemcpy(d_coords)");

        const dim3 block(BLOCK_THREADS);
        const dim3 grid(static_cast<unsigned int>(this_batch));

        process_tiles_kernel<<<grid, block, shared_bytes>>>(d_coords, d_output, d_prime_counts, this_batch);
        check_cuda(cudaGetLastError(), "kernel launch");
        check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize");

        std::vector<TileOp> output(this_batch);
        std::vector<uint32_t> prime_counts(this_batch, 0U);
        check_cuda(cudaMemcpy(output.data(), d_output, sizeof(TileOp) * this_batch,
                              cudaMemcpyDeviceToHost),
                   "cudaMemcpy(output)");
        check_cuda(cudaMemcpy(prime_counts.data(), d_prime_counts, sizeof(uint32_t) * this_batch,
                              cudaMemcpyDeviceToHost),
                   "cudaMemcpy(prime_counts)");

        for (int i = 0; i < this_batch; ++i) {
            int idx = offset + i;
            std::fwrite(&coords[idx].a_lo, sizeof(int64_t), 1, fp);
            std::fwrite(&coords[idx].b_lo, sizeof(int64_t), 1, fp);
            std::fwrite(&prime_counts[i], sizeof(uint32_t), 1, fp);
            std::fwrite(output[i].bytes, 1, 128, fp);
        }

        std::fprintf(stderr, "  cuda: %d/%d tiles done\n",
                     offset + this_batch, num_tiles);
    }

    auto t_end = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count() / 1000.0;

    std::fclose(fp);

    cudaFree(d_prime_counts);
    cudaFree(d_output);
    cudaFree(d_coords);

    std::fprintf(stderr, "cuda_dump: wrote %d tiles to %s in %.1f ms (%.1f ms/tile)\n",
                 num_tiles, output_path, elapsed_ms, elapsed_ms / num_tiles);

    return 0;
}
