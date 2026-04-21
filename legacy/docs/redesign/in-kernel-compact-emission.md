# In-Kernel Compact Port Emission for `gpu_uf_tile_kernel`

## Scope

This document replaces the post-UF extraction idea from [docs/redesign/two-phase-merge-plan.md](/Users/otonashi/thinking/building/gaussian-moat-cuda/docs/redesign/two-phase-merge-plan.md). The new design keeps the union-find work exactly where it belongs, inside `gpu_uf_tile_kernel()`, and emits the compact merge payload before the tile-local UF state goes cold.

The relevant checked-in code today is:

- `src/gpu_uf.cu:275-552` for the tile kernel and current face-port emission
- `src/gpu_uf.cu:584-944` for GPU UF allocation and D2H staging
- `src/gpu_uf.cuh:38-91` for caps and `GpuUfContext`
- `src/fat_stripe_cuda.cu:328-354` for `tile_face_ports_from_gpu_uf()`
- `src/fat_stripe_cuda.cu:356-583` for the current host-side merge materialization
- `src/fat_stripe_cuda.cu:629-867` for the current seam merge kernels and host finalization
- `tile-probe/crates/moat-kernel/src/compose.rs:99-177` and `tile-probe/crates/moat-kernel/src/compose.rs:236-314` for the Rust face-retention semantics
- `docs/bugs/gpu-merge-false-connectivity.md` for the exposed-face scoping rule that must be preserved

The core changes are:

1. UF arrays become `[batch_cap * kMaxPrimesPerTile]`, not `[batch_cap * total_points]`.
2. `kMaxFacePortsPerFace` drops from `2048` to `256`.
3. `gpu_uf_tile_kernel()` emits one compact per-tile record directly into a fixed-slot device buffer.
4. The host repacks those fixed slots into a packed compact blob and performs the manifest-order component-base prefix as it streams batches.
5. Phase 2 merges only packed compact records plus a merge UF sized by total global components.

## 1. Struct Definitions

Put these in `src/gpu_uf.cuh` so both `gpu_uf.cu` and `fat_stripe_cuda.cu` share the same layout. `kMaxPrimesPerTile` must move out of the anonymous namespace in `src/gpu_uf.cu:22-27` for the same reason.

```cpp
namespace gm {

constexpr uint32_t kMaxPrimesPerTile = 8192u;
constexpr uint32_t kMaxFacePortsPerFace = 256u;
constexpr uint32_t kMaxCompactPortsPerTile = 4u * kMaxFacePortsPerFace;  // 1024

struct alignas(4) CompactPortRecord {
    uint16_t x;        // a - tile.a_lo, nominal coordinate, 0..tile_side
    uint16_t y;        // b - tile.b_lo, nominal coordinate, 0..tile_side
    uint32_t comp_id;  // phase-2/globalized value after host prefix fixup
};
static_assert(sizeof(CompactPortRecord) == 8, "CompactPortRecord size mismatch");
static_assert(alignof(CompactPortRecord) == 4, "CompactPortRecord alignment mismatch");

struct alignas(4) CompactTileResult {
    uint32_t tile_idx;        // manifest index, not batch-local index
    uint32_t num_components;  // <= kMaxPrimesPerTile
    uint32_t num_ports;       // unique boundary primes, 0..kMaxCompactPortsPerTile
    uint32_t component_base;  // exclusive prefix over prior tiles, host-patched after batch D2H
    // Followed by:
    //   uint8_t face_bits[num_components];
    //   uint8_t face_bits_pad[(4 - (num_components & 3)) & 3];
    //   CompactPortRecord ports[num_ports];
};
static_assert(sizeof(CompactTileResult) == 16, "CompactTileResult size mismatch");
static_assert(alignof(CompactTileResult) == 4, "CompactTileResult alignment mismatch");

enum CompactTileStatus : uint32_t {
    kCompactOk             = 0u,
    kCompactPrimeOverflow  = 1u << 0,  // num_primes > kMaxPrimesPerTile
    kCompactInnerOverflow  = 1u << 1,  // per-face count > kMaxFacePortsPerFace
    kCompactOuterOverflow  = 1u << 2,
    kCompactLeftOverflow   = 1u << 3,
    kCompactRightOverflow  = 1u << 4,
    kCompactBytesOverflow  = 1u << 5,  // encoded bytes > kMaxCompactTileBytes
};

struct alignas(4) TileOrigin {
    int32_t a_lo;
    int32_t b_lo;
};
static_assert(sizeof(TileOrigin) == 8, "TileOrigin size mismatch");

constexpr uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

constexpr uint32_t compact_face_bytes(uint32_t num_components) {
    return num_components;
}

constexpr uint32_t compact_face_padded_bytes(uint32_t num_components) {
    return align_up_u32(compact_face_bytes(num_components), alignof(CompactPortRecord));
}

constexpr uint32_t compact_tile_bytes(uint32_t num_components, uint32_t num_ports) {
    return sizeof(CompactTileResult) +
           compact_face_padded_bytes(num_components) +
           num_ports * sizeof(CompactPortRecord);
}

constexpr uint32_t kMaxCompactTileBytes =
    compact_tile_bytes(kMaxPrimesPerTile, kMaxCompactPortsPerTile);
static_assert(kMaxCompactTileBytes == 16400u, "Compact tile slot size mismatch");

inline uint8_t* compact_face_bits(CompactTileResult* tile) {
    return reinterpret_cast<uint8_t*>(tile) + sizeof(CompactTileResult);
}

inline const uint8_t* compact_face_bits(const CompactTileResult* tile) {
    return reinterpret_cast<const uint8_t*>(tile) + sizeof(CompactTileResult);
}

inline CompactPortRecord* compact_ports(CompactTileResult* tile) {
    return reinterpret_cast<CompactPortRecord*>(
        reinterpret_cast<uint8_t*>(tile) +
        sizeof(CompactTileResult) +
        compact_face_padded_bytes(tile->num_components));
}

inline const CompactPortRecord* compact_ports(const CompactTileResult* tile) {
    return reinterpret_cast<const CompactPortRecord*>(
        reinterpret_cast<const uint8_t*>(tile) +
        sizeof(CompactTileResult) +
        compact_face_padded_bytes(tile->num_components));
}

} // namespace gm
```

