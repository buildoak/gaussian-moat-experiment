#include "gpu_uf.cuh"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <cuda_runtime.h>

// IMPORTANT: do NOT include face_extract.cuh or tile_kernel.cuh here.
// Those headers define non-inline __global__ kernels. Including them in
// both this TU and fat_stripe_cuda.cu causes duplicate-symbol link errors.
// Only face_port_io.h (structs) and types.h (TileGeometry) are safe to include.
#include "face_port_io.h"
#include "types.h"

namespace {

constexpr uint32_t kBlockSize = 256u;
constexpr uint32_t kMaxPrimesPerTile = 8192u;
constexpr uint32_t kMaxOffsets = 128u;
constexpr uint32_t kInvalidPrime = 0xFFFFFFFFu;

__constant__ int2 c_offsets[kMaxOffsets];
__constant__ int c_num_offsets;

struct TileSharedScalars {
    uint32_t num_primes;
    uint32_t num_components;
    uint32_t overflow;
    uint32_t origin_anchor;
    int32_t origin_component;
    uint32_t face_counts[4];
};

struct TileSharedView {
    uint32_t* bitmap;
    uint32_t* prefix;
    uint32_t* prime_pos;
    TileSharedScalars* scalars;
};

inline size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

size_t tile_shared_bytes(uint32_t bitmap_words) {
    size_t offset = 0;

    offset = align_up(offset, alignof(uint32_t));
    offset += static_cast<size_t>(bitmap_words) * sizeof(uint32_t);

    offset = align_up(offset, alignof(uint32_t));
    offset += static_cast<size_t>(bitmap_words) * sizeof(uint32_t);

    offset = align_up(offset, alignof(uint32_t));
    offset += static_cast<size_t>(kMaxPrimesPerTile) * sizeof(uint32_t);

    offset = align_up(offset, alignof(TileSharedScalars));
    offset += sizeof(TileSharedScalars);

    return offset;
}

uint64_t ceil_sqrt_u64(uint64_t value) {
    if (value == 0) {
        return 0;
    }

    uint64_t root = static_cast<uint64_t>(std::sqrt(static_cast<long double>(value)));
    while (static_cast<unsigned __int128>(root) * root < value) {
        ++root;
    }
    while (root > 0 &&
           static_cast<unsigned __int128>(root - 1) * static_cast<unsigned __int128>(root - 1) >=
               value) {
        --root;
    }
    return root;
}

cudaError_t init_offset_table(uint64_t k_sq) {
    static std::mutex mutex;
    static uint64_t cached_k_sq = std::numeric_limits<uint64_t>::max();

    std::lock_guard<std::mutex> lock(mutex);
    if (cached_k_sq == k_sq) {
        return cudaSuccess;
    }

    const uint64_t collar_u64 = ceil_sqrt_u64(k_sq);
    if (collar_u64 > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        return cudaErrorInvalidValue;
    }

    std::vector<int2> offsets;
    const int collar = static_cast<int>(collar_u64);
    offsets.reserve(kMaxOffsets);
    for (int dy = -collar; dy <= collar; ++dy) {
        for (int dx = -collar; dx <= collar; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }

            const int64_t dist_sq = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
            if (dist_sq <= static_cast<int64_t>(k_sq)) {
                if (offsets.size() >= kMaxOffsets) {
                    return cudaErrorInvalidValue;
                }
                offsets.push_back(int2{dx, dy});
            }
        }
    }

    int count = static_cast<int>(offsets.size());
    cudaError_t status = cudaSuccess;
    if (!offsets.empty()) {
        status = cudaMemcpyToSymbol(
            c_offsets,
            offsets.data(),
            offsets.size() * sizeof(int2));
        if (status != cudaSuccess) {
            return status;
        }
    }

    status = cudaMemcpyToSymbol(c_num_offsets, &count, sizeof(count));
    if (status != cudaSuccess) {
        return status;
    }

    cached_k_sq = k_sq;
    return cudaSuccess;
}

// Read a single bit from a packed-uint32 bitmap.
__device__ __forceinline__
bool bitmap_test_dev(const uint32_t* bitmap, uint32_t idx) {
    return ((bitmap[idx >> 5] >> (idx & 31u)) & 1u) != 0u;
}

