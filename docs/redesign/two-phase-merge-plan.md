# GPU Boundary Merge Redesign: Two-Phase Plan

## Scope

This plan is based on the current checked-in implementation in:

- `src/fat_stripe_cuda.cu`
- `src/gpu_uf.cu`
- `src/gpu_uf.cuh`
- `src/face_port_io.h`
- `tile-probe/crates/moat-kernel/src/compose.rs`
- `tile-probe/crates/fat-stripe/src/orchestrator.rs`
- `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs`
- `tile-probe/crates/fat-stripe-cuda/src/protocol.rs`
- `docs/bugs/gpu-merge-false-connectivity.md`

The current code already reuses `GpuUfContext` across batches, but the merge path is still built around the legacy `FacePortRecord` stream and late host-side reconstruction in `prepare_boundary_merge_data()` at `src/fat_stripe_cuda.cu:416-584`. The redesign below makes the compact representation the primary merge format, assigns global component bases during batch processing, and keeps phase 2 completely independent of per-prime UF state.

The target is:

1. Phase 1: batch UF, compact immediately, discard UF state.
2. Phase 2: merge only compact boundary data plus a global merge UF.
3. Phase 3: run the radial spanning verdict using only exposed campaign-boundary faces.

## 7. Per-Tile VRAM Fix (Do This First)

This is independent of the full redesign and should land first.

### Current problem

`create_gpu_uf_context()` allocates the UF arrays as `[batch_cap * total_points]`:

- `d_parent` at `src/gpu_uf.cu:603`
- `d_comp_id` at `src/gpu_uf.cu:607`
- `d_rank` at `src/gpu_uf.cu:615`

For `tile_side=256`, `k_sq=40`, `collar=7`, `total_points=73441`, that is:

- `73441 * (4 + 4 + 1) = 660,969 bytes = 645.48 KiB/tile`

But the kernel only ever indexes these arrays by compact prime index `i < scalars.num_primes`, and `scalars.num_primes` is hard-capped at `kMaxPrimesPerTile` by the overflow check at `src/gpu_uf.cu:348-365`.

### Required change

1. Move `kMaxPrimesPerTile` from `src/gpu_uf.cu:25` into `src/gpu_uf.cuh` so the allocator and VRAM estimator can use it.
2. Change `GpuUfContext` comments in `src/gpu_uf.cuh:58-64` from `[batch_cap * total_points]` to `[batch_cap * kMaxPrimesPerTile]`.
3. In `create_gpu_uf_context()` in `src/gpu_uf.cu:597-615`, allocate:
   - `d_parent`: `tile_count * kMaxPrimesPerTile`
   - `d_comp_id`: `tile_count * kMaxPrimesPerTile`
   - `d_rank`: `tile_count * kMaxPrimesPerTile`
4. In `gpu_uf_tile_kernel()` at `src/gpu_uf.cu:303-306`, compute the per-tile UF slice with stride `kMaxPrimesPerTile`, not `total_points`.
5. Leave `total_points` unchanged for bitmap traversal and coordinate math. Only the UF arrays shrink.

### Face-port cap fix

Change `kMaxFacePortsPerFace` in `src/gpu_uf.cuh:41`:

```cpp
constexpr uint32_t kMaxFacePortsPerFace = 256;
```

Savings:

- Device face buffers: `4 * (2048 - 256) * 12 = 86,016 bytes = 84 KiB/tile`
- Pinned host mirrors: same again
- Total save: `168 KiB/tile`

### Also delete dead allocations

These are allocated but unused in the current kernel:

- `d_comp_counter` in `src/gpu_uf.cuh:62`, allocated at `src/gpu_uf.cu:611`
- `d_origin_set` in `src/gpu_uf.cuh:76`, allocated at `src/gpu_uf.cu:669`

Remove them from:

- `GpuUfContext`
- `create_gpu_uf_context()`
- `destroy_gpu_uf_context()`

### VRAM estimate update

Update `per_tile_uf` in `src/fat_stripe_cuda.cu:914-918` to use:

- `kMaxPrimesPerTile * (4 + 4 + 1)` for UF arrays
- `4 * kMaxFacePortsPerFace * sizeof(FacePortRecord) * 2` for device + pinned host face buffers

After the fix:

- UF arrays: `8192 * 9 = 72 KiB/tile`
- Face buffers at cap 256: `24 KiB/tile` total device + host
- Immediate savings before the redesign: about `741 KiB/tile`

### Safety requirement

Do not silently truncate once the cap is lowered to 256. Add an explicit per-face overflow flag and fail the campaign if any tile exceeds the cap. The current saturating writes at `src/gpu_uf.cu:497-531` are acceptable at 2048; they are too risky at 256 without a hard error.

## 1. Data Structures

### 1.1 C++ compact records

Add the compact merge records to `src/fat_stripe_cuda.cu` first. If they end up shared with `gpu_uf.cu`, move them into a new header such as `src/compact_merge.h`.

```cpp
#pragma pack(push, 1)
struct TilePortRecord {
    uint16_t rel_a;               // absolute a = tile.a_lo + rel_a
    uint16_t rel_b;               // absolute b = tile.b_lo + rel_b
    uint16_t local_component_id;  // tile-local component id
};
#pragma pack(pop)
static_assert(sizeof(TilePortRecord) == 6, "TilePortRecord size mismatch");

struct TileCompactRecord {
    int32_t  a_lo;                // tile origin
    int32_t  b_lo;                // tile origin
    uint32_t component_base;      // exclusive prefix sum over prior tiles
    uint32_t port_offset;         // start of this tile's ports in all_ports[]
    uint32_t face_bits_offset;    // start of this tile's bytes in component_face_bits[]
    uint16_t num_components;      // <= kMaxPrimesPerTile
    uint16_t inner_count;         // number of ports in [port_offset, ...)
    uint16_t outer_count;
    uint16_t left_count;
    uint16_t right_count;
    uint8_t  exposed_face_mask;   // FACE_INNER/OUTER/LEFT/RIGHT bits for campaign boundary exposure
    uint8_t  reserved[3];
};
static_assert(sizeof(TileCompactRecord) == 32, "TileCompactRecord size mismatch");
```

Port order inside `all_ports` must be fixed:

1. inner
2. outer
3. left
4. right

This lets phase 2 derive every face span from one `port_offset` plus four counts.

### 1.2 Global host container

Replace the current `TileFaceSpan + all_inner/all_outer/all_left/all_right + PreparedMergeData` build-up with:

```cpp
struct MergeTopology {
    std::vector<uint8_t> exposed_face_masks; // per tile
    std::vector<SeamPair> horizontal_seams;
    std::vector<SeamPair> vertical_seams;
    int64_t a_min = 0;
    int64_t a_max = 0;
    int64_t b_min = 0;
    int64_t b_max = 0;
};

struct CompactMergeData {
    std::vector<TileCompactRecord> tiles;
    std::vector<TilePortRecord> all_ports;
    std::vector<uint8_t> component_face_bits; // one byte per global component id
    std::vector<SeamPair> horizontal_seams;
    std::vector<SeamPair> vertical_seams;
    uint32_t total_components = 0;
    uint64_t total_primes = 0;
    int64_t a_min = 0;
    int64_t a_max = 0;
    int64_t b_min = 0;
    int64_t b_max = 0;
};
```

`component_face_bits` stores campaign-exposed face bits only. It is not required for the radial verdict, but it is valuable for debug, invariants, and preserving the rectangular-face semantics when needed.

### 1.3 Exact byte layout, alignment, endianness

- `TilePortRecord` is packed, 6 bytes, 1-byte alignment.
- `TileCompactRecord` is naturally 4-byte aligned, 32 bytes.
- `component_face_bits` is raw `uint8_t`.
- All current C++/Rust protocol structs are POD and implicitly little-endian. Keep the same assumption here.
- Because the compact records are internal to the CUDA binary in the first implementation, no wire-compatibility issue exists yet.
- If these records are later serialized, document them as little-endian and keep the `static_assert(sizeof(...))` checks on the C++ side and `size_of::<...>()` tests on the Rust side.

### 1.4 Global component id encoding

Do not store a raw global component id in the 6-byte port record. That will not fit at 600K x 600K scale.

Use:

```text
global_component_id = tile.component_base + port.local_component_id
```

Rules:

- `component_base` is the exclusive prefix sum of `num_components` over tiles in manifest order.
- Compute the prefix sum in `uint64_t`.
- Fail before phase 2 if the final total exceeds `UINT32_MAX`.
- Store `component_base` as `uint32_t` only after the overflow check passes.
- `local_component_id` remains `uint16_t` because `kMaxPrimesPerTile=8192`.

This keeps the port payload at 6 bytes while still supporting `~165M` global component ids.

### 1.5 Rust protocol struct

No Rust wire-format change is required for the main path if the CUDA binary still emits only `CampaignSummary` for `--gpu-boundary-merge`.

Keep unchanged:

- `src/face_port_io.h`
- `tile-probe/crates/fat-stripe-cuda/src/protocol.rs`
- `tile-probe/crates/fat-stripe-cuda/src/driver.rs`

If you later add an optional debug dump of the compact phase-1 output, mirror the structs in Rust as:

```rust
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TilePortRecord {
    pub rel_a: u16,
    pub rel_b: u16,
    pub local_component_id: u16,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TileCompactRecord {
    pub a_lo: i32,
    pub b_lo: i32,
    pub component_base: u32,
    pub port_offset: u32,
    pub face_bits_offset: u32,
    pub num_components: u16,
    pub inner_count: u16,
    pub outer_count: u16,
    pub left_count: u16,
    pub right_count: u16,
    pub exposed_face_mask: u8,
    pub reserved: [u8; 3],
}
```

## 2. Phase 1 Implementation

### 2.1 What to reuse vs. what to change

Keep the current GPU UF kernel and face extraction logic as the first implementation. The least risky approach is:

- Keep `gpu_uf_tile_kernel()` generating the existing per-face `FacePortRecord` output at `src/gpu_uf.cu:468-552`
- Compact those face ports on the host immediately after each batch
- Free the batch UF buffers after phase 1 completes

This avoids coupling the redesign to a risky rewrite of the proven component-assignment logic. The big VRAM win comes from:

- the section-7 allocation fix
- discarding batch UF state before phase 2
- never constructing per-prime merge state across all tiles

If phase-1 compaction later shows up in profiles, a post-UF device compaction kernel can be added, but it is not required for the first end-to-end implementation.

### 2.2 New topology prepass

Extract the neighbor/topology portion of `prepare_boundary_merge_data()` from `src/fat_stripe_cuda.cu:438-581` into a new function:

```cpp
MergeTopology build_merge_topology(
    const std::vector<TileJob>& jobs,
    uint32_t tile_side);
```

Responsibilities:

1. Build `tile_lookup` keyed by `(a_lo, b_lo)`.
2. Build `horizontal_seams` and `vertical_seams`.
3. Compute `a_min/a_max/b_min/b_max`.
4. Compute `exposed_face_mask` per tile using missing-neighbor checks, not min/max row checks:
   - inner exposed iff no tile at `(a_lo - side, b_lo)`
   - outer exposed iff no tile at `(a_lo + side, b_lo)`
   - left exposed iff no tile at `(a_lo, b_lo - side)`
   - right exposed iff no tile at `(a_lo, b_lo + side)`

This matches the fix recommended in `docs/bugs/gpu-merge-false-connectivity.md:140-153` and also handles ragged future grids correctly.

### 2.3 Host compaction helper

Add a helper in `src/fat_stripe_cuda.cu`:

```cpp
void append_compact_gpu_uf_batch(
    const std::vector<TileJob>& jobs,
    uint32_t batch_start,
    uint32_t batch_count,
    const gm::GpuUfContext& ctx,
    const MergeTopology& topology,
    uint32_t tile_side,
    uint64_t* next_component_base,
    CompactMergeData* out);
```

Behavior per tile:

1. Read:
   - `num_components` from `ctx.h_num_components`
   - `num_primes` from `ctx.h_num_primes`
   - face counts from `ctx.h_face_counts`
   - face payloads from `ctx.h_face_inner/outer/left/right`
2. Validate:
   - `num_components <= kMaxPrimesPerTile`
   - each `component_id < num_components`
   - each relative coordinate fits `0..tile_side`
   - `*next_component_base + num_components <= UINT32_MAX`
3. Create one `TileCompactRecord`.
4. Append the tile's ports to `out->all_ports` in fixed order `inner, outer, left, right`.
5. Append `num_components` bytes to `out->component_face_bits`, initialized to zero.
6. For each appended port, OR the current face bit into the local component's byte only if that face is exposed in `topology.exposed_face_masks[global_tile_idx]`.
7. Increment `*next_component_base`.
8. Accumulate `out->total_primes`.