### Byte layout

- `CompactPortRecord`: `2 + 2 + 4 = 8` bytes, 4-byte aligned.
- `CompactTileResult`: `4 + 4 + 4 + 4 = 16` bytes, 4-byte aligned.
- Encoded tile bytes:

```text
bytes(tile) = 16 + align_up(num_components, 4) + 8 * num_ports
```

- Empty tile: `16` bytes.
- Typical tile target at `R≈80M`: `~620` bytes retained after repack.
- Dense tile target near origin: `~2 KiB` retained after repack.
- Exact fixed device slot for one-kernel emission:

```text
kMaxCompactTileBytes
= 16 + align_up(8192, 4) + 8 * (4 * 256)
= 16 + 8192 + 8192
= 16,400 bytes
```

### Important implementation note about `comp_id`

The persisted phase-2 format uses global component IDs in `CompactPortRecord::comp_id`, exactly as above.

The kernel cannot know `component_base` yet. Therefore:

1. `gpu_uf_tile_kernel()` writes tile-local `local_comp_id` into `comp_id`.
2. The host computes the manifest-order prefix after each batch D2H.
3. The host patches `tile.component_base` and adds that base to every `ports[p].comp_id` before appending the tile into the packed retained blob.

This is still one UF kernel and one compact emission pass. The rejected part is the second GPU extraction kernel, not a host-side `+component_base` over already compact bytes.

## 2. Kernel Modifications

### 2.1 UF stride fix

The current kernel slices the UF arrays using `total_points` at `src/gpu_uf.cu:303-306`:

```cpp
uint32_t* g_parent    = d_g_parent    + tile_idx * total_points;
uint32_t* g_root_comp = d_g_root_comp + tile_idx * total_points;
uint8_t*  g_rank      = d_g_rank      + tile_idx * total_points;
```

Change that to:

```cpp
const uint32_t batch_tile_idx = blockIdx.x;
uint32_t* g_parent    = d_g_parent    + static_cast<size_t>(batch_tile_idx) * gm::kMaxPrimesPerTile;
uint32_t* g_root_comp = d_g_root_comp + static_cast<size_t>(batch_tile_idx) * gm::kMaxPrimesPerTile;
uint8_t*  g_rank      = d_g_rank      + static_cast<size_t>(batch_tile_idx) * gm::kMaxPrimesPerTile;
```

`total_points` still stays in the signature because it is required for bitmap traversal and row/column math.

### 2.2 New kernel parameters

Replace the current face-buffer outputs at `src/gpu_uf.cu:288-295` with:

```cpp
uint32_t        tile_base,                // manifest index of batch tile 0
const uint8_t*  __restrict__ d_exposed_face_masks, // one byte per tile in batch
uint8_t*        __restrict__ d_compact_slots,      // raw bytes [batch_cap * kMaxCompactTileBytes]
uint32_t        compact_stride_bytes,     // = kMaxCompactTileBytes
uint32_t*       __restrict__ d_compact_bytes_used, // [batch_cap]
uint32_t*       __restrict__ d_compact_status,     // [batch_cap]
uint32_t*       __restrict__ d_num_primes,
int32_t*        __restrict__ d_origin_component     // keep until legacy stream path is retired
```

Remove:

- `FacePortRecord* d_face_inner`
- `FacePortRecord* d_face_outer`
- `FacePortRecord* d_face_left`
- `FacePortRecord* d_face_right`
- `uint32_t* d_face_counts`
- `uint32_t* d_num_components`

### 2.3 Exposed-face scoping

The bug fix in [docs/bugs/gpu-merge-false-connectivity.md](/Users/otonashi/thinking/building/gaussian-moat-cuda/docs/bugs/gpu-merge-false-connectivity.md) must survive this rewrite.

Do not let the kernel invent campaign-boundary bits from tile-local seam faces. The host precomputes `exposed_face_mask[tile]` from the full manifest, using the same topology logic that currently lives in `src/fat_stripe_cuda.cu:438-581`, and passes that byte into the kernel.

Inside the kernel:

- `touch_mask` means which tile-local faces a prime lies on.
- `touch_mask & exposed_face_mask` is what gets OR-ed into `face_bits[local_comp]`.
- The compact port list still contains all boundary primes on all four tile faces, exposed or not, because internal seam ports are still needed for cross-tile union.

This matches the Rust composition behavior in:

- `tile-probe/crates/moat-kernel/src/compose.rs:162-177`
- `tile-probe/crates/moat-kernel/src/compose.rs:261-314`