__device__ __forceinline__
uint32_t prefix_rank_dev(const uint32_t* prefix, const uint32_t* bitmap, uint32_t idx) {
    const uint32_t word_idx = idx >> 5;
    const uint32_t bit_idx = idx & 31u;
    const uint32_t lower_mask = bit_idx == 0u ? 0u : ((1u << bit_idx) - 1u);
    return prefix[word_idx] + __popc(bitmap[word_idx] & lower_mask);
}

__device__ __forceinline__
size_t align_up_dev(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

__device__ __forceinline__
TileSharedView shared_view(char* base, uint32_t bitmap_words) {
    TileSharedView view{};
    size_t offset = 0;

    offset = align_up_dev(offset, alignof(uint32_t));
    view.bitmap = reinterpret_cast<uint32_t*>(base + offset);
    offset += static_cast<size_t>(bitmap_words) * sizeof(uint32_t);

    offset = align_up_dev(offset, alignof(uint32_t));
    view.prefix = reinterpret_cast<uint32_t*>(base + offset);
    offset += static_cast<size_t>(bitmap_words) * sizeof(uint32_t);

    offset = align_up_dev(offset, alignof(uint32_t));
    view.prime_pos = reinterpret_cast<uint32_t*>(base + offset);
    offset += static_cast<size_t>(kMaxPrimesPerTile) * sizeof(uint32_t);

    offset = align_up_dev(offset, alignof(TileSharedScalars));
    view.scalars = reinterpret_cast<TileSharedScalars*>(base + offset);
    offset += sizeof(TileSharedScalars);

    return view;
}

// Absolute value: int64 -> uint64 without overflow.
__device__ __forceinline__
uint64_t abs_i64_u64(int64_t value) {
    return value < 0 ? static_cast<uint64_t>(-(value + 1)) + 1ULL
                     : static_cast<uint64_t>(value);
}

// Gaussian norm a^2 + b^2 using a 128-bit intermediate.
__device__ __forceinline__
uint64_t gaussian_norm_dev(int64_t a, int64_t b) {
    const uint64_t ua = abs_i64_u64(a);
    const uint64_t ub = abs_i64_u64(b);
    const unsigned __int128 norm =
        static_cast<unsigned __int128>(ua) * ua +
        static_cast<unsigned __int128>(ub) * ub;
    return static_cast<uint64_t>(norm);
}

__device__ __forceinline__
uint32_t uf_find_dev(uint32_t* parent, uint32_t x) {
    while (true) {
        const uint32_t p = parent[x];
        if (p == x) {
            return x;
        }

        const uint32_t gp = parent[p];
        if (gp != p) {
            atomicCAS(&parent[x], p, gp);
        }
        x = gp;
    }
}

__device__ __forceinline__
void uf_union_dev(uint32_t* parent, uint8_t* rank, uint32_t a, uint32_t b) {
    while (true) {
        uint32_t ra = uf_find_dev(parent, a);
        uint32_t rb = uf_find_dev(parent, b);
        if (ra == rb) {
            return;
        }

        if (rank[ra] < rank[rb] || (rank[ra] == rank[rb] && ra > rb)) {
            const uint32_t tmp = ra;
            ra = rb;
            rb = tmp;
        }

        if (atomicCAS(&parent[rb], rb, ra) == rb) {
            if (rank[ra] == rank[rb] && rank[ra] != 0xFFu) {
                rank[ra] = static_cast<uint8_t>(rank[ra] + 1u);
            }
            return;
        }
    }
}

__device__ __forceinline__
bool in_nominal_bounds(
    int64_t a,
    int64_t b,
    int64_t a_lo,
    int64_t a_hi,
    int64_t b_lo,
    int64_t b_hi
) {
    return a >= a_lo && a <= a_hi && b >= b_lo && b <= b_hi;
}

__device__
void write_empty_tile(
    uint32_t tile_idx,
    uint32_t num_primes,
    int32_t origin_component,
    uint32_t* d_face_counts,
    uint32_t* d_num_components,
    uint32_t* d_num_primes,
    int32_t* d_origin_component
) {
    d_num_primes[tile_idx] = num_primes;
    d_num_components[tile_idx] = 0u;
    d_origin_component[tile_idx] = origin_component;

    uint32_t* tile_face_counts = d_face_counts + tile_idx * 4u;
    tile_face_counts[0] = 0u;
    tile_face_counts[1] = 0u;
    tile_face_counts[2] = 0u;
    tile_face_counts[3] = 0u;
}

__global__
void gpu_uf_tile_kernel(
    const uint32_t* __restrict__ d_bitmaps,
    uint32_t bitmap_words,
    uint32_t total_points,
    uint32_t side_exp,
    uint32_t side,
    uint32_t collar,
    uint64_t k_sq,
    const TileJob* __restrict__ d_jobs,
    uint32_t* __restrict__ d_g_parent,
    uint32_t* __restrict__ d_g_root_comp,
    uint8_t*  __restrict__ d_g_rank,
    FacePortRecord* __restrict__ d_face_inner,
    FacePortRecord* __restrict__ d_face_outer,
    FacePortRecord* __restrict__ d_face_left,
    FacePortRecord* __restrict__ d_face_right,
    uint32_t* __restrict__ d_face_counts,
    uint32_t* __restrict__ d_num_components,
    uint32_t* __restrict__ d_num_primes,
    int32_t* __restrict__ d_origin_component
) {
    const uint32_t tile_idx = blockIdx.x;
    const uint32_t tid = threadIdx.x;

    extern __shared__ char s_mem[];
    TileSharedView shared = shared_view(s_mem, bitmap_words);
    TileSharedScalars& scalars = *shared.scalars;
    // Global memory UF arrays for this tile (moved out of shared to save shmem)
    uint32_t* g_parent    = d_g_parent    + static_cast<size_t>(tile_idx) * total_points;
    uint32_t* g_root_comp = d_g_root_comp + static_cast<size_t>(tile_idx) * total_points;
    uint8_t*  g_rank      = d_g_rank      + static_cast<size_t>(tile_idx) * total_points;

    if (tid == 0u) {
        scalars.num_primes = 0u;
        scalars.num_components = 0u;
        scalars.overflow = 0u;
        scalars.origin_anchor = kInvalidPrime;
        scalars.origin_component = -1;
        scalars.face_counts[0] = 0u;
        scalars.face_counts[1] = 0u;
        scalars.face_counts[2] = 0u;
        scalars.face_counts[3] = 0u;
    }
    __syncthreads();

    // Phase 1: load the tile bitmap into shared memory. Mask off padding bits
    // in the final word so rank queries never see out-of-range points.
    const uint32_t* tile_bitmap = d_bitmaps + static_cast<uint64_t>(tile_idx) * bitmap_words;
    const uint32_t tail_bits = total_points & 31u;
    const uint32_t tail_mask = tail_bits == 0u ? 0xFFFFFFFFu : ((1u << tail_bits) - 1u);
    for (uint32_t word_idx = tid; word_idx < bitmap_words; word_idx += blockDim.x) {
        uint32_t word = tile_bitmap[word_idx];
        if (word_idx + 1u == bitmap_words) {
            word &= tail_mask;
        }
        shared.bitmap[word_idx] = word;
    }
    __syncthreads();

    // Phase 2: build an exclusive prefix popcount table. The word count is
    // small enough that a single-thread scan is simpler and still cheap.
    if (tid == 0u) {
        uint32_t running = 0u;
        for (uint32_t word_idx = 0; word_idx < bitmap_words; ++word_idx) {
            shared.prefix[word_idx] = running;
            running += static_cast<uint32_t>(__popc(shared.bitmap[word_idx]));
        }
        scalars.num_primes = running;
        scalars.overflow = running > kMaxPrimesPerTile ? 1u : 0u;
    }
    __syncthreads();

    if (scalars.overflow != 0u) {
        if (tid == 0u) {
            printf(
                "gpu_uf_tile_kernel: tile %u has %u primes; max supported is %u\n",
                tile_idx,
                scalars.num_primes,
                kMaxPrimesPerTile);
            write_empty_tile(
                tile_idx,
                scalars.num_primes,
                -2,
                d_face_counts,
                d_num_components,
                d_num_primes,
                d_origin_component);
        }
        return;
    }

    // Phase 3: compact set bits into a dense prime-position array. Because
    // the prefix table is exclusive, each thread can write its slice without
    // atomics and the resulting order is row-major / bitmap order.
    for (uint32_t word_idx = tid; word_idx < bitmap_words; word_idx += blockDim.x) {
        uint32_t word = shared.bitmap[word_idx];
        uint32_t out_idx = shared.prefix[word_idx];
        while (word != 0u) {
            const uint32_t bit = static_cast<uint32_t>(__ffs(word) - 1);
            shared.prime_pos[out_idx++] = (word_idx << 5) + bit;
            word &= (word - 1u);
        }
    }

    // Phase 4: initialize the global-memory union-find arrays.
    for (uint32_t i = tid; i < scalars.num_primes; i += blockDim.x) {
        g_parent[i] = i;
        g_rank[i] = 0u;
        g_root_comp[i] = gm::kNoComponent;
    }
    __syncthreads();

    // Phase 5: union every prime with its prime neighbors inside the tile.
    // Neighbor membership is tested against the shared bitmap, and the shared
    // prefix table maps the neighbor bit position back to its compact UF index.
    for (uint32_t i = tid; i < scalars.num_primes; i += blockDim.x) {
        const uint32_t pos = shared.prime_pos[i];
        const uint32_t row = pos / side_exp;
        const uint32_t col = pos % side_exp;

        for (int off = 0; off < c_num_offsets; ++off) {
            const int nr = static_cast<int>(row) + c_offsets[off].y;
            const int nc = static_cast<int>(col) + c_offsets[off].x;
            if (nr < 0 || nc < 0 || nr >= static_cast<int>(side_exp) || nc >= static_cast<int>(side_exp)) {
                continue;
            }

            const uint32_t neighbor_pos =
                static_cast<uint32_t>(nr) * side_exp + static_cast<uint32_t>(nc);
            if (!bitmap_test_dev(shared.bitmap, neighbor_pos)) {
                continue;
            }

            const uint32_t neighbor_idx = prefix_rank_dev(shared.prefix, shared.bitmap, neighbor_pos);
            uf_union_dev(g_parent, g_rank, i, neighbor_idx);
        }
    }
    __syncthreads();

    const TileJob job = d_jobs[tile_idx];
    const int64_t a_lo = static_cast<int64_t>(job.a_lo);
    const int64_t b_lo = static_cast<int64_t>(job.b_lo);
    const int64_t a_hi = a_lo + static_cast<int64_t>(side);
    const int64_t b_hi = b_lo + static_cast<int64_t>(side);
    const int64_t expanded_a_lo = a_lo - static_cast<int64_t>(collar);
    const int64_t expanded_b_lo = b_lo - static_cast<int64_t>(collar);
    const bool tile_contains_origin =
        a_lo <= 0 && 0 <= a_hi &&
        b_lo <= 0 && 0 <= b_hi;

    // Phase 6: if this nominal tile contains the origin, collapse every
    // prime with norm <= k^2 into one component before the final flatten.
    if (tile_contains_origin) {
        if (tid == 0u) {
            for (uint32_t i = 0; i < scalars.num_primes; ++i) {
                const uint32_t pos = shared.prime_pos[i];
                const int64_t row = static_cast<int64_t>(pos / side_exp);
                const int64_t col = static_cast<int64_t>(pos % side_exp);
                const int64_t a = expanded_a_lo + row;
                const int64_t b = expanded_b_lo + col;
                if (gaussian_norm_dev(a, b) <= k_sq) {
                    scalars.origin_anchor = i;
                    break;
                }
            }
        }
        __syncthreads();

        if (scalars.origin_anchor != kInvalidPrime) {
            for (uint32_t i = tid; i < scalars.num_primes; i += blockDim.x) {
                const uint32_t pos = shared.prime_pos[i];
                const int64_t row = static_cast<int64_t>(pos / side_exp);
                const int64_t col = static_cast<int64_t>(pos % side_exp);
                const int64_t a = expanded_a_lo + row;
                const int64_t b = expanded_b_lo + col;
                if (gaussian_norm_dev(a, b) <= k_sq && i != scalars.origin_anchor) {
                    uf_union_dev(g_parent, g_rank, scalars.origin_anchor, i);
                }
            }
            __syncthreads();
        }
    }

    // Phase 7: flatten the UF forest. Three passes are enough here because
    // union already uses path splitting and the trees are shallow.
    for (int pass = 0; pass < 3; ++pass) {
        for (uint32_t i = tid; i < scalars.num_primes; i += blockDim.x) {
            g_parent[i] = uf_find_dev(g_parent, i);
        }
        __syncthreads();
    }

    // Phase 8: assign deterministic component IDs and emit face ports. This
    // is done by thread 0 in compact-prime order so the output ordering matches
    // the CPU reference's row-major traversal.
    if (tid == 0u) {
        const uint64_t tile_face_off = static_cast<uint64_t>(tile_idx) * gm::kMaxFacePortsPerFace;
        for (uint32_t i = 0; i < scalars.num_primes; ++i) {
            const uint32_t pos = shared.prime_pos[i];
            const int64_t row = static_cast<int64_t>(pos / side_exp);
            const int64_t col = static_cast<int64_t>(pos % side_exp);
            const int64_t a = expanded_a_lo + row;
            const int64_t b = expanded_b_lo + col;

            if (!in_nominal_bounds(a, b, a_lo, a_hi, b_lo, b_hi)) {
                continue;
            }

            const uint32_t root = g_parent[i];
            uint32_t component = g_root_comp[root];
            if (component == gm::kNoComponent) {
                component = scalars.num_components++;
                g_root_comp[root] = component;
            }

            const FacePortRecord record{
                static_cast<int32_t>(a),
                static_cast<int32_t>(b),
                component,
            };

            if (static_cast<uint64_t>(a - a_lo) <= collar) {
                const uint32_t out = scalars.face_counts[0];
                if (out < gm::kMaxFacePortsPerFace) {
                    d_face_inner[tile_face_off + out] = record;
                }
                if (scalars.face_counts[0] < gm::kMaxFacePortsPerFace) {
                    ++scalars.face_counts[0];
                }
            }
            if (static_cast<uint64_t>(a_hi - a) <= collar) {
                const uint32_t out = scalars.face_counts[1];
                if (out < gm::kMaxFacePortsPerFace) {
                    d_face_outer[tile_face_off + out] = record;
                }
                if (scalars.face_counts[1] < gm::kMaxFacePortsPerFace) {
                    ++scalars.face_counts[1];
                }
            }
            if (static_cast<uint64_t>(b - b_lo) <= collar) {
                const uint32_t out = scalars.face_counts[2];
                if (out < gm::kMaxFacePortsPerFace) {
                    d_face_left[tile_face_off + out] = record;
                }
                if (scalars.face_counts[2] < gm::kMaxFacePortsPerFace) {
                    ++scalars.face_counts[2];
                }
            }
            if (static_cast<uint64_t>(b_hi - b) <= collar) {
                const uint32_t out = scalars.face_counts[3];
                if (out < gm::kMaxFacePortsPerFace) {
                    d_face_right[tile_face_off + out] = record;
                }
                if (scalars.face_counts[3] < gm::kMaxFacePortsPerFace) {
                    ++scalars.face_counts[3];
                }
            }
        }

        if (tile_contains_origin && scalars.origin_anchor != kInvalidPrime) {
            const uint32_t origin_root = g_parent[scalars.origin_anchor];
            const uint32_t component = g_root_comp[origin_root];
            if (component != gm::kNoComponent) {
                scalars.origin_component = static_cast<int32_t>(component);
            }
        }

        d_num_primes[tile_idx] = scalars.num_primes;
        d_num_components[tile_idx] = scalars.num_components;
        d_origin_component[tile_idx] = scalars.origin_component;

        uint32_t* tile_face_counts = d_face_counts + tile_idx * 4u;
        tile_face_counts[0] = scalars.face_counts[0];
        tile_face_counts[1] = scalars.face_counts[1];
        tile_face_counts[2] = scalars.face_counts[2];
        tile_face_counts[3] = scalars.face_counts[3];
    }
}

static cudaError_t alloc_mirror(
    void** d_ptr, void** h_ptr, size_t bytes
) {
    cudaError_t status = cudaMalloc(d_ptr, bytes);
    if (status != cudaSuccess) {
        return status;
    }

    status = cudaMallocHost(h_ptr, bytes);
    if (status != cudaSuccess) {
        cudaFree(*d_ptr);
        *d_ptr = nullptr;
    }
    return status;
}

static void free_mirror(void* d_ptr, void* h_ptr) {
    if (d_ptr != nullptr) {
        cudaFree(d_ptr);
    }
    if (h_ptr != nullptr) {
        cudaFreeHost(h_ptr);
    }
}

} // namespace

