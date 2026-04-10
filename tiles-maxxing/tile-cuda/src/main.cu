#include "gpu_constants.cuh"
#include "gpu_types.cuh"

#include <cuda_runtime.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

__global__ void process_tiles_kernel(const TileCoord* coords,
                                     TileOp* output,
                                     uint32_t* prime_counts,
                                     int num_tiles);

void upload_sieve_tables(const SieveTables& tables);
void upload_backward_offsets(const int8_t* bk_dr, const int8_t* bk_dc, int count);
void upload_constants();
size_t tile_kernel_shared_bytes();

namespace {

constexpr uint32_t SIEVE_LIMIT = 10000U;

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

bool init_sieve_tables_host(SieveTables& tables) {
    std::memset(&tables, 0, sizeof(tables));

    uint8_t is_prime_table[SIEVE_LIMIT + 1];
    std::memset(is_prime_table, 1, sizeof(is_prime_table));
    is_prime_table[0] = 0U;
    is_prime_table[1] = 0U;

    for (uint32_t p = 2U; p * p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) {
            continue;
        }
        for (uint32_t multiple = p * p; multiple <= SIEVE_LIMIT; multiple += p) {
            is_prime_table[multiple] = 0U;
        }
    }

    for (uint32_t p = 2U; p <= SIEVE_LIMIT; ++p) {
        if (is_prime_table[p] == 0U) {
            continue;
        }

        if ((p & 3U) == 1U) {
            const uint64_t root_raw = fast_sqrt_neg1(static_cast<uint64_t>(p));
            if (root_raw == std::numeric_limits<uint64_t>::max()) {
                return false;
            }

            uint64_t root = root_raw;
            const uint64_t neg_root = static_cast<uint64_t>(p) - root;
            if (neg_root < root) {
                root = neg_root;
            }

            if (mulmod_small(root, root, static_cast<uint64_t>(p)) != static_cast<uint64_t>(p - 1U)) {
                return false;
            }
            if (tables.split_count >= SPLIT_PRIMES_COUNT) {
                return false;
            }

            tables.split_table[tables.split_count++] =
                (static_cast<uint32_t>(root) << 16) | p;
        } else if ((p & 3U) == 3U) {
            if (tables.inert_count >= INERT_PRIMES_COUNT) {
                return false;
            }
            tables.inert_primes[tables.inert_count++] = static_cast<uint16_t>(p);
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
            if ((dr > 0) || (dr == 0 && dc >= 0)) {
                continue;
            }
            if ((dr * dr + dc * dc) > K_SQ) {
                continue;
            }

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

}  // namespace

int main() {
    SieveTables tables{};
    if (!init_sieve_tables_host(tables)) {
        std::fprintf(stderr, "failed to initialize sieve tables\n");
        return 1;
    }

    BackwardOffsetsHost offsets{};
    init_backward_offsets_host(offsets);

    upload_sieve_tables(tables);
    upload_backward_offsets(offsets.dr, offsets.dc, offsets.count);
    upload_constants();

    std::vector<TileCoord> coords = default_tiles();
    const int num_tiles = static_cast<int>(coords.size());

    TileCoord* d_coords = nullptr;
    TileOp* d_output = nullptr;
    uint32_t* d_prime_counts = nullptr;

    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_coords), sizeof(TileCoord) * coords.size()),
               "cudaMalloc(d_coords)");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_output), sizeof(TileOp) * coords.size()),
               "cudaMalloc(d_output)");
    check_cuda(cudaMalloc(reinterpret_cast<void**>(&d_prime_counts),
                          sizeof(uint32_t) * coords.size()),
               "cudaMalloc(d_prime_counts)");

    check_cuda(cudaMemcpy(d_coords, coords.data(), sizeof(TileCoord) * coords.size(),
                          cudaMemcpyHostToDevice),
               "cudaMemcpy(d_coords)");

    check_cuda(cudaFuncSetAttribute(process_tiles_kernel,
                                    cudaFuncAttributeMaxDynamicSharedMemorySize,
                                    36864),
               "cudaFuncSetAttribute(process_tiles_kernel)");

    const dim3 block(BLOCK_THREADS);
    const dim3 grid(static_cast<unsigned int>(num_tiles));
    const size_t shared_bytes = tile_kernel_shared_bytes();

    cudaEvent_t start{};
    cudaEvent_t stop{};
    check_cuda(cudaEventCreate(&start), "cudaEventCreate(start)");
    check_cuda(cudaEventCreate(&stop), "cudaEventCreate(stop)");

    check_cuda(cudaEventRecord(start), "cudaEventRecord(start)");
    process_tiles_kernel<<<grid, block, shared_bytes>>>(d_coords, d_output, d_prime_counts, num_tiles);
    check_cuda(cudaEventRecord(stop), "cudaEventRecord(stop)");
    check_cuda(cudaGetLastError(), "process_tiles_kernel launch");
    check_cuda(cudaEventSynchronize(stop), "cudaEventSynchronize(stop)");

    float elapsed_ms = 0.0f;
    check_cuda(cudaEventElapsedTime(&elapsed_ms, start, stop), "cudaEventElapsedTime");

    std::vector<TileOp> output(coords.size());
    std::vector<uint32_t> prime_counts(coords.size(), 0U);
    check_cuda(cudaMemcpy(output.data(), d_output, sizeof(TileOp) * output.size(),
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy(output)");
    check_cuda(cudaMemcpy(prime_counts.data(), d_prime_counts,
                          sizeof(uint32_t) * prime_counts.size(),
                          cudaMemcpyDeviceToHost),
               "cudaMemcpy(prime_counts)");

    std::printf("processed %d tile(s) in %.3f ms (%.3f ms/tile)\n",
                num_tiles, elapsed_ms, elapsed_ms / static_cast<float>(num_tiles));
    std::printf("shared memory: %zu bytes\n", shared_bytes);

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

    cudaEventDestroy(stop);
    cudaEventDestroy(start);
    cudaFree(d_prime_counts);
    cudaFree(d_output);
    cudaFree(d_coords);
    return 0;
}