Relative-coordinate conversion:

```cpp
rel_a = static_cast<uint16_t>(port.a - job.a_lo);
rel_b = static_cast<uint16_t>(port.b - job.b_lo);
```

### 2.4 Batch loop pseudocode

Replace the current merge accumulation path at `src/fat_stripe_cuda.cu:967-1128` with:

```cpp
MergeTopology topology = build_merge_topology(jobs, manifest.tile_side);

CompactMergeData compact;
compact.tiles.resize(manifest.num_jobs);
compact.horizontal_seams = topology.horizontal_seams;
compact.vertical_seams = topology.vertical_seams;
compact.a_min = topology.a_min;
compact.a_max = topology.a_max;
compact.b_min = topology.b_min;
compact.b_max = topology.b_max;
compact.all_ports.reserve(manifest.num_jobs * 96); // conservative initial reserve

uint64_t next_component_base = 0;

for each batch:
    launch_batch_sieve(...)
    run_gpu_uf(...)
    append_compact_gpu_uf_batch(
        jobs,
        batch_start,
        batch_count,
        gpu_uf_ctx,
        topology,
        manifest.tile_side,
        &next_component_base,
        &compact)

compact.total_components = static_cast<uint32_t>(next_component_base);

destroy_gpu_uf_context(&gpu_uf_ctx)
destroy_batch_context(&batch_ctx)

CampaignSummary summary = run_gpu_boundary_merge(compact, manifest.k_sq);
```

Important:

- Reuse the existing batch processing loop. Do not allocate per batch unless that simplifies error handling; one reusable batch-scoped context is fine.
- The critical boundary is that `gpu_uf_ctx` and `batch_ctx` must be destroyed before phase 2 allocations begin.

### 2.5 Prefix sum location

Compute the global component prefix sum on the CPU during host compaction.

Reasons:

- `num_components` is already copied back to host by `run_gpu_uf()`.
- Prefix cost is trivial compared with UF and seam merge.
- CPU-side prefix avoids introducing another device scan dependency.
- The merge topology and batch order already follow manifest order, so a streaming prefix is exact.

Storage:

- Keep the running sum in `uint64_t next_component_base`.
- Store the per-tile base in `TileCompactRecord.component_base` as `uint32_t`.
- Store the final total in `CompactMergeData.total_components`.

## 3. Phase 2 Implementation

### 3.1 Replace `PreparedMergeData` with compact merge input

Delete the late reconstruction pass centered on:

- `TileFaceSpan` at `src/fat_stripe_cuda.cu:356-367`
- `MergePort` at `src/fat_stripe_cuda.cu:369-373`
- `PreparedMergeData` at `src/fat_stripe_cuda.cu:380-398`
- `prepare_boundary_merge_data()` at `src/fat_stripe_cuda.cu:416-584`

Phase 1 already produced:

- globally based component ids
- one compact port array
- one tile metadata array
- seam lists
- campaign extents

Nothing in phase 2 should need the legacy `FacePortRecord` buffers anymore.

### 3.2 Device buffers

Replace `MergeDeviceBuffers` in `src/fat_stripe_cuda.cu:673-698` with:

```cpp
struct MergeDeviceBuffers {
    TileCompactRecord* d_tiles = nullptr;
    TilePortRecord*    d_ports = nullptr;
    SeamPair*          d_horizontal_seams = nullptr;
    SeamPair*          d_vertical_seams = nullptr;
    uint32_t*          d_parent = nullptr;
};
```

Do not upload `component_face_bits` to the GPU in the first implementation. Phase 3 will run on the host using the already compacted host buffers.

### 3.3 Face-span helpers

Add host and device helpers:

```cpp
enum FaceKind : uint8_t {
    kFaceInner = 0,
    kFaceOuter = 1,
    kFaceLeft  = 2,
    kFaceRight = 3,
};

__host__ __device__
inline uint32_t face_begin(const TileCompactRecord& tile, FaceKind face);

__host__ __device__
inline uint32_t face_count(const TileCompactRecord& tile, FaceKind face);
```

With the fixed port order:

- inner begins at `port_offset`
- outer begins at `port_offset + inner_count`
- left begins at `port_offset + inner_count + outer_count`
- right begins at `port_offset + inner_count + outer_count + left_count`

### 3.4 Seam merge kernel redesign

Replace the current `merge_seams_kernel()` at `src/fat_stripe_cuda.cu:629-663` with:

```cpp
__global__
void merge_seams_kernel(
    const TileCompactRecord* tiles,
    const TilePortRecord* ports,
    const SeamPair* seams,
    uint32_t num_seams,
    FaceKind face_a,
    FaceKind face_b,
    uint64_t k_sq,
    uint32_t* parent);
```

Per seam:

1. Load `tile_a = tiles[seam.tile_a]`, `tile_b = tiles[seam.tile_b]`.
2. Derive the face spans from `port_offset` and face counts.
3. For each port pair:
   - reconstruct absolute coordinates
   - reconstruct global component ids
   - union if distance squared `<= k_sq`

Device-side reconstruction:

```cpp
const auto& pa = ports[i];
const auto& pb = ports[j];

int64_t a_a = static_cast<int64_t>(tile_a.a_lo) + pa.rel_a;
int64_t b_a = static_cast<int64_t>(tile_a.b_lo) + pa.rel_b;
int64_t a_b = static_cast<int64_t>(tile_b.a_lo) + pb.rel_a;
int64_t b_b = static_cast<int64_t>(tile_b.b_lo) + pb.rel_b;

uint32_t ga = tile_a.component_base + pa.local_component_id;
uint32_t gb = tile_b.component_base + pb.local_component_id;
```

Call pattern:

- horizontal seams: `(right, left)`
- vertical seams: `(outer, inner)`

That preserves the logic in:

- CUDA: `src/fat_stripe_cuda.cu:747-775`
- Rust reference: `tile-probe/crates/moat-kernel/src/compose.rs:99-118` and `236-255`

### 3.5 Seam-pair enumeration

Enumerate seam pairs once in `build_merge_topology()`.

Definitions:

- horizontal seam: tile `(a_lo, b_lo)` has neighbor `(a_lo, b_lo + side)`
- vertical seam: tile `(a_lo, b_lo)` has neighbor `(a_lo + side, b_lo)`

This keeps the current grid topology logic but moves it ahead of phase 1 instead of rebuilding it during `prepare_boundary_merge_data()`.

### 3.6 Merge UF sizing and allocation

Size the phase-2 UF from `CompactMergeData.total_components`.

Allocation:

```cpp
cudaMalloc(&buffers.d_parent, total_components * sizeof(uint32_t));
```

No rank array is required for phase 2. Reuse the existing merge UF logic:

- `merge_uf_find()` from `src/fat_stripe_cuda.cu:586-599`
- `merge_uf_union()` from `src/fat_stripe_cuda.cu:601-618`
- `merge_init_parent_kernel()` from `src/fat_stripe_cuda.cu:620-626`

### 3.7 Flatten kernel

Keep `merge_flatten_kernel()` exactly as today:

- current implementation at `src/fat_stripe_cuda.cu:666-671`

Run the same three-pass device flatten followed by a host path-compression pass after copying `d_parent` back.

## 4. Phase 3 Implementation

### 4.1 Verdict source of truth

Match the radial test in:

- `tile-probe/crates/fat-stripe/src/orchestrator.rs:100-195`

Do not use rectangular `INNER && OUTER` as the final moat verdict.

### 4.2 Threshold calculation

Keep the same threshold math already present in `run_gpu_boundary_merge()` at `src/fat_stripe_cuda.cu:821-834`.

Use `CompactMergeData.a_min/a_max/b_min/b_max`:

- on-axis (`b_min == 0`):
  - `r_inner_geom = a_min`
  - `r_outer_geom = a_max`
- off-axis (`b_min > 0`):
  - `r_inner_geom = sqrt(a_min^2 + b_min^2)`
  - `r_outer_geom = sqrt(a_max^2 + b_max^2)`
- `collar = ceil(sqrt(k_sq))`
- `r_inner_thresh = r_inner_geom + collar`
- `r_outer_thresh = max(r_outer_geom - collar, 0)`

### 4.3 Only boundary tiles contribute

This is the crucial scoping rule from the bug audit.

Do not scan every stored seam port. Only scan ports belonging to exposed campaign faces.