namespace gm {

cudaError_t create_gpu_uf_context(
    uint32_t batch_cap,
    uint64_t total_points,
    GpuUfContext* ctx
) {
    if (ctx == nullptr || batch_cap == 0u || total_points == 0u) {
        return cudaErrorInvalidValue;
    }

    GpuUfContext next{};
    next.batch_capacity = batch_cap;
    next.total_points = total_points;

    const size_t tile_count = static_cast<size_t>(batch_cap);
    const size_t point_count = static_cast<size_t>(total_points);
    const size_t face_cap = static_cast<size_t>(kMaxFacePortsPerFace);

    cudaError_t status = cudaSuccess;

    status = cudaMalloc(&next.d_parent, tile_count * point_count * sizeof(uint32_t));
    if (status != cudaSuccess) {
        goto cleanup;
    }
    status = cudaMalloc(&next.d_comp_id, tile_count * point_count * sizeof(uint32_t));
    if (status != cudaSuccess) {
        goto cleanup;
    }
    status = cudaMalloc(&next.d_comp_counter, tile_count * sizeof(uint32_t));
    if (status != cudaSuccess) {
        goto cleanup;
    }
    status = cudaMalloc(&next.d_rank, tile_count * point_count * sizeof(uint8_t));
    if (status != cudaSuccess) {
        goto cleanup;
    }

    status = cudaMalloc(&next.d_face_inner, tile_count * face_cap * sizeof(FacePortRecord));
    if (status != cudaSuccess) {
        goto cleanup;
    }
    status = cudaMalloc(&next.d_face_outer, tile_count * face_cap * sizeof(FacePortRecord));
    if (status != cudaSuccess) {
        goto cleanup;
    }
    status = cudaMalloc(&next.d_face_left, tile_count * face_cap * sizeof(FacePortRecord));
    if (status != cudaSuccess) {
        goto cleanup;
    }
    status = cudaMalloc(&next.d_face_right, tile_count * face_cap * sizeof(FacePortRecord));
    if (status != cudaSuccess) {
        goto cleanup;
    }

    status = alloc_mirror(
        reinterpret_cast<void**>(&next.d_face_counts),
        reinterpret_cast<void**>(&next.h_face_counts),
        tile_count * 4u * sizeof(uint32_t));
    if (status != cudaSuccess) {
        goto cleanup;
    }

    status = alloc_mirror(
        reinterpret_cast<void**>(&next.d_num_components),
        reinterpret_cast<void**>(&next.h_num_components),
        tile_count * sizeof(uint32_t));
    if (status != cudaSuccess) {
        goto cleanup;
    }

    status = alloc_mirror(
        reinterpret_cast<void**>(&next.d_origin_component),
        reinterpret_cast<void**>(&next.h_origin_component),
        tile_count * sizeof(int32_t));
    if (status != cudaSuccess) {
        goto cleanup;
    }

    status = alloc_mirror(
        reinterpret_cast<void**>(&next.d_num_primes),
        reinterpret_cast<void**>(&next.h_num_primes),
        tile_count * sizeof(uint32_t));
    if (status != cudaSuccess) {
        goto cleanup;
    }

    status = cudaMalloc(&next.d_origin_set, tile_count * sizeof(int32_t));
    if (status != cudaSuccess) {
        goto cleanup;
    }

    status = cudaMallocHost(&next.h_face_inner, tile_count * face_cap * sizeof(FacePortRecord));
    if (status != cudaSuccess) {
        goto cleanup;
    }
    status = cudaMallocHost(&next.h_face_outer, tile_count * face_cap * sizeof(FacePortRecord));
    if (status != cudaSuccess) {
        goto cleanup;
    }
    status = cudaMallocHost(&next.h_face_left, tile_count * face_cap * sizeof(FacePortRecord));
    if (status != cudaSuccess) {
        goto cleanup;
    }
    status = cudaMallocHost(&next.h_face_right, tile_count * face_cap * sizeof(FacePortRecord));
    if (status != cudaSuccess) {
        goto cleanup;
    }

    *ctx = next;
    return cudaSuccess;

cleanup:
    {
        GpuUfContext tmp = next;
        destroy_gpu_uf_context(&tmp);
    }
    return status;
}

void destroy_gpu_uf_context(GpuUfContext* ctx) {
    if (ctx == nullptr) {
        return;
    }

    if (ctx->d_parent != nullptr) {
        cudaFree(ctx->d_parent);
    }
    if (ctx->d_comp_id != nullptr) {
        cudaFree(ctx->d_comp_id);
    }
    if (ctx->d_comp_counter != nullptr) {
        cudaFree(ctx->d_comp_counter);
    }
    if (ctx->d_rank != nullptr) {
        cudaFree(ctx->d_rank);
    }
    if (ctx->d_face_inner != nullptr) {
        cudaFree(ctx->d_face_inner);
    }
    if (ctx->d_face_outer != nullptr) {
        cudaFree(ctx->d_face_outer);
    }
    if (ctx->d_face_left != nullptr) {
        cudaFree(ctx->d_face_left);
    }
    if (ctx->d_face_right != nullptr) {
        cudaFree(ctx->d_face_right);
    }
    if (ctx->d_origin_set != nullptr) {
        cudaFree(ctx->d_origin_set);
    }

    free_mirror(ctx->d_face_counts, ctx->h_face_counts);
    free_mirror(ctx->d_num_components, ctx->h_num_components);
    free_mirror(ctx->d_origin_component, ctx->h_origin_component);
    free_mirror(ctx->d_num_primes, ctx->h_num_primes);

    if (ctx->h_face_inner != nullptr) {
        cudaFreeHost(ctx->h_face_inner);
    }
    if (ctx->h_face_outer != nullptr) {
        cudaFreeHost(ctx->h_face_outer);
    }
    if (ctx->h_face_left != nullptr) {
        cudaFreeHost(ctx->h_face_left);
    }
    if (ctx->h_face_right != nullptr) {
        cudaFreeHost(ctx->h_face_right);
    }

    *ctx = GpuUfContext{};
}

cudaError_t run_gpu_uf(
    GpuUfContext& ctx,
    const uint32_t* d_bitmaps,
    const TileJob* d_jobs,
    uint32_t num_tiles,
    uint32_t tile_side,
    int64_t collar,
    uint64_t k_sq,
    uint64_t side_exp,
    size_t bitmap_words
) {
    if (num_tiles == 0u || num_tiles > ctx.batch_capacity || d_bitmaps == nullptr || d_jobs == nullptr) {
        return cudaErrorInvalidValue;
    }
    if (tile_side > 512u || collar < 0) {
        return cudaErrorInvalidValue;
    }
    if (side_exp == 0u || ctx.total_points == 0u) {
        return cudaErrorInvalidValue;
    }

    const uint64_t expected_side_exp =
        static_cast<uint64_t>(tile_side) + 1ULL + 2ULL * static_cast<uint64_t>(collar);
    if (side_exp != expected_side_exp) {
        return cudaErrorInvalidValue;
    }

    const uint64_t expected_total_points = side_exp * side_exp;
    if (expected_total_points != ctx.total_points) {
        return cudaErrorInvalidValue;
    }

    const size_t expected_bitmap_words =
        static_cast<size_t>((ctx.total_points + 31ULL) / 32ULL);
    if (bitmap_words != expected_bitmap_words || bitmap_words > std::numeric_limits<uint32_t>::max()) {
        return cudaErrorInvalidValue;
    }

    cudaError_t status = init_offset_table(k_sq);
    if (status != cudaSuccess) {
        return status;
    }

    const size_t shared_bytes = tile_shared_bytes(static_cast<uint32_t>(bitmap_words));
    int current_device = 0;
    status = cudaGetDevice(&current_device);
    if (status != cudaSuccess) {
        return status;
    }

    int shared_limit = 0;
    status = cudaDeviceGetAttribute(
        &shared_limit,
        cudaDevAttrMaxSharedMemoryPerBlockOptin,
        current_device);
    if (status != cudaSuccess || shared_limit == 0) {
        status = cudaDeviceGetAttribute(
            &shared_limit,
            cudaDevAttrMaxSharedMemoryPerBlock,
            current_device);
        if (status != cudaSuccess) {
            return status;
        }
    }
    if (shared_bytes > static_cast<size_t>(shared_limit)) {
        return cudaErrorInvalidValue;
    }

    status = cudaFuncSetAttribute(
        gpu_uf_tile_kernel,
        cudaFuncAttributeMaxDynamicSharedMemorySize,
        static_cast<int>(shared_bytes));
    if (status != cudaSuccess) {
        return status;
    }

    status = cudaMemset(ctx.d_face_counts, 0, static_cast<size_t>(num_tiles) * 4u * sizeof(uint32_t));
    if (status != cudaSuccess) {
        return status;
    }
    status = cudaMemset(ctx.d_num_components, 0, static_cast<size_t>(num_tiles) * sizeof(uint32_t));
    if (status != cudaSuccess) {
        return status;
    }
    status = cudaMemset(ctx.d_num_primes, 0, static_cast<size_t>(num_tiles) * sizeof(uint32_t));
    if (status != cudaSuccess) {
        return status;
    }
    status = cudaMemset(ctx.d_origin_component, 0xFF, static_cast<size_t>(num_tiles) * sizeof(int32_t));
    if (status != cudaSuccess) {
        return status;
    }

    gpu_uf_tile_kernel<<<num_tiles, kBlockSize, shared_bytes>>>(
        d_bitmaps,
        static_cast<uint32_t>(bitmap_words),
        static_cast<uint32_t>(ctx.total_points),
        static_cast<uint32_t>(side_exp),
        tile_side,
        static_cast<uint32_t>(collar),
        k_sq,
        d_jobs,
        ctx.d_parent,
        ctx.d_comp_id,
        ctx.d_rank,
        ctx.d_face_inner,
        ctx.d_face_outer,
        ctx.d_face_left,
        ctx.d_face_right,
        ctx.d_face_counts,
        ctx.d_num_components,
        ctx.d_num_primes,
        ctx.d_origin_component);
    status = cudaGetLastError();
    if (status != cudaSuccess) {
        return status;
    }

    status = cudaDeviceSynchronize();
    if (status != cudaSuccess) {
        return status;
    }

    const size_t tile_count = static_cast<size_t>(num_tiles);
    const size_t face_cap = static_cast<size_t>(kMaxFacePortsPerFace);

    status = cudaMemcpy(
        ctx.h_face_counts,
        ctx.d_face_counts,
        tile_count * 4u * sizeof(uint32_t),
        cudaMemcpyDeviceToHost);
    if (status != cudaSuccess) {
        return status;
    }
    status = cudaMemcpy(
        ctx.h_num_components,
        ctx.d_num_components,
        tile_count * sizeof(uint32_t),
        cudaMemcpyDeviceToHost);
    if (status != cudaSuccess) {
        return status;
    }
    status = cudaMemcpy(
        ctx.h_origin_component,
        ctx.d_origin_component,
        tile_count * sizeof(int32_t),
        cudaMemcpyDeviceToHost);
    if (status != cudaSuccess) {
        return status;
    }
    status = cudaMemcpy(
        ctx.h_num_primes,
        ctx.d_num_primes,
        tile_count * sizeof(uint32_t),
        cudaMemcpyDeviceToHost);
    if (status != cudaSuccess) {
        return status;
    }

    for (uint32_t tile_idx = 0; tile_idx < num_tiles; ++tile_idx) {
        const uint32_t* face_counts = ctx.h_face_counts + tile_idx * 4u;
        const uint64_t tile_face_off = static_cast<uint64_t>(tile_idx) * face_cap;

        if (face_counts[0] > 0u) {
            status = cudaMemcpy(
                ctx.h_face_inner + tile_face_off,
                ctx.d_face_inner + tile_face_off,
                static_cast<size_t>(face_counts[0]) * sizeof(FacePortRecord),
                cudaMemcpyDeviceToHost);
            if (status != cudaSuccess) {
                return status;
            }
        }
        if (face_counts[1] > 0u) {
            status = cudaMemcpy(
                ctx.h_face_outer + tile_face_off,
                ctx.d_face_outer + tile_face_off,
                static_cast<size_t>(face_counts[1]) * sizeof(FacePortRecord),
                cudaMemcpyDeviceToHost);
            if (status != cudaSuccess) {
                return status;
            }
        }
        if (face_counts[2] > 0u) {
            status = cudaMemcpy(
                ctx.h_face_left + tile_face_off,
                ctx.d_face_left + tile_face_off,
                static_cast<size_t>(face_counts[2]) * sizeof(FacePortRecord),
                cudaMemcpyDeviceToHost);
            if (status != cudaSuccess) {
                return status;
            }
        }
        if (face_counts[3] > 0u) {
            status = cudaMemcpy(
                ctx.h_face_right + tile_face_off,
                ctx.d_face_right + tile_face_off,
                static_cast<size_t>(face_counts[3]) * sizeof(FacePortRecord),
                cudaMemcpyDeviceToHost);
            if (status != cudaSuccess) {
                return status;
            }
        }
    }

    return cudaSuccess;
}

} // namespace gm