### 2.4 Replace the current phase-8 face-port emission

The current thread-0 block at `src/gpu_uf.cu:468-552` assigns local component IDs and writes four `FacePortRecord` arrays with silent per-face saturation. Replace it with three subphases:

#### Phase 8A: thread 0 assigns local component IDs, counts boundary primes, and writes face bits

Pseudocode:

```cpp
if (tid == 0u) {
    const uint32_t global_tile_idx = tile_base + batch_tile_idx;
    const uint8_t exposed_mask = d_exposed_face_masks[batch_tile_idx];
    uint8_t* tile_slot_bytes =
        d_compact_slots + static_cast<size_t>(batch_tile_idx) * compact_stride_bytes;
    auto* tile_out = reinterpret_cast<gm::CompactTileResult*>(tile_slot_bytes);

    tile_out->tile_idx = global_tile_idx;
    tile_out->num_components = 0u;
    tile_out->num_ports = 0u;
    tile_out->component_base = 0u;  // host patches later

    for (uint32_t c = 0; c < gm::kMaxPrimesPerTile; ++c) {
        gm::compact_face_bits(tile_out)[c] = 0u;
    }

    uint32_t status = gm::kCompactOk;
    uint32_t inner_count = 0u;
    uint32_t outer_count = 0u;
    uint32_t left_count = 0u;
    uint32_t right_count = 0u;

    for (uint32_t i = 0; i < scalars.num_primes; ++i) {
        const uint32_t pos = shared.prime_pos[i];
        const int64_t row = pos / side_exp;
        const int64_t col = pos % side_exp;
        const int64_t a = expanded_a_lo + row;
        const int64_t b = expanded_b_lo + col;

        if (!in_nominal_bounds(a, b, a_lo, a_hi, b_lo, b_hi)) {
            continue;
        }

        const uint32_t root = g_parent[i];
        uint32_t local_comp = g_root_comp[root];
        if (local_comp == gm::kNoComponent) {
            local_comp = tile_out->num_components++;
            g_root_comp[root] = local_comp;
        }

        const uint16_t x = static_cast<uint16_t>(a - a_lo);
        const uint16_t y = static_cast<uint16_t>(b - b_lo);

        uint8_t touch_mask = 0u;
        if (x <= collar)            { touch_mask |= gm::kFaceInnerBit; ++inner_count; }
        if (x >= side - collar)     { touch_mask |= gm::kFaceOuterBit; ++outer_count; }
        if (y <= collar)            { touch_mask |= gm::kFaceLeftBit;  ++left_count;  }
        if (y >= side - collar)     { touch_mask |= gm::kFaceRightBit; ++right_count; }

        if (touch_mask != 0u) {
            ++tile_out->num_ports;
            gm::compact_face_bits(tile_out)[local_comp] |= (touch_mask & exposed_mask);
        }
    }

    if (inner_count > gm::kMaxFacePortsPerFace) status |= gm::kCompactInnerOverflow;
    if (outer_count > gm::kMaxFacePortsPerFace) status |= gm::kCompactOuterOverflow;
    if (left_count  > gm::kMaxFacePortsPerFace) status |= gm::kCompactLeftOverflow;
    if (right_count > gm::kMaxFacePortsPerFace) status |= gm::kCompactRightOverflow;
    if (tile_out->num_ports > gm::kMaxCompactPortsPerTile) {
        status |= gm::kCompactBytesOverflow;
    }

    if (tile_contains_origin && scalars.origin_anchor != kInvalidPrime) {
        const uint32_t origin_root = g_parent[scalars.origin_anchor];
        const uint32_t origin_local_comp = g_root_comp[origin_root];
        if (origin_local_comp != gm::kNoComponent) {
            scalars.origin_component = static_cast<int32_t>(origin_local_comp);
        }
    }

    const uint32_t used_bytes =
        gm::compact_tile_bytes(tile_out->num_components, tile_out->num_ports);
    if (used_bytes > compact_stride_bytes) {
        status |= gm::kCompactBytesOverflow;
    }

    d_compact_bytes_used[batch_tile_idx] = used_bytes;
    d_compact_status[batch_tile_idx] = status;
    d_num_primes[batch_tile_idx] = scalars.num_primes;
    d_origin_component[batch_tile_idx] = scalars.origin_component;
}
__syncthreads();
```

Notes:

- Zero the full fixed face-bit region once per tile before the loop.
- Do not silently clamp counts. Any overflow flag is fatal on the host.
- `num_ports` counts unique boundary primes, not duplicated per-face emissions.

#### Phase 8B: warp-cooperative port write

This replaces the four face-array appends at `src/gpu_uf.cu:497-531`. The write order should remain row-major over `shared.prime_pos[]`, so batch results stay deterministic.

Pseudocode:

```cpp
auto* tile_out = reinterpret_cast<gm::CompactTileResult*>(
    d_compact_slots + static_cast<size_t>(batch_tile_idx) * compact_stride_bytes);
gm::CompactPortRecord* ports = gm::compact_ports(tile_out);

__shared__ uint32_t block_write_base;
for (uint32_t base = 0; base < scalars.num_primes; base += blockDim.x) {
    const uint32_t i = base + tid;

    bool emit = false;
    gm::CompactPortRecord record{};

    if (i < scalars.num_primes) {
        const uint32_t pos = shared.prime_pos[i];
        const int64_t row = pos / side_exp;
        const int64_t col = pos % side_exp;
        const int64_t a = expanded_a_lo + row;
        const int64_t b = expanded_b_lo + col;

        if (in_nominal_bounds(a, b, a_lo, a_hi, b_lo, b_hi)) {
            const uint16_t x = static_cast<uint16_t>(a - a_lo);
            const uint16_t y = static_cast<uint16_t>(b - b_lo);
            const bool on_boundary =
                (x <= collar) || (x >= side - collar) ||
                (y <= collar) || (y >= side - collar);

            if (on_boundary) {
                const uint32_t root = g_parent[i];
                const uint32_t local_comp = g_root_comp[root];
                record = gm::CompactPortRecord{x, y, local_comp};
                emit = true;
            }
        }
    }

    const uint32_t lane = tid & 31u;
    const uint32_t warp = tid >> 5;
    const uint32_t mask = __ballot_sync(0xFFFFFFFFu, emit);
    const uint32_t warp_count = __popc(mask);
    const uint32_t warp_rank = __popc(mask & ((1u << lane) - 1u));

    __shared__ uint32_t warp_counts[8];
    __shared__ uint32_t warp_offsets[8];
    if (lane == 0u) {
        warp_counts[warp] = warp_count;
    }
    __syncthreads();

    if (warp == 0u) {
        uint32_t sum = 0u;
        if (lane < 8u) {
            const uint32_t count = warp_counts[lane];
            warp_offsets[lane] = sum;
            sum += count;
        }
        if (lane == 0u) {
            block_write_base = atomicAdd(&scalars.num_ports_written, sum);
        }
    }
    __syncthreads();

    const uint32_t tile_offset = block_write_base + warp_offsets[warp] + warp_rank;
    if (emit && tile_offset < gm::kMaxCompactPortsPerTile) {
        ports[tile_offset] = record;
    }
    __syncthreads();
}
```

Implementation notes:

- Add `uint32_t num_ports_written` to `TileSharedScalars` and initialize it to `0` before phase 8B.
- This writes only the compact port array. It does not write face bits.
- Because `base` increments monotonically, the final order is stable row-major.
- The `tile_offset < gm::kMaxCompactPortsPerTile` guard is only to prevent OOB writes. Any tile that hits it has already set an overflow bit and is a hard host-side failure, not a tolerated truncation.

#### Phase 8C: finalize empty/overflow cases

Update `write_empty_tile()` at `src/gpu_uf.cu:255-273` to write a compact-header-only tile instead of face counts. For any nonzero `d_compact_status[tile]`, the host aborts phase 1 and reports the tile index plus the specific flag bits.

## 3. Allocation Changes

### 3.1 `GpuUfContext` changes

Today `GpuUfContext` in `src/gpu_uf.cuh:58-91` holds:

- full per-prime UF arrays sized by `total_points`
- four device face buffers
- four pinned face mirrors
- per-face counts
- two dead fields: `d_comp_counter`, `d_origin_set`

Replace it with:

```cpp
struct GpuUfContext {
    uint32_t* d_parent = nullptr;     // [batch_cap * kMaxPrimesPerTile]
    uint32_t* d_comp_id = nullptr;    // [batch_cap * kMaxPrimesPerTile]
    uint8_t*  d_rank = nullptr;       // [batch_cap * kMaxPrimesPerTile]

    uint8_t*  d_compact_slots = nullptr;      // [batch_cap * kMaxCompactTileBytes]
    uint32_t* d_compact_bytes_used = nullptr; // [batch_cap]
    uint32_t* d_compact_status = nullptr;     // [batch_cap]
    uint32_t* d_num_primes = nullptr;         // [batch_cap]
    int32_t*  d_origin_component = nullptr;   // [batch_cap], keep until legacy stream path is deleted

    uint8_t*  h_compact_slots = nullptr;      // pinned mirror of used slots
    uint32_t* h_compact_bytes_used = nullptr; // pinned
    uint32_t* h_compact_status = nullptr;     // pinned
    uint32_t* h_num_primes = nullptr;         // pinned
    int32_t*  h_origin_component = nullptr;   // pinned

    uint32_t batch_capacity = 0;
    uint64_t total_points = 0;
};
```

Delete:

- `d_comp_counter`
- `d_face_inner`, `d_face_outer`, `d_face_left`, `d_face_right`
- `d_face_counts`
- `d_num_components`
- `d_origin_set`
- `h_face_inner`, `h_face_outer`, `h_face_left`, `h_face_right`
- `h_face_counts`
- `h_num_components`

### 3.2 Exact formulas

For the current `tile_side=256`, `k_sq=40` geometry in `src/gpu_uf.cuh:24-25`:

```text
collar = 7
side_exp = 271
total_points = 73,441
bitmap_words = ceil(73,441 / 32) = 2,296
```

#### Reduced per-tile UF buffers

```text
UF bytes/tile
= kMaxPrimesPerTile * (sizeof(parent) + sizeof(comp_id) + sizeof(rank))
= 8192 * (4 + 4 + 1)
= 73,728 bytes
```

This replaces the current:

```text
73,441 * (4 + 4 + 1) = 660,969 bytes
```

#### Compact output staging buffer

Safe fixed device slot:

```text
kMaxCompactTileBytes
= sizeof(CompactTileResult)
 + align_up(kMaxPrimesPerTile, 4)
 + (4 * kMaxFacePortsPerFace) * sizeof(CompactPortRecord)
= 16 + 8192 + 1024 * 8
= 16,400 bytes/tile
```