Implementation:

1. After copying `d_parent` to host, run host path compression.
2. Allocate `std::vector<uint8_t> root_flags(total_components, 0)`.
3. For each tile:
   - read `tile.exposed_face_mask`
   - if `FACE_INNER_BIT` is set, scan the tile's inner face span
   - if `FACE_OUTER_BIT` is set, scan the tile's outer face span
   - if `FACE_LEFT_BIT` is set, scan the tile's left face span
   - if `FACE_RIGHT_BIT` is set, scan the tile's right face span
4. For each scanned port:
   - reconstruct `global_component_id`
   - `root = parent[global_component_id]`
   - compute `r_sq`
   - set `root_flags[root] |= kSeenInner` if `r_sq <= r_inner_sq`
   - set `root_flags[root] |= kSeenOuter` if `r_sq >= r_outer_sq`

This exactly enforces "only boundary tiles contribute" while still using the radial criterion.

### 4.4 Component counting and spanning id

Do not build a dense `root_to_dense` vector of length `total_components` unless you need it for debugging.

Use:

1. Count merged components:

```cpp
uint32_t num_components = 0;
for (uint32_t i = 0; i < total_components; ++i) {
    if (parent[i] == i) ++num_components;
}
```

2. Recover the dense index of the first spanning component:

```cpp
uint32_t dense = 0;
for (uint32_t i = 0; i < total_components; ++i) {
    if (parent[i] != i) continue;
    if (root_flags[i] == (kSeenInner | kSeenOuter) && summary.spanning_component < 0) {
        summary.spanning_component = static_cast<int32_t>(dense);
    }
    ++dense;
}
summary.num_components = num_components;
```

This preserves the current summary semantics without allocating another 660 MB host vector.

### 4.5 Role of `component_face_bits`

`component_face_bits` is not required for the radial verdict. Use it for:

- debug assertions during development
- optional rectangular-face summaries
- verifying the exposed-face scoping bug is fixed

Example invariant:

- if a component byte has `FACE_INNER_BIT`, at least one scanned exposed inner-face port for that component must exist before merging

## 5. Migration Path

### 5.1 Reuse

Keep and reuse:

- `launch_batch_sieve()` and `BatchContext`
- `run_gpu_uf()` and `gpu_uf_tile_kernel()`
- merge UF device helpers in `src/fat_stripe_cuda.cu:586-671`
- `CampaignSummary` output format in `src/face_port_io.h` and `protocol.rs`
- Rust driver/orchestrator entrypoints:
  - `tile-probe/crates/fat-stripe-cuda/src/driver.rs`
  - `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs`

### 5.2 Rewrite

Rewrite these areas:

- `prepare_boundary_merge_data()` in `src/fat_stripe_cuda.cu`
- merge accumulation inside the main batch loop in `src/fat_stripe_cuda.cu:967-1128`
- `run_gpu_boundary_merge()` in `src/fat_stripe_cuda.cu:701-866`

Add:

- `build_merge_topology()`
- `append_compact_gpu_uf_batch()`
- compact face-span helpers
- compact-data upload in phase 2
- rewritten `merge_seams_kernel()` signature

### 5.3 Delete

Delete after the compact path is working:

- `TileFaceSpan`
- `append_records()`
- `MergePort`
- `PreparedMergeData`
- the old `prepare_boundary_merge_data()`
- `d_comp_counter`
- `d_origin_set`

Optional cleanup:

- remove the unreachable CPU-extraction branch for `cfg.gpu_boundary_merge` in `src/fat_stripe_cuda.cu:1073-1106`, because `main()` already forbids `--gpu-boundary-merge` without `--gpu-uf` at `src/fat_stripe_cuda.cu:894-896`

### 5.4 Estimated LoC delta

Expected order of magnitude:

- `src/gpu_uf.cuh`: `+20 / -10`
- `src/gpu_uf.cu`: `+60 / -40`
- `src/fat_stripe_cuda.cu`: `+250 / -220`
- Rust protocol/driver/orchestrator: `0` for the main path

Net change: roughly `+60 to +100 LoC`, but with major simplification in the merge preparation path.

### 5.5 Risk areas and edge cases

1. `tile_side > 65535`
   - `TilePortRecord` no longer fits.
   - Add a startup guard and fail fast, or widen `rel_a/rel_b` to `uint32_t`.
