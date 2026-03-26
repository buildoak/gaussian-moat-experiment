#include "gpu_uf.cuh"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <cuda_runtime.h>

// IMPORTANT: do NOT include face_extract.cuh or tile_kernel.cuh here.
// Those headers define non-inline __global__ kernels.  Including them in
// both this TU and fat_stripe_cuda.cu causes duplicate-symbol link errors.
// Only face_port_io.h (structs) and types.h (TileGeometry) are safe to include.
#include "face_port_io.h"
#include "types.h"

// ---------------------------------------------------------------------------
// Device helpers
// ---------------------------------------------------------------------------

namespace {

// Read a single bit from a packed-uint32 bitmap.
__device__ __forceinline__
bool bitmap_test_dev(const uint32_t* bitmap, uint64_t idx) {
    return ((bitmap[idx >> 5] >> (idx & 31u)) & 1u) != 0u;
}

// ---------------------------------------------------------------------------
// UF find with path splitting (wait-free for acyclic parent arrays)
// ---------------------------------------------------------------------------
__device__ __forceinline__
uint32_t uf_find_dev(uint32_t* parent, uint32_t x) {
    uint32_t p = parent[x];
    while (p != x) {
        // path splitting: point x to its grandparent
        uint32_t gp = parent[p];
        atomicCAS(&parent[x], p, gp);   // ignore return; best-effort
        x = p;
        p = parent[x];
    }
    return x;
}

// ---------------------------------------------------------------------------
// UF union by always connecting the larger root to the smaller root.
// This avoids needing a rank array and converges correctly with path splitting.
// ---------------------------------------------------------------------------
__device__ __forceinline__
void uf_union_dev(uint32_t* parent, uint32_t a, uint32_t b) {
    while (true) {
        a = uf_find_dev(parent, a);
        b = uf_find_dev(parent, b);
        if (a == b) return;
        // Canonical: smaller root becomes the representative
        if (a > b) { uint32_t t = a; a = b; b = t; }
        // Try to attach b → a
        uint32_t old = atomicCAS(&parent[b], b, a);
        if (old == b) return;   // success
        // lost the race; retry with refreshed roots
    }
}

// ---------------------------------------------------------------------------
// Compute the backward offsets array at compile time from a fixed k_sq=40.
// For generic k_sq we would need a device-side precomputation; here we bake
// in the k²=40 set and fall back to per-point inline checking for other values.
// ---------------------------------------------------------------------------

// Absolute value: int64 → uint64 without overflow
__device__ __forceinline__
uint64_t abs_i64_u64(int64_t v) {
    return v < 0 ? static_cast<uint64_t>(-(v + 1)) + 1ULL : static_cast<uint64_t>(v);
}

// Gaussian norm a²+b² using 128-bit intermediate (no overflow)
__device__ __forceinline__
uint64_t gnorm(int64_t a, int64_t b) {
    const uint64_t ua = abs_i64_u64(a);
    const uint64_t ub = abs_i64_u64(b);
    const unsigned __int128 n =
        static_cast<unsigned __int128>(ua) * ua +
        static_cast<unsigned __int128>(ub) * ub;
    return static_cast<uint64_t>(n);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// K E R N E L S
// ---------------------------------------------------------------------------

namespace gm {

// -----------------------------------------------------------------------
// K1: init_label
//   One thread per point per tile.
//   For set bits: label[i] = i (self-label, the global position index).
//   For unset bits: label[i] = UINT32_MAX (never participates).
//   We use "label" stored in the parent[] array for reuse of the same
//   device memory.  After convergence, label[i] = minimum-index prime
//   in the connected component containing i.
// -----------------------------------------------------------------------
__global__
void gpu_uf_init_kernel(
    uint32_t*       parent,   // used as label array
    const uint32_t* bitmaps,
    uint64_t        total_points,
    uint32_t        num_tiles
) {
    const uint64_t gid = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (gid >= static_cast<uint64_t>(num_tiles) * total_points) return;

    const uint64_t tile_idx  = gid / total_points;
    const uint64_t local_idx = gid % total_points;
    const uint32_t* bmap = bitmaps + tile_idx * ((total_points + 31u) / 32u);

    parent[gid] = bitmap_test_dev(bmap, local_idx)
                  ? static_cast<uint32_t>(gid)
                  : 0xFFFFFFFFu;
}

// -----------------------------------------------------------------------
// K2: label_propagation_pass
//   One thread per set bit per tile.
//   Uses atomicMin to propagate the minimum label across ALL neighbours
//   (forward + backward) within dist² ≤ k_sq.
//
//   Each pass propagates labels by one hop.  After enough passes (≥ graph
//   diameter), label[i] equals the global minimum index in the component.
//   This is equivalent to parallel BFS from all sources simultaneously.
//
//   Using ALL offsets (not just backward) is essential for correctness:
//   backward-only passes can miss paths that run forward.
// -----------------------------------------------------------------------
__global__
void gpu_uf_union_kernel(   // kept same name to avoid changing the call site
    uint32_t*       parent,   // label array
    const uint32_t* bitmaps,
    uint64_t        total_points,
    uint64_t        side_exp,
    uint64_t        k_sq,
    int64_t         collar,
    uint32_t        num_tiles
) {
    const uint64_t gid = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const uint64_t tile_idx = gid / total_points;
    if (tile_idx >= static_cast<uint64_t>(num_tiles)) return;

    const uint64_t local_idx = gid % total_points;
    const uint32_t* bmap = bitmaps + tile_idx * ((total_points + 31u) / 32u);
    if (!bitmap_test_dev(bmap, local_idx)) return;

    uint32_t* tile_parent = parent + tile_idx * total_points;
    const uint32_t my_label = tile_parent[local_idx];

    const int64_t row = static_cast<int64_t>(local_idx / side_exp);
    const int64_t col = static_cast<int64_t>(local_idx % side_exp);

    // ALL offsets (da,db) with dist² ≤ k_sq, excluding (0,0)
    const int64_t c = collar;
    for (int64_t da = -c; da <= c; ++da) {
        for (int64_t db = -c; db <= c; ++db) {
            if (da == 0 && db == 0) continue;
            const uint64_t dist_sq = static_cast<uint64_t>(da * da + db * db);
            if (dist_sq > k_sq) continue;

            const int64_t nr = row + da;
            const int64_t nc = col + db;
            if (nr < 0 || nc < 0 ||
                nr >= static_cast<int64_t>(side_exp) ||
                nc >= static_cast<int64_t>(side_exp)) continue;

            const uint64_t nidx = static_cast<uint64_t>(nr) * side_exp + static_cast<uint64_t>(nc);
            if (!bitmap_test_dev(bmap, nidx)) continue;

            // Propagate: pull the neighbour's label (atomicMin into our slot)
            const uint32_t nb_label = tile_parent[nidx];
            if (nb_label < my_label) {
                atomicMin(&tile_parent[local_idx], nb_label);
            }
        }
    }
}

// -----------------------------------------------------------------------
// K3: flatten_labels
//   One thread per set bit per tile.
//   After all propagation passes, label[i] already holds the minimum index
//   in the component (because atomicMin propagated it).  But due to async
//   updates there may still be indirect paths.  One extra chasing pass
//   replaces label[i] with label[label[i]] until stable.
//
//   Run this kernel 2× to handle two-hop indirect paths left after
//   propagation converges.
// -----------------------------------------------------------------------
__global__
void gpu_uf_compress_kernel(  // kept same name to avoid changing the call site
    uint32_t*       parent,
    const uint32_t* bitmaps,
    uint64_t        total_points,
    uint32_t        num_tiles
) {
    const uint64_t gid = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const uint64_t tile_idx = gid / total_points;
    if (tile_idx >= static_cast<uint64_t>(num_tiles)) return;

    const uint64_t local_idx = gid % total_points;
    const uint32_t* bmap = bitmaps + tile_idx * ((total_points + 31u) / 32u);
    if (!bitmap_test_dev(bmap, local_idx)) return;

    uint32_t* tile_parent = parent + tile_idx * total_points;

    // Chase: label[i] = label[label[i]] (one hop of indirection removal)
    uint32_t lbl = tile_parent[local_idx];
    while (lbl != 0xFFFFFFFFu) {
        const uint32_t next = tile_parent[lbl];
        if (next == lbl || next == 0xFFFFFFFFu) break;
        // Try to shorten: point directly to next
        atomicMin(&tile_parent[local_idx], next);
        lbl = next;
    }
}

// -----------------------------------------------------------------------
// K4: assign_comp
//   One thread per point per tile.
//   For each set bit in the INNER tile region (a_lo ≤ a ≤ a_hi, b_lo ≤ b ≤ b_hi),
//   assign a sequential component ID to each unique root using atomicCAS.
// -----------------------------------------------------------------------
__global__
void gpu_uf_assign_comp_kernel(
    const uint32_t* parent,
    uint32_t*       comp_id,       // [num_tiles * total_points], init to kNoComponent
    uint32_t*       comp_counter,  // [num_tiles], init to 0
    const uint32_t* bitmaps,
    const TileJob*  jobs,
    uint64_t        total_points,
    uint64_t        side_exp,
    uint64_t        k_sq,
    int64_t         collar,
    uint32_t        tile_side,
    uint32_t        num_tiles,
    uint32_t*       num_components,
    int32_t*        origin_component,
    int32_t*        origin_set,
    uint32_t*       num_primes
) {
    const uint64_t gid = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const uint64_t tile_idx = gid / total_points;
    if (tile_idx >= static_cast<uint64_t>(num_tiles)) return;

    const uint64_t local_idx = gid % total_points;
    const TileJob& job = jobs[tile_idx];
    const uint32_t* bmap = bitmaps + tile_idx * ((total_points + 31u) / 32u);
    if (!bitmap_test_dev(bmap, local_idx)) return;

    // Count all primes (all set bits)
    atomicAdd(&num_primes[tile_idx], 1u);

    // Check if this point is in the inner tile (a_lo..a_hi, b_lo..b_hi)
    const int64_t row = static_cast<int64_t>(local_idx / side_exp);
    const int64_t col = static_cast<int64_t>(local_idx % side_exp);
    const int64_t a = static_cast<int64_t>(job.a_lo) - collar + row;
    const int64_t b = static_cast<int64_t>(job.b_lo) - collar + col;
    const int64_t a_lo = static_cast<int64_t>(job.a_lo);
    const int64_t a_hi = a_lo + static_cast<int64_t>(tile_side);
    const int64_t b_lo = static_cast<int64_t>(job.b_lo);
    const int64_t b_hi = b_lo + static_cast<int64_t>(tile_side);

    if (a < a_lo || a > a_hi || b < b_lo || b > b_hi) return;

    // Get root (parent is already compressed after K3)
    const uint32_t* tile_parent = parent + tile_idx * total_points;
    uint32_t* tile_comp = comp_id  + tile_idx * total_points;

    const uint32_t root = tile_parent[local_idx];

    // Assign component ID to this root if not yet assigned
    uint32_t cid = tile_comp[root];
    if (cid == kNoComponent) {
        // atomicCAS: try to set comp_id[root] from kNoComponent to a fresh ID
        uint32_t new_id = atomicAdd(&comp_counter[tile_idx], 1u);
        uint32_t got    = atomicCAS(&tile_comp[root], kNoComponent, new_id);
        if (got == kNoComponent) {
            cid = new_id;
        } else {
            // Another thread won the race; use the ID it assigned.
            // The counter increment is wasted but that's harmless (IDs may be sparse).
            cid = got;
        }
    }
    // After K4, num_components = max(comp_counter[tile]) + 1;
    // We set it in a separate kernel (below) or in host code.

    // Check for origin: the prime whose gaussian norm is ≤ k_sq, set origin_component once.
    if (gnorm(a, b) <= k_sq) {
        // atomicCAS on origin_set: only the first thread to set it wins
        int32_t old_set = atomicCAS(&origin_set[tile_idx], 0, 1);
        if (old_set == 0) {
            origin_component[tile_idx] = static_cast<int32_t>(cid);
        }
    }
}

// -----------------------------------------------------------------------
// K4b: finalise_comp_count
//   Tiny kernel: one thread per tile, reads comp_counter → num_components.
//   Separated because comp_counter is updated by K4 atomics and we need
//   all K4 threads to complete before we can read the final count.
// -----------------------------------------------------------------------
__global__
void gpu_uf_finalise_count_kernel(
    const uint32_t* comp_counter,
    uint32_t*       num_components,
    uint32_t        num_tiles
) {
    const uint32_t tile_idx = static_cast<uint32_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (tile_idx >= num_tiles) return;
    num_components[tile_idx] = comp_counter[tile_idx];
}

// -----------------------------------------------------------------------
// K5: extract_faces
//   One thread per point per tile.
//   For each inner-tile set bit, emit FacePortRecord to the appropriate
//   per-face output buffer(s) using atomicAdd for the write index.
// -----------------------------------------------------------------------
__global__
void gpu_uf_extract_faces_kernel(
    const uint32_t*     parent,
    const uint32_t*     comp_id,
    const uint32_t*     bitmaps,
    const TileJob*      jobs,
    FacePortRecord*     face_inner,
    FacePortRecord*     face_outer,
    FacePortRecord*     face_left,
    FacePortRecord*     face_right,
    uint32_t*           face_counts,   // [num_tiles * 4]: inner,outer,left,right
    uint64_t            total_points,
    uint64_t            side_exp,
    int64_t             collar,
    uint32_t            tile_side,
    uint32_t            num_tiles
) {
    const uint64_t gid = static_cast<uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const uint64_t tile_idx = gid / total_points;
    if (tile_idx >= static_cast<uint64_t>(num_tiles)) return;

    const uint64_t local_idx = gid % total_points;
    const TileJob& job = jobs[tile_idx];
    const uint32_t* bmap = bitmaps + tile_idx * ((total_points + 31u) / 32u);
    if (!bitmap_test_dev(bmap, local_idx)) return;

    const int64_t row = static_cast<int64_t>(local_idx / side_exp);
    const int64_t col = static_cast<int64_t>(local_idx % side_exp);
    const int64_t a = static_cast<int64_t>(job.a_lo) - collar + row;
    const int64_t b = static_cast<int64_t>(job.b_lo) - collar + col;
    const int64_t a_lo = static_cast<int64_t>(job.a_lo);
    const int64_t a_hi = a_lo + static_cast<int64_t>(tile_side);
    const int64_t b_lo = static_cast<int64_t>(job.b_lo);
    const int64_t b_hi = b_lo + static_cast<int64_t>(tile_side);

    if (a < a_lo || a > a_hi || b < b_lo || b > b_hi) return;

    // Look up component ID
    const uint32_t* tile_parent = parent + tile_idx * total_points;
    const uint32_t* tile_comp   = comp_id + tile_idx * total_points;
    const uint32_t root = tile_parent[local_idx];
    const uint32_t cid  = tile_comp[root];
    if (cid == kNoComponent) return;  // safety guard; should not happen

    const FacePortRecord rec{
        static_cast<int32_t>(a),
        static_cast<int32_t>(b),
        cid
    };

    uint32_t* tile_counts = face_counts + tile_idx * 4u;
    const uint32_t face_cap = kMaxFacePortsPerFace;
    const uint64_t tile_face_off = tile_idx * face_cap;

    const bool is_inner = (a - a_lo) <= collar;
    const bool is_outer = (a_hi - a) <= collar;
    const bool is_left  = (b - b_lo) <= collar;
    const bool is_right = (b_hi - b) <= collar;

    if (is_inner) {
        uint32_t pos = atomicAdd(&tile_counts[0], 1u);
        if (pos < face_cap) {
            face_inner[tile_face_off + pos] = rec;
        }
    }
    if (is_outer) {
        uint32_t pos = atomicAdd(&tile_counts[1], 1u);
        if (pos < face_cap) {
            face_outer[tile_face_off + pos] = rec;
        }
    }
    if (is_left) {
        uint32_t pos = atomicAdd(&tile_counts[2], 1u);
        if (pos < face_cap) {
            face_left[tile_face_off + pos] = rec;
        }
    }
    if (is_right) {
        uint32_t pos = atomicAdd(&tile_counts[3], 1u);
        if (pos < face_cap) {
            face_right[tile_face_off + pos] = rec;
        }
    }
}

// -----------------------------------------------------------------------
// Helper: allocate or free a device + pinned-host mirror pair
// -----------------------------------------------------------------------
static cudaError_t alloc_mirror(
    void** d_ptr, void** h_ptr, size_t bytes
) {
    cudaError_t s = cudaMalloc(d_ptr, bytes);
    if (s != cudaSuccess) return s;
    s = cudaMallocHost(h_ptr, bytes);
    if (s != cudaSuccess) { cudaFree(*d_ptr); *d_ptr = nullptr; }
    return s;
}

static void free_mirror(void* d_ptr, void* h_ptr) {
    if (d_ptr) cudaFree(d_ptr);
    if (h_ptr) cudaFreeHost(h_ptr);
}

// -----------------------------------------------------------------------
// Context lifecycle
// -----------------------------------------------------------------------

cudaError_t create_gpu_uf_context(
    uint32_t batch_cap,
    uint64_t total_points,
    GpuUfContext* ctx
) {
    if (!ctx || batch_cap == 0 || total_points == 0) return cudaErrorInvalidValue;

    GpuUfContext next{};
    next.batch_capacity = batch_cap;
    next.total_points   = total_points;

    const size_t n     = static_cast<size_t>(batch_cap);
    const size_t pts   = static_cast<size_t>(total_points);
    const size_t fpcap = static_cast<size_t>(kMaxFacePortsPerFace);

    cudaError_t s;

    // UF arrays (device only)
    s = cudaMalloc(&next.d_parent,   n * pts * sizeof(uint32_t));
    if (s != cudaSuccess) goto cleanup;
    s = cudaMalloc(&next.d_comp_id,  n * pts * sizeof(uint32_t));
    if (s != cudaSuccess) goto cleanup;
    s = cudaMalloc(&next.d_comp_counter, n * sizeof(uint32_t));
    if (s != cudaSuccess) goto cleanup;

    // Face port buffers (device only — large, transferred selectively)
    s = cudaMalloc(&next.d_face_inner, n * fpcap * sizeof(FacePortRecord));
    if (s != cudaSuccess) goto cleanup;
    s = cudaMalloc(&next.d_face_outer, n * fpcap * sizeof(FacePortRecord));
    if (s != cudaSuccess) goto cleanup;
    s = cudaMalloc(&next.d_face_left,  n * fpcap * sizeof(FacePortRecord));
    if (s != cudaSuccess) goto cleanup;
    s = cudaMalloc(&next.d_face_right, n * fpcap * sizeof(FacePortRecord));
    if (s != cudaSuccess) goto cleanup;

    // Summary scalars: device + pinned-host mirrors
    s = alloc_mirror(
            reinterpret_cast<void**>(&next.d_face_counts),
            reinterpret_cast<void**>(&next.h_face_counts),
            n * 4 * sizeof(uint32_t));
    if (s != cudaSuccess) goto cleanup;

    s = alloc_mirror(
            reinterpret_cast<void**>(&next.d_num_components),
            reinterpret_cast<void**>(&next.h_num_components),
            n * sizeof(uint32_t));
    if (s != cudaSuccess) goto cleanup;

    s = alloc_mirror(
            reinterpret_cast<void**>(&next.d_origin_component),
            reinterpret_cast<void**>(&next.h_origin_component),
            n * sizeof(int32_t));
    if (s != cudaSuccess) goto cleanup;

    s = alloc_mirror(
            reinterpret_cast<void**>(&next.d_num_primes),
            reinterpret_cast<void**>(&next.h_num_primes),
            n * sizeof(uint32_t));
    if (s != cudaSuccess) goto cleanup;

    // origin_set: device only (bool flag per tile)
    s = cudaMalloc(&next.d_origin_set, n * sizeof(int32_t));
    if (s != cudaSuccess) goto cleanup;

    // Face port host mirrors
    s = cudaMallocHost(&next.h_face_inner, n * fpcap * sizeof(FacePortRecord));
    if (s != cudaSuccess) goto cleanup;
    s = cudaMallocHost(&next.h_face_outer, n * fpcap * sizeof(FacePortRecord));
    if (s != cudaSuccess) goto cleanup;
    s = cudaMallocHost(&next.h_face_left,  n * fpcap * sizeof(FacePortRecord));
    if (s != cudaSuccess) goto cleanup;
    s = cudaMallocHost(&next.h_face_right, n * fpcap * sizeof(FacePortRecord));
    if (s != cudaSuccess) goto cleanup;

    *ctx = next;
    return cudaSuccess;

cleanup:
    // Partial allocation — free whatever was set
    {
        GpuUfContext tmp = next;
        destroy_gpu_uf_context(&tmp);
    }
    return s;
}

void destroy_gpu_uf_context(GpuUfContext* ctx) {
    if (!ctx) return;

    if (ctx->d_parent)       cudaFree(ctx->d_parent);
    if (ctx->d_comp_id)      cudaFree(ctx->d_comp_id);
    if (ctx->d_comp_counter) cudaFree(ctx->d_comp_counter);
    if (ctx->d_face_inner)   cudaFree(ctx->d_face_inner);
    if (ctx->d_face_outer)   cudaFree(ctx->d_face_outer);
    if (ctx->d_face_left)    cudaFree(ctx->d_face_left);
    if (ctx->d_face_right)   cudaFree(ctx->d_face_right);
    if (ctx->d_origin_set)   cudaFree(ctx->d_origin_set);

    free_mirror(ctx->d_face_counts,      ctx->h_face_counts);
    free_mirror(ctx->d_num_components,   ctx->h_num_components);
    free_mirror(ctx->d_origin_component, ctx->h_origin_component);
    free_mirror(ctx->d_num_primes,       ctx->h_num_primes);

    if (ctx->h_face_inner)   cudaFreeHost(ctx->h_face_inner);
    if (ctx->h_face_outer)   cudaFreeHost(ctx->h_face_outer);
    if (ctx->h_face_left)    cudaFreeHost(ctx->h_face_left);
    if (ctx->h_face_right)   cudaFreeHost(ctx->h_face_right);

    *ctx = GpuUfContext{};
}

// -----------------------------------------------------------------------
// run_gpu_uf
// -----------------------------------------------------------------------

cudaError_t run_gpu_uf(
    GpuUfContext&   ctx,
    const uint32_t* d_bitmaps,
    const TileJob*  d_jobs,
    uint32_t        num_tiles,
    uint32_t        tile_side,
    int64_t         collar,
    uint64_t        k_sq,
    uint64_t        side_exp,
    size_t          bitmap_words
) {
    if (num_tiles == 0 || num_tiles > ctx.batch_capacity) return cudaErrorInvalidValue;

    const uint64_t total_points = ctx.total_points;
    const size_t   n            = static_cast<size_t>(num_tiles);
    const size_t   fpcap        = static_cast<size_t>(kMaxFacePortsPerFace);
    const uint32_t block        = 256u;

    cudaError_t s;

    // -- Zeroise mutable device state -----------------------------------
    s = cudaMemset(ctx.d_comp_id,      0xFF, n * static_cast<size_t>(total_points) * sizeof(uint32_t)); // kNoComponent
    if (s != cudaSuccess) return s;
    s = cudaMemset(ctx.d_comp_counter, 0,    n * sizeof(uint32_t));
    if (s != cudaSuccess) return s;
    s = cudaMemset(ctx.d_face_counts,  0,    n * 4 * sizeof(uint32_t));
    if (s != cudaSuccess) return s;
    s = cudaMemset(ctx.d_num_components,  0, n * sizeof(uint32_t));
    if (s != cudaSuccess) return s;
    s = cudaMemset(ctx.d_num_primes,   0,    n * sizeof(uint32_t));
    if (s != cudaSuccess) return s;
    s = cudaMemset(ctx.d_origin_set,   0,    n * sizeof(int32_t));
    if (s != cudaSuccess) return s;
    // origin_component: init to -1 (no origin found)
    s = cudaMemset(ctx.d_origin_component, 0xFF, n * sizeof(int32_t));  // 0xFF..FF == -1 as two's-complement
    if (s != cudaSuccess) return s;

    // -- K1: init_parent ------------------------------------------------
    const uint64_t total_work  = static_cast<uint64_t>(num_tiles) * total_points;
    const uint32_t grid_init   = static_cast<uint32_t>((total_work + block - 1) / block);

    gpu_uf_init_kernel<<<grid_init, block>>>(
        ctx.d_parent, d_bitmaps, total_points, num_tiles);
    s = cudaGetLastError();
    if (s != cudaSuccess) return s;

    // -- K2+K3: union rounds (union_pass × N + compress) × M_rounds ------
    // Dynamic pass count: worst case = component spans full tile diagonal.
    // Each pass propagates labels by one hop of ≤ sqrt(k_sq) cells.
    // passes_needed = ceil(tile_side * sqrt(2) / sqrt(k_sq)) + safety margin.
    const uint32_t sqrt_k = static_cast<uint32_t>(ceil(sqrt(static_cast<double>(k_sq))));
    const uint32_t min_passes = (sqrt_k > 0) ? (tile_side * 3u / (sqrt_k * 2u)) + 15u : 100u;
    const uint32_t num_rounds = 3u;
    const uint32_t passes_per_round = (min_passes + num_rounds - 1u) / num_rounds;

    const uint32_t grid_union = grid_init;
    for (uint32_t round = 0; round < num_rounds; ++round) {
        for (uint32_t pass = 0; pass < passes_per_round; ++pass) {
            gpu_uf_union_kernel<<<grid_union, block>>>(
                ctx.d_parent,
                d_bitmaps,
                total_points,
                side_exp,
                k_sq,
                collar,
                num_tiles);
            s = cudaGetLastError();
            if (s != cudaSuccess) return s;
        }
        // Compress after each round to shorten parent chains
        gpu_uf_compress_kernel<<<grid_init, block>>>(
            ctx.d_parent, d_bitmaps, total_points, num_tiles);
        s = cudaGetLastError();
        if (s != cudaSuccess) return s;
    }
    // Final compress pass to fully flatten any remaining indirections
    gpu_uf_compress_kernel<<<grid_init, block>>>(
        ctx.d_parent, d_bitmaps, total_points, num_tiles);
    s = cudaGetLastError();
    if (s != cudaSuccess) return s;

    // -- K4: assign_comp ------------------------------------------------
    gpu_uf_assign_comp_kernel<<<grid_init, block>>>(
        ctx.d_parent,
        ctx.d_comp_id,
        ctx.d_comp_counter,
        d_bitmaps,
        d_jobs,
        total_points,
        side_exp,
        k_sq,
        collar,
        tile_side,
        num_tiles,
        ctx.d_num_components,
        ctx.d_origin_component,
        ctx.d_origin_set,
        ctx.d_num_primes);
    s = cudaGetLastError();
    if (s != cudaSuccess) return s;

    // -- K4b: finalise component count ----------------------------------
    const uint32_t grid_fin = static_cast<uint32_t>((num_tiles + block - 1) / block);
    gpu_uf_finalise_count_kernel<<<grid_fin, block>>>(
        ctx.d_comp_counter, ctx.d_num_components, num_tiles);
    s = cudaGetLastError();
    if (s != cudaSuccess) return s;

    // -- K5: extract faces ----------------------------------------------
    gpu_uf_extract_faces_kernel<<<grid_init, block>>>(
        ctx.d_parent,
        ctx.d_comp_id,
        d_bitmaps,
        d_jobs,
        ctx.d_face_inner,
        ctx.d_face_outer,
        ctx.d_face_left,
        ctx.d_face_right,
        ctx.d_face_counts,
        total_points,
        side_exp,
        collar,
        tile_side,
        num_tiles);
    s = cudaGetLastError();
    if (s != cudaSuccess) return s;

    // -- Synchronise and transfer results to host -----------------------
    s = cudaDeviceSynchronize();
    if (s != cudaSuccess) return s;

    // Scalars
    s = cudaMemcpy(ctx.h_face_counts,      ctx.d_face_counts,
                   n * 4 * sizeof(uint32_t), cudaMemcpyDeviceToHost);
    if (s != cudaSuccess) return s;
    s = cudaMemcpy(ctx.h_num_components,   ctx.d_num_components,
                   n * sizeof(uint32_t), cudaMemcpyDeviceToHost);
    if (s != cudaSuccess) return s;
    s = cudaMemcpy(ctx.h_origin_component, ctx.d_origin_component,
                   n * sizeof(int32_t), cudaMemcpyDeviceToHost);
    if (s != cudaSuccess) return s;
    s = cudaMemcpy(ctx.h_num_primes,       ctx.d_num_primes,
                   n * sizeof(uint32_t), cudaMemcpyDeviceToHost);
    if (s != cudaSuccess) return s;

    // Face port arrays: transfer only the filled portion per tile
    for (uint32_t i = 0; i < num_tiles; ++i) {
        const uint32_t* fc   = ctx.h_face_counts + i * 4u;
        const uint64_t  off  = static_cast<uint64_t>(i) * fpcap;

        if (fc[0] > 0) {
            s = cudaMemcpy(ctx.h_face_inner + off, ctx.d_face_inner + off,
                           static_cast<size_t>(fc[0]) * sizeof(FacePortRecord),
                           cudaMemcpyDeviceToHost);
            if (s != cudaSuccess) return s;
        }
        if (fc[1] > 0) {
            s = cudaMemcpy(ctx.h_face_outer + off, ctx.d_face_outer + off,
                           static_cast<size_t>(fc[1]) * sizeof(FacePortRecord),
                           cudaMemcpyDeviceToHost);
            if (s != cudaSuccess) return s;
        }
        if (fc[2] > 0) {
            s = cudaMemcpy(ctx.h_face_left + off, ctx.d_face_left + off,
                           static_cast<size_t>(fc[2]) * sizeof(FacePortRecord),
                           cudaMemcpyDeviceToHost);
            if (s != cudaSuccess) return s;
        }
        if (fc[3] > 0) {
            s = cudaMemcpy(ctx.h_face_right + off, ctx.d_face_right + off,
                           static_cast<size_t>(fc[3]) * sizeof(FacePortRecord),
                           cudaMemcpyDeviceToHost);
            if (s != cudaSuccess) return s;
        }
    }

    return cudaSuccess;
}

} // namespace gm