This is a policy cap derived from the new `256`-ports-per-face limit, not a geometric guarantee over all possible bitmaps. Any tile that exceeds it must raise `kCompactBytesOverflow` and abort the campaign.

Retained packed host blob:

```text
packed_bytes(tile) = 16 + align_up(num_components, 4) + 8 * num_ports
```

This is the number that trends toward `~620` bytes/tile at `R≈80M`.

#### Merge UF buffer

```text
merge_uf_bytes = total_global_components * sizeof(uint32_t)
               = 4 * total_global_components
```

This is the only phase-2 UF state. It no longer scales with `total_points` or total primes.

#### Phase-2 compact indexing/topology buffers

Recommended device buffers:

```text
compact_blob_bytes   = sum_i packed_bytes(tile_i)
compact_offsets      = 8 * (num_tiles + 1)           // uint64_t offsets
tile_origins         = 8 * num_tiles                 // TileOrigin
seam_pairs           = 8 * num_seams                 // SeamPair {u32,u32}
```

`compact_offsets` must be `uint64_t`, not `uint32_t`, because the dense 5.49M-tile compact blob can exceed 4 GiB.

### 3.3 Exact VRAM formulas for batch sizing

The current auto-batcher at `src/fat_stripe_cuda.cu:910-919` mixes device VRAM and pinned host memory into one `per_tile_total`. That is conservative but inaccurate because `cudaMemGetInfo()` only reports device memory.

Replace it with two formulas:

```text
per_tile_phase1_device
= bitmap_words * 4                      // device bitmap
 + 73,728                               // reduced UF
 + 16,400                               // compact device slot
 + 16                                   // d_compact_bytes_used + d_compact_status + d_num_primes + d_origin_component
= 9,184 + 73,728 + 16,400 + 16
= 99,328 bytes/tile
```

```text
per_tile_phase1_pinned_host_stage
= bitmap_words * 4                      // pinned bitmap mirror, only if CPU fallback still exists
 + 16,400                               // compact pinned slot
 + 16                                   // h_compact_bytes_used + h_compact_status + h_num_primes + h_origin_component
= 9,184 + 16,400 + 16
= 25,600 bytes/tile
```

If you keep the current `BatchContext` host bitmap mirror alive, phase-1 device+pinned combined staging is:

```text
99,328 + 25,600 = 124,928 bytes/tile
```

That is the correct replacement for the current `876,009 bytes/tile` combined staging cost.

## 4. Host Orchestration Pseudocode

### 4.1 Topology prepass

Lift the topology construction out of `prepare_boundary_merge_data()` in `src/fat_stripe_cuda.cu:416-583` and make it its own prepass before phase 1.

Outputs:

- `std::vector<uint8_t> exposed_face_masks(num_tiles)`
- `std::vector<SeamPair> horizontal_seams`
- `std::vector<SeamPair> vertical_seams`
- `std::vector<TileOrigin> tile_origins`
- campaign bounds for the radial thresholds

Pseudocode:

```cpp
MergeTopology topo = build_merge_topology(jobs, tile_side);
```

This is the same neighbor lookup already used at `src/fat_stripe_cuda.cu:438-581`, just separated from the now-deleted per-face materialization.

### 4.2 Phase-1 batch loop

```cpp
std::vector<uint8_t> packed_compact_blob;
std::vector<uint64_t> compact_offsets(manifest.num_jobs + 1u, 0u);

uint64_t total_primes = 0;
uint32_t next_component_base = 0;

for (uint32_t batch_start = 0; batch_start < manifest.num_jobs; batch_start += batch_cap) {
    const uint32_t batch_count = min(batch_cap, manifest.num_jobs - batch_start);

    launch_batch_sieve(batch_ctx, jobs.data() + batch_start, batch_count, tile_side, collar);

    run_gpu_uf_compact(
        gpu_uf_ctx,
        batch_ctx.d_bitmaps,
        batch_ctx.d_jobs,
        topo.exposed_face_masks.data() + batch_start,
        batch_start,        // tile_base
        batch_count,
        tile_side,
        collar,
        manifest.k_sq,
        sample_geom.side_exp,
        batch_ctx.bitmap_words);

    // Copy sidebands for this batch.
    cudaMemcpy(h_compact_bytes_used, d_compact_bytes_used, ...);
    cudaMemcpy(h_compact_status, d_compact_status, ...);
    cudaMemcpy(h_num_primes, d_num_primes, ...);
    cudaMemcpy(h_origin_component, d_origin_component, ...); // until legacy path is retired

    for (uint32_t i = 0; i < batch_count; ++i) {
        const uint32_t global_tile_idx = batch_start + i;
        const uint32_t used_bytes = h_compact_bytes_used[i];
        const uint32_t status = h_compact_status[i];

        if (status != gm::kCompactOk) {
            fail_compact_tile(global_tile_idx, status);
        }

        cudaMemcpy(
            h_compact_slots + i * gm::kMaxCompactTileBytes,
            d_compact_slots + i * gm::kMaxCompactTileBytes,
            used_bytes,
            cudaMemcpyDeviceToHost);

        auto* src = reinterpret_cast<gm::CompactTileResult*>(
            h_compact_slots + i * gm::kMaxCompactTileBytes);
        const uint32_t num_components = src->num_components;
        const uint32_t num_ports = src->num_ports;

        if (next_component_base > UINT32_MAX - num_components) {
            fail(kExitIoError, "global component count exceeds uint32_t merge UF");
        }

        total_primes += h_num_primes[i];

        const uint64_t dst_offset = packed_compact_blob.size();
        compact_offsets[global_tile_idx] = dst_offset;
        packed_compact_blob.resize(dst_offset + used_bytes);

        std::memcpy(
            packed_compact_blob.data() + dst_offset,
            src,
            used_bytes);

        auto* dst = reinterpret_cast<gm::CompactTileResult*>(
            packed_compact_blob.data() + dst_offset);
        dst->tile_idx = global_tile_idx;
        dst->component_base = next_component_base;

        gm::CompactPortRecord* ports = gm::compact_ports(dst);
        for (uint32_t p = 0; p < num_ports; ++p) {
            ports[p].comp_id += next_component_base;  // local -> global
        }

        next_component_base += num_components;
    }
}

compact_offsets[manifest.num_jobs] = packed_compact_blob.size();
const uint32_t total_global_components = next_component_base;
```