2. Total components exceed `UINT32_MAX`
   - Fail before phase 2; the current plan assumes `~165M`, which is safe.
3. Face-port overflow at cap 256
   - Must be a hard error, not silent truncation.
4. Empty tiles
   - `num_components=0`, zero face counts, zero-length `component_face_bits` slice.
5. Ragged future grids
   - Must use missing-neighbor exposure logic, not min/max row logic.
6. Off-axis campaigns
   - Verify threshold math against `fat-stripe` reference.
7. Host memory
   - The compact representation is intentionally host-resident during phase 1. Budget for a few GB of RAM.

## 6. Verification Plan

### 6.1 Correctness

#### Unit tests

Add focused tests for:

1. `build_merge_topology()`
   - seam counts on `1x1`, `1xN`, `Nx1`, `2x2`
   - exposed-face masks on missing-neighbor cases
2. `append_compact_gpu_uf_batch()`
   - relative coordinate conversion
   - fixed face ordering
   - component-base assignment
   - exposed-face-bit population
3. phase-2 face-span helpers
   - every face begin/count is correct for tiles with mixed zero/nonzero counts

#### Differential tests

Compare the final verdict against the CPU/reference path on the same campaign geometry:

1. `fat-stripe-cuda` without GPU merge
   - still useful for merge topology and component count sanity
2. `fat-stripe` moat verdict in `tile-probe/crates/fat-stripe/src/orchestrator.rs`
   - this is the real radial-spanning reference

Test sets:

- tiny synthetic `1x1`, `1x2`, `2x1`, `2x2`
- on-axis campaigns
- off-axis campaigns (`b_min > 0`)
- the false-connectivity case from `docs/bugs/gpu-merge-false-connectivity.md`

#### Regression assertions

Keep one targeted regression for the exact bug described in the audit:

- two vertically adjacent tiles
- lower component touches only local outer seam
- upper component touches only local inner seam
- one seam match exists
- result must not be reported as spanning unless an exposed campaign boundary is also touched

### 6.2 VRAM usage

Instrument with `cudaMemGetInfo()` and stderr logging at:

1. before phase 1 allocations
2. after `create_batch_context() + create_gpu_uf_context()`
3. after phase 1 contexts are destroyed
4. after phase 2 uploads (`tiles + ports + seams + parent`)

Expected shape on a 24 GB 3090:

- phase 1 peak dominated by batch UF + bitmaps
- phase 2 peak roughly:
  - ports: `~2.8 to 3.1 GB` depending on average ports/tile
  - tile metadata: `~176 MB`
  - merge UF parent: `~630 MB`
  - seams: tens to low hundreds of MB
  - total: comfortably below `5 GB`

The key acceptance criterion is that phase 2 no longer carries any per-prime per-tile UF arrays and therefore does not OOM on campaigns that phase 1 can batch through.

### 6.3 Performance expectations

Phase 1:

- Sieve + UF kernel runtime should be effectively unchanged.
- Host compaction cost is linear in boundary-port count and should be small relative to sieve + UF.

Phase 2:

- One-time H2D upload of compact data.
- Seam merge is now proportional to boundary ports, not `total_points`.
- Flatten and D2H parent copy should be on the order of seconds or less for the target scale.

If phase 2 is slower than expected, the first optimization is not another data-structure rewrite. It is sorting each face slice by the seam-axis coordinate and replacing the all-pairs nested loop with a bounded window walk.

## Recommended Implementation Order

1. Land section 7 only:
   - UF arrays sized by `kMaxPrimesPerTile`
   - face cap reduced to `256`
   - hard overflow detection
   - delete dead allocations
2. Extract `build_merge_topology()` from the current `prepare_boundary_merge_data()`.
3. Add `TilePortRecord`, `TileCompactRecord`, `CompactMergeData`.
4. Compact per batch on the host immediately after `run_gpu_uf()`.
5. Destroy phase-1 contexts before phase 2.
6. Rewrite `run_gpu_boundary_merge()` to consume `CompactMergeData`.
7. Add differential tests and VRAM instrumentation.

This ordering gives a shippable intermediate after step 1 and keeps the full redesign low-risk by reusing the existing UF kernel and only replacing the merge preparation/output format.
