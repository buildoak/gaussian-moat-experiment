#ifndef GM_GPU_UF_CUH
#define GM_GPU_UF_CUH

#include <cstdint>
#include <cuda_runtime.h>

#include "face_port_io.h"
#include "types.h"
// NOTE: face_extract.cuh (which defines TileFacePorts) is intentionally NOT
// included here, because it pulls in tile_kernel.cuh which defines non-inline
// __global__ kernels.  Including it from two translation units would cause
// linker duplicate-symbol errors.  fat_stripe_cuda.cu includes face_extract.cuh
// directly; the tile_face_ports_from_gpu_uf helper is declared there.

// GPU Union-Find for connected component labelling on device bitmaps.
//
// Replaces the CPU path in fat_stripe_cuda.cu:
//   transfer_batch_bitmaps()  +  for-each-tile extract_face_ports()
// with a GPU-side computation that leaves bitmaps on device and only
// transfers the much smaller FacePortRecord arrays back to the host.
//
// Design
// ------
// For k²=40, tile_side=256: collar=7, side_exp=271, total_points=73441.
// A single 256-thread block cannot hold the full UF table in shared memory,
// so we use flat global-memory atomics across a multi-block launch.
//
// Five-kernel pipeline per batch:
//   K1: init_parent   – set parent[i] = i for every point
//   K2: union_pass    – for each set bit, union with backward neighbours
//                       (repeated kNumUnionPasses times to reach convergence)
//   K3: compress      – flatten all parent chains (one full-path compression pass)
//   K4: assign_comp   – assign sequential component IDs to unique roots
//   K5: extract_faces – emit FacePortRecord to per-face output buffers

namespace gm {

// Conservative upper bound on face ports per face per tile.
// For k²=40 collar=7: each face strip is 7 × side_exp cells, ~1.2% are prime,
// so ≤ ceil(7 × 271 × 0.012) ≈ 23 per face.  2048 gives a 90× safety margin.
constexpr uint32_t kMaxFacePortsPerFace = 2048;

// Number of union passes per round.
// Worst case: a connected component spans the full tile width (256 cells).
// Each union pass propagates connectivity by one hop of ≤ sqrt(k_sq) ≈ 6.3 cells.
// For a 256-cell wide tile: ⌈256/6.3⌉ ≈ 41 passes needed.
// We run 3 rounds of (union × 25 passes + compress) = 75 union passes total,
// which is conservative enough for any tile_side ≤ 512 with k²=40.
constexpr uint32_t kNumUnionPassesPerRound = 25;
constexpr uint32_t kNumRounds = 3;

// Sentinel used during component-ID assignment: means "not yet claimed".
constexpr uint32_t kNoComponent = 0xFFFFFFFFu;

// -----------------------------------------------------------------------
// Per-batch GPU workspace
// -----------------------------------------------------------------------
struct GpuUfContext {
    // Device allocations – sized [batch_cap * total_points]
    uint32_t* d_parent       = nullptr; // UF parent array
    uint32_t* d_comp_id      = nullptr; // component ID per point (kNoComponent initially)
    uint32_t* d_comp_counter = nullptr; // per-tile atomic counters for component IDs [batch_cap]
    uint8_t*  d_rank         = nullptr; // UF rank array [batch_cap * total_points]

    // Per-face output buffers – sized [batch_cap * kMaxFacePortsPerFace]
    FacePortRecord* d_face_inner = nullptr;
    FacePortRecord* d_face_outer = nullptr;
    FacePortRecord* d_face_left  = nullptr;
    FacePortRecord* d_face_right = nullptr;

    // Per-tile summary scalars – sized [batch_cap]
    uint32_t* d_face_counts      = nullptr; // [batch_cap * 4]: inner,outer,left,right
    uint32_t* d_num_components   = nullptr;
    int32_t*  d_origin_component = nullptr;
    uint32_t* d_num_primes       = nullptr;
    int32_t*  d_origin_set       = nullptr; // boolean flag: has origin been set for this tile?

    // Pinned host mirrors for result download
    FacePortRecord* h_face_inner = nullptr;
    FacePortRecord* h_face_outer = nullptr;
    FacePortRecord* h_face_left  = nullptr;
    FacePortRecord* h_face_right = nullptr;

    uint32_t* h_face_counts      = nullptr;
    uint32_t* h_num_components   = nullptr;
    int32_t*  h_origin_component = nullptr;
    uint32_t* h_num_primes       = nullptr;

    uint32_t batch_capacity = 0;
    uint64_t total_points   = 0;
};

// -----------------------------------------------------------------------
// Context lifecycle
// -----------------------------------------------------------------------

cudaError_t create_gpu_uf_context(
    uint32_t batch_cap,
    uint64_t total_points,
    GpuUfContext* ctx
);

void destroy_gpu_uf_context(GpuUfContext* ctx);

// -----------------------------------------------------------------------
// Main entry point
// -----------------------------------------------------------------------

// Run GPU UF on the device bitmaps already populated by launch_batch_sieve().
// On return, all h_* fields of ctx are valid for tile indices [0, num_tiles).
// Does NOT call cudaDeviceSynchronize at the end — caller is responsible.
cudaError_t run_gpu_uf(
    GpuUfContext&   ctx,
    const uint32_t* d_bitmaps,  // from BatchContext::d_bitmaps
    const TileJob*  d_jobs,     // from BatchContext::d_jobs (device pointer)
    uint32_t        num_tiles,
    uint32_t        tile_side,
    int64_t         collar,
    uint64_t        k_sq,
    uint64_t        side_exp,
    size_t          bitmap_words
);

// NOTE: tile_face_ports_from_gpu_uf() is defined as a static helper in
// fat_stripe_cuda.cu (the only caller) to avoid pulling face_extract.cuh
// (and thus tile_kernel.cuh) into this translation unit.

} // namespace gm

#endif // GM_GPU_UF_CUH