This online prefix is exactly the same exclusive prefix sum the prompt asks for. It is just streamed batch-by-batch because the batches already arrive in manifest order.

### 4.3 Phase 2

```cpp
CampaignSummary summary = run_gpu_boundary_merge_compact(
    packed_compact_blob,
    compact_offsets,
    topo.tile_origins,
    topo.horizontal_seams,
    topo.vertical_seams,
    topo.bounds,
    total_global_components,
    total_primes,
    manifest.k_sq,
    manifest.tile_side,
    collar);
```

At this point all phase-1 UF buffers are already destroyed. Phase 2 owns only:

- packed compact blob
- compact offset table
- tile origins
- seam pairs
- merge UF parent array

## 5. Seam Merge Kernel Changes

### 5.1 Replace `PreparedMergeData`

Delete the current `PreparedMergeData` build-up at `src/fat_stripe_cuda.cu:380-583`.

Phase 2 no longer uploads:

- `inner_ports`, `outer_ports`, `left_ports`, `right_ports`
- `inner_offsets`, `outer_offsets`, `left_offsets`, `right_offsets`
- `node_face_bits`

Instead upload:

- `d_compact_blob`
- `d_compact_offsets`
- `d_tile_origins`
- `d_horizontal_seams`
- `d_vertical_seams`
- `d_parent` sized by `total_global_components`

### 5.2 New seam kernel shape

Current kernel at `src/fat_stripe_cuda.cu:629-663` receives two already-filtered face arrays plus per-face offsets. Replace it with:

```cpp
__global__
void merge_compact_seams_kernel(
    const uint8_t* __restrict__ d_compact_blob,
    const uint64_t* __restrict__ d_compact_offsets,
    const TileOrigin* __restrict__ d_tile_origins,
    const SeamPair* __restrict__ d_seams,
    uint32_t num_seams,
    uint32_t tile_side,
    uint32_t collar,
    uint8_t face_a,   // gm::kFaceRightBit or gm::kFaceOuterBit
    uint8_t face_b,   // gm::kFaceLeftBit  or gm::kFaceInnerBit
    uint64_t k_sq,
    uint32_t* __restrict__ parent);
```

Face predicate helper:

```cpp
__device__ __forceinline__
bool port_on_face(const gm::CompactPortRecord& p, uint8_t face, uint32_t side, uint32_t collar) {
    switch (face) {
        case gm::kFaceInnerBit: return p.x <= collar;
        case gm::kFaceOuterBit: return p.x >= side - collar;
        case gm::kFaceLeftBit:  return p.y <= collar;
        case gm::kFaceRightBit: return p.y >= side - collar;
        default: return false;
    }
}
```

Kernel pseudocode:

```cpp
const SeamPair seam = d_seams[idx];

const auto* tile_a = reinterpret_cast<const gm::CompactTileResult*>(
    d_compact_blob + d_compact_offsets[seam.tile_a]);
const auto* tile_b = reinterpret_cast<const gm::CompactTileResult*>(
    d_compact_blob + d_compact_offsets[seam.tile_b]);

const gm::CompactPortRecord* ports_a = gm::compact_ports(tile_a);
const gm::CompactPortRecord* ports_b = gm::compact_ports(tile_b);
const TileOrigin origin_a = d_tile_origins[seam.tile_a];
const TileOrigin origin_b = d_tile_origins[seam.tile_b];

for (uint32_t i = 0; i < tile_a->num_ports; ++i) {
    const gm::CompactPortRecord pa = ports_a[i];
    if (!port_on_face(pa, face_a, tile_side, collar)) {
        continue;
    }

    const int64_t a_a = static_cast<int64_t>(origin_a.a_lo) + pa.x;
    const int64_t b_a = static_cast<int64_t>(origin_a.b_lo) + pa.y;

    for (uint32_t j = 0; j < tile_b->num_ports; ++j) {
        const gm::CompactPortRecord pb = ports_b[j];
        if (!port_on_face(pb, face_b, tile_side, collar)) {
            continue;
        }

        const int64_t a_b = static_cast<int64_t>(origin_b.a_lo) + pb.x;
        const int64_t b_b = static_cast<int64_t>(origin_b.b_lo) + pb.y;

        const int64_t da = a_a - a_b;
        const int64_t db = b_a - b_b;
        const uint64_t dist_sq =
            static_cast<uint64_t>(da * da) + static_cast<uint64_t>(db * db);

        if (dist_sq <= k_sq) {
            merge_uf_union(parent, pa.comp_id, pb.comp_id);
        }
    }
}
```

Launches:

- horizontal seams: `face_a = gm::kFaceRightBit`, `face_b = gm::kFaceLeftBit`
- vertical seams: `face_a = gm::kFaceOuterBit`, `face_b = gm::kFaceInnerBit`

### 5.3 Phase-2 finalization and radial spanning

Keep the current merge-parent flatten pattern from `src/fat_stripe_cuda.cu:777-799`.

For component counting:

1. Densify over the full merge UF domain `0..total_global_components-1`, not only over ports.
2. Set `summary.num_components` from that dense-root count.

For radial spanning:

1. Iterate every tile's compact ports.
2. For each port, recover its tile-local component index:

```cpp
const uint32_t local_comp = port.comp_id - tile->component_base;
```

3. Read `face_bits[local_comp]`.
4. Only use the port for radial marking if it lies on at least one face that is still present in `face_bits[local_comp]`.

That preserves the exposed-face scoping fix while still using actual boundary coordinates for the moat verdict, exactly as the current host code does at `src/fat_stripe_cuda.cu:819-865`.

## 6. What To Delete

### Immediate deletions

These become dead as soon as compact emission is the only GPU merge input format:

- `src/gpu_uf.cuh:62` `d_comp_counter`
- `src/gpu_uf.cuh:65-69` all four device face buffers
- `src/gpu_uf.cuh:72` `d_face_counts`
- `src/gpu_uf.cuh:73` `d_num_components`
- `src/gpu_uf.cuh:76` `d_origin_set`
- `src/gpu_uf.cuh:79-87` all four pinned face mirrors plus `h_face_counts` and `h_num_components`
- `src/gpu_uf.cu:611-614` `cudaMalloc(&next.d_comp_counter, ...)`
- `src/gpu_uf.cu:620-689` all face-buffer allocations and host mirrors
- `src/gpu_uf.cu:713-715` free of `d_comp_counter`
- `src/gpu_uf.cu:719-750` frees of face buffers, face mirrors, and `d_origin_set`
- `src/gpu_uf.cu:832-944` the face-count copyback plus the four per-face D2H loops
- `src/fat_stripe_cuda.cu:405-414` `append_records()`
- `src/fat_stripe_cuda.cu:356-367` `TileFaceSpan`
- `src/fat_stripe_cuda.cu:369-373` `MergePort`
- `src/fat_stripe_cuda.cu:380-398` `PreparedMergeData`
- `src/fat_stripe_cuda.cu:416-583` `prepare_boundary_merge_data()`
- `src/fat_stripe_cuda.cu:673-698` the old `MergeDeviceBuffers` fields for four face arrays and four offset arrays
- `src/fat_stripe_cuda.cu:729-738` uploads of `inner/outer/left/right` ports and offsets
- `src/fat_stripe_cuda.cu:967-978` `merge_tile_spans`, `merge_all_inner`, `merge_all_outer`, `merge_all_left`, `merge_all_right`
- `src/fat_stripe_cuda.cu:1007-1043` GPU-UF batch accumulation into four face vectors
- `src/fat_stripe_cuda.cu:1120-1128` call to `prepare_boundary_merge_data()`

### Delete after the legacy `--gpu-uf` stream path is moved to compact decode

These are only needed by the existing face-port stream writer:

- `src/fat_stripe_cuda.cu:328-354` `tile_face_ports_from_gpu_uf()`
- `src/fat_stripe_cuda.cu:301-326` direct `write_tile_result(..., TileFacePorts)` use from the GPU path
- `d_origin_component` / `h_origin_component`, if no caller still needs tile-local origin IDs

If the legacy file format must remain temporarily, reconstruct `TileFacePorts` from one tile's compact record on the host by reclassifying each compact port into the four face vectors using `(x, y, tile_side, collar)`.

## 7. Migration Checklist

Each step is independently testable.

1. Move `kMaxPrimesPerTile` into `src/gpu_uf.cuh`, change the UF allocation/stride from `total_points` to `kMaxPrimesPerTile`, and keep the existing face-port output path.
Test: existing GPU UF differential tests still pass and per-tile VRAM drops from `660,969` UF bytes to `73,728`.

2. Lower `kMaxFacePortsPerFace` to `256` and add explicit per-face overflow flags in the current kernel.
Test: campaigns either pass unchanged or fail loudly on overflow; no silent truncation.

3. Add `CompactPortRecord`, `CompactTileResult`, layout helpers, and `kMaxCompactTileBytes`.
Test: compile-time `static_assert`s and a small host-side unit test for `compact_tile_bytes()`.

4. Add the topology prepass that produces `exposed_face_masks`, `horizontal_seams`, `vertical_seams`, and `tile_origins`.
Test: compare exposed masks and seam pairs against the current `prepare_boundary_merge_data()` behavior on a small synthetic grid.

5. Replace `gpu_uf_tile_kernel()` phase 8 with compact emission into fixed slots.
Test: decode one compact tile on the host and reconstruct the old four face vectors; compare against the current GPU output for the same tile.

6. Replace `GpuUfContext` face buffers and D2H loops with compact slot staging.
Test: batch D2H returns exact `used_bytes` per tile; overflow flags remain zero on known-good campaigns.

7. Stream batches into a packed compact blob and apply the manifest-order `component_base` prefix plus in-place `comp_id += component_base`.
Test: `component_base` is monotone, final `total_global_components` equals the sum of all tile `num_components`, and no `UINT32_MAX` overflow occurs.

8. Replace the old phase-2 merge inputs with compact blob + offsets + tile origins + seam pairs.
Test: `run_gpu_boundary_merge_compact()` matches the current `run_gpu_boundary_merge()` on the same manifest before deleting the old path.

9. Delete dead face-buffer code.
Test: full build succeeds with no references to `d_face_inner`, `d_face_outer`, `d_face_left`, `d_face_right`, `PreparedMergeData`, or `TileFaceSpan`.

## 8. VRAM Budget Table

All byte counts below use the current checked-in geometry:

- `total_points = 73,441`
- `bitmap_words = 2,296`
- `kMaxPrimesPerTile = 8,192`
- `kMaxFacePortsPerFace = 256`
- `kMaxCompactTileBytes = 16,400`

### 8.1 Per-tile staging

| Item | Current bytes/tile | New bytes/tile | Notes |
|---|---:|---:|---|
| Device bitmap | 9,184 | 9,184 | unchanged |
| Pinned bitmap mirror | 9,184 | 9,184 | unchanged while CPU fallback exists |
| Device UF arrays | 660,969 | 73,728 | `total_points` -> `kMaxPrimesPerTile` |
| Device face / compact staging | 98,304 | 16,400 | `4 * 2048 * 12` -> fixed compact slot |
| Pinned face / compact staging | 98,304 | 16,400 | host-side batch staging mirror |
| Sideband scalars | 64 | 32 | current estimate already budgets 64; new path needs 4 device + 4 pinned scalars |
| Combined staging total | 876,009 | 124,928 | 7.01x smaller |

Current staging total comes directly from the formula already embedded at `src/fat_stripe_cuda.cu:910-919`:

```text
current_combined_staging
= bitmap_words * 4 * 2
 + total_points * 9
 + 4 * 2048 * 12 * 2
 + 64
= 876,009 bytes/tile
```

New combined staging:

```text
new_combined_staging
= bitmap_words * 4 * 2
 + 8192 * 9
 + 16,400 * 2
 + 32
= 124,928 bytes/tile
```

### 8.2 5.49M-tile totals

| Quantity | Exact formula | Bytes | GiB |
|---|---:|---:|---:|
| Current all-tiles simultaneous device-only UF+faces+bitmap | `5.49M * (9,184 + 660,969 + 98,304 + 36)` | `4,219,026,570,000` | `3929.27` |
| Current all-tiles simultaneous device+pinned staging | `5.49M * 876,009` | `4,809,289,410,000` | `4479.00` |
| New phase-1 combined staging if all 5.49M tiles were resident at once | `5.49M * 124,928` | `685,854,720,000` | `638.75` |

The last row is intentionally not the operating mode; phase 1 is batched. The useful numbers are the per-tile batch cost and the retained phase-2 footprint below.

### 8.3 Phase-2 retained footprint for 5.49M tiles

| Item | Exact formula | Bytes | GiB |
|---|---:|---:|---:|
| Packed compact blob at target `620` bytes/tile | `5.49M * 620` | `3,403,800,000` | `3.17` |
| Packed compact blob at dense `2 KiB`/tile | `5.49M * 2,048` | `11,243,520,000` | `10.47` |
| Compact offset table | `8 * (5,490,000 + 1)` | `43,920,008` | `0.04` |
| Tile origins | `8 * 5,490,000` | `43,920,000` | `0.04` |
| Seam pairs upper bound | `< 16 * 5,490,000` | `< 87,840,000` | `< 0.08` |
| Merge UF | `4 * total_global_components` | campaign-dependent | campaign-dependent |

For a representative planning number, if phase 1 averages `80` components per tile:

```text
total_global_components = 5,490,000 * 80 = 439,200,000
merge_uf_bytes = 4 * 439,200,000 = 1,756,800,000 bytes = 1.64 GiB
```

Then the phase-2 peak is approximately:

```text
3.17 GiB compact blob
+ 0.04 GiB offsets
+ 0.04 GiB origins
+ 0.08 GiB seam pairs upper bound
+ 1.64 GiB merge UF
= 4.97 GiB
```

That is the important qualitative change: phase 2 becomes a few-GiB problem instead of a multi-terabyte all-tiles UF problem.

### 8.4 Peak formulas used by the runtime

```text
phase1_peak_vram(batch_cap)
= batch_cap * 99,328
```

```text
phase2_peak_vram
= compact_blob_bytes
 + 8 * (num_tiles + 1)
 + 8 * num_tiles
 + 8 * num_seams
 + 4 * total_global_components
```

With `num_seams < 2 * num_tiles`, that last term is bounded by:

```text
phase2_peak_vram
< compact_blob_bytes
 + 32 * num_tiles
 + 8
 + 4 * total_global_components
```

## Summary

The clean implementation is:

1. shrink the per-tile UF workspace to `kMaxPrimesPerTile`,
2. emit one compact tile record directly from `gpu_uf_tile_kernel()`,
3. apply `component_base` on the host while streaming batches,
4. merge only compact records plus a global component UF.

No second extraction kernel survives this design. The only retained phase-1 artifact is the compact boundary representation that phase 2 actually needs.
