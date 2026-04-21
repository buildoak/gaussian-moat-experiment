---
date: 2026-04-21
engine: codex
role: lifter
status: complete
refs:
  - planning/2026-04-21-synthesis-canonical-plan.md
  - planning/2026-04-21-codex-performance.md
  - ../campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu
  - ../cpp-campaign-v2/apps/campaign_main.cpp
---

# Host Pipeline Scaffold Implementation Spec

Scope: M8 only. This document specifies the host-side CUDA campaign scaffold that replaces the CPU per-tile `process_tile` loop with chunked GPU TileOp production, then hands those TileOps to the unchanged CPU compositor and unchanged CPU snapshot writer.

The implementation target is K=36, R=80M, RTX 4090, 24 GB device memory, 200k-tile host chunks, three CUDA streams, and final snapshot SHA parity with the CPU campaign.

## 1. File Ownership

M8 workers should create or modify only the host pipeline and public pipeline interface. Kernel implementation details belong to M2-M7 and should not be redesigned here.

### Create

| File | Owner | Purpose |
|---|---|---|
| `include/cuda_campaign/host_pipeline.h` | M8 | Public C++ API for chunked GPU TileOp production. Owns `GpuPipelineConfig`, `GpuPipelineStats`, and `GpuTilePipeline` declarations. |
| `include/cuda_campaign/gpu_types.cuh` | M8 if not already created by M2 | CUDA-visible SoA pointer structs and `TileInputSoA`/device-slice descriptors. Keep this header layout-only; no policy. |
| `src/host_driver.cpp` | M8 | CUDA allocation, pinned host ring, stream/event schedule, two-phase overlay allocator, chunk dispatch, and D2H handoff. |
| `apps/campaign_main_cuda.cpp` | M8 | Fork of `../cpp-campaign-v2/apps/campaign_main.cpp`; only replaces the tile loop at CPU lines 426-432 with GPU dispatch and overlapped compositor ingest. |
| `tests/test_host_pipeline_small_snapshot.cpp` | M8 | Small full-grid/region test that runs `campaign_main_cuda` path and compares TileOps/snapshot against CPU for a tractable region. |

### Modify

| File | Owner | Required change |
|---|---|---|
| `include/cuda_campaign/kernels.cuh` | M8 with M7 API owner | Add launch prototypes that accept stream, device SoA slices, and per-phase scratch buffers. Do not change kernel semantics. |
| `CMakeLists.txt` | M8 | Add `host_driver.cpp`, `campaign_main_cuda`, and host-pipeline test target. Link against `campaign` from `../cpp-campaign-v2`. |
| `apps/cuda_vs_cpu_diff.cpp` | M8 optional | Add a path that can compare GPU-produced chunk TileOps with CPU TileOps and report first divergence. Keep existing M1 behavior intact. |

### Do Not Modify

| File area | Reason |
|---|---|
| `src/kernel_*.cu` | M8 is host orchestration only; kernel changes belong to M2-M7. |
| `../cpp-campaign-v2/src/snapshot.cpp` | `write_snapshot` must remain CPU-owned and unmodified. |
| `../cpp-campaign-v2/src/compositor.cpp` | Host compositor remains the correctness anchor. |
| `../cpp-campaign-v2/apps/campaign_main.cpp` | Keep CPU app as golden reference; fork into `campaign_main_cuda.cpp`. |

## 2. Host Control Flow

`campaign_main_cuda.cpp` should preserve the CPU main structure:

1. Parse `--k-sq`, `--r-inner`, `--r-outer`, `--region`, `--out`, and timing flags.
2. Build `CampaignConstants` with `CampaignConstants::from_radii`.
3. Build and verify `Grid`.
4. Resolve and clip the requested region.
5. Enumerate `active_tiles` and sort by `(i, j)`, exactly as the CPU main does.
6. Allocate final `std::vector<campaign::TileOp> tileops(active_tiles.size())`.
7. Initialize `campaign::Compositor`.
8. Dispatch GPU chunks in flat-index order.
9. Ingest completed TileOps into the CPU compositor in column order as soon as complete columns are available.
10. Call unmodified `campaign::write_snapshot(out_path, grid, tileops, constants)`.

The GPU path must emit TileOps in `Grid::flat_index` order. Snapshot parity depends on the final `tileops` vector having the same order as CPU `active_tiles`.

## 3. Buffer Interfaces

### Tile Input SoA

Use SoA, not an array of host structs, for device input. This avoids alignment ambiguity and supports coalesced per-field loads.

Per tile:

| Field | Type | Bytes | Meaning |
|---|---:|---:|---|
| `a_lo` | `int64_t` | 8 | Snapped tile low `a` coordinate. |
| `b_lo` | `int64_t` | 8 | Snapped tile low `b` coordinate. |
| `i` | `int32_t` | 4 | Grid tower/column index. |
| `j` | `int32_t` | 4 | Grid row index inside tower. |
| Total |  | 24 | Logical input bytes per tile. |

Host pinned ring buffers:

```cpp
struct HostInputSoA {
  std::int64_t* a_lo;
  std::int64_t* b_lo;
  std::int32_t* i;
  std::int32_t* j;
};

struct HostOutputBuffer {
  campaign::TileOp* tileops;  // 256 B/tile, pinned until compositor ingest is done.
};
```

Device SoA mirrors the same four arrays. Kernel launch functions should receive a slice descriptor with base pointers plus `count` and `global_tile_offset`; kernels should use local tile index for buffer offsets and preserve flat-index order in `d_output[local_idx]`.

### Per-Chunk Sizing

Nominal host chunk size is 200,000 tiles.

| Buffer | Bytes/tile | 200k bytes | 200k MiB |
|---|---:|---:|---:|
| H2D input SoA | 24 | 4,800,000 | 4.58 |
| D2H TileOps | 256 | 51,200,000 | 48.83 |
| One pinned input set | 24 | 4,800,000 | 4.58 |
| One pinned output set | 256 | 51,200,000 | 48.83 |
| Triple pinned input ring | 72 | 14,400,000 | 13.73 |
| Triple pinned output ring | 768 | 153,600,000 | 146.48 |

Host pinned memory for the triple ring is about 160 MiB plus allocation overhead. This is intentional; TileOps must remain pinned until their chunk has been copied into the final `tileops` vector and any eligible columns have been ingested.

### Device Production Buffers

K1-K5 kernels use flat per-tile slices. Constants for K=36:

| Constant | Value |
|---|---:|
| `MAX_PRIMES_GPU` | 6144 |
| `SIDE_EXP` | 269 |
| `BITMAP_WORDS_PER_ROW` | 9 |
| `BITMAP_WORDS` | 2421 |
| `ACTIVE_ROWS + 1` / row-prefix entries | 270 |
| `TileOp` bytes | 256 |

Device buffers:

| Buffer | Type | Bytes/tile | Lifetime |
|---|---|---:|---|
| `d_a_lo` | `int64_t[]` | 8 | K1-K5 coordinate lookup. |
| `d_b_lo` | `int64_t[]` | 8 | K1-K5 coordinate lookup. |
| `d_i` | `int32_t[]` | 4 | K5/tile metadata if needed. |
| `d_j` | `int32_t[]` | 4 | K5/tile metadata if needed. |
| `d_cand_list` | `uint32_t[N * 6144]` | 24,576 | K1 writes, K2 reads. Phase 1 only. |
| `d_total_cands` | `uint32_t[N]` | 4 | K1 writes, K2 reads. Phase 1 only. |
| `d_bitmap` | `uint32_t[N * 2421]` | 9,684 | K2 writes, K3/K4 read. |
| `d_row_prefix` | `uint16_t[N * 270]` | 540 | K3 writes, K4 reads. |
| `d_prime_pos` | `uint32_t[N * 6144]` | 24,576 | K3 writes, K4/K5 read. |
| `d_prime_count` | `uint32_t[N]` | 4 | K3 writes, K4/K5 read. |
| `d_parent` | `uint16_t[N * 6144]` | 12,288 | K4 writes, K5 reads. |
| `d_group_flags` | `uint8_t[N * 32]` | 32 | K4 writes, K5 reads. |
| `d_k4_overflow` | `uint8_t[N]` | 1 | K4 writes, K5 reads. |
| `d_output` | `TileOp[N]` | 256 | K5 writes, D2H reads. |

Production M8 should not allocate persistent `d_prime_geo_bits` for full-campaign runs. If M4 tests keep a debug `prime_geo_bits` buffer, hide it behind a test/debug build flag; it costs 1.23 GB at 200k tiles if stored as one byte per prime.

## 4. Two-Phase Buffer Overlay

The v1 pattern at `../campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu:240-252` separates K1/K2 space from K3/K4/K5 space. M8 must keep that lifetime split.

### Phase 1: K1/K2

Live buffers:

| Buffer | 200k bytes |
|---|---:|
| Input SoA | 4,800,000 |
| `d_cand_list` | 4,915,200,000 |
| `d_total_cands` | 800,000 |
| `d_bitmap` | 1,936,800,000 |
| Total | 6,857,600,000 |

K1 writes candidates and counts. K2 consumes candidates and writes bitmap. After K2 completes, `d_cand_list` and `d_total_cands` are dead.

### Phase 2: K3/K4/K5

Live buffers:

| Buffer | 200k bytes |
|---|---:|
| Input SoA | 4,800,000 |
| `d_bitmap` | 1,936,800,000 |
| `d_row_prefix` | 108,000,000 |
| `d_prime_pos` | 4,915,200,000 |
| `d_prime_count` | 800,000 |
| `d_parent` | 2,457,600,000 |
| `d_group_flags` | 6,400,000 |
| `d_k4_overflow` | 200,000 |
| `d_output` | 51,200,000 |
| Total | 9,481,000,000 |

Exact total: 9,481,000,000 bytes before allocator metadata, about 8.83 GiB.

Because strict "peak <8.5 GB" and literal 200k full Phase 2 are in tension, the M8 allocator must implement a device-slab guard:

```text
host_chunk_tiles = 200000
device_slab_tiles = min(host_chunk_tiles, computed_budget_tiles)
computed_budget_tiles = floor((8.5 GiB - static_tables - safety_margin) / phase2_bytes_per_tile)
```

With the production table above, `phase2_bytes_per_tile = 47,405` including input/output. A conservative 8.5 GiB binary budget with 256 MiB safety gives `device_slab_tiles ~= 180k`. The host still schedules 200k-tile chunks; the driver may internally run that chunk as two device slabs when needed. On a 24 GB 4090, the guard may be configured to allow the full 200k slab if the coordinator accepts the actual 8.83 GiB Phase 2 peak. The code must log the chosen slab size at startup.

### Overlay Rules

1. Allocate one long-lived device arena for Phase 1/Phase 2 scratch, plus persistent input/output buffers and static tables.
2. During Phase 1, map the arena as `d_cand_list` and `d_total_cands`.
3. After the K2-complete event, remap/free/reallocate the dead Phase 1 region as Phase 2 buffers.
4. `d_bitmap` survives from K2 through K4.
5. After K4 completes, `d_bitmap` and `d_row_prefix` are dead. K5 must not read them.
6. D2H may start only after K5-complete event for that slab.

Avoid allocating all Phase 1 and Phase 2 buffers at once. That recreates the 14.4 GB flat allocation called out in the canonical plan and leaves too little room for driver/runtime overhead.

## 5. Device Memory Budget

### Full Flat Allocation, Do Not Use

| Buffer group | 200k bytes | MiB |
|---|---:|---:|
| Input SoA | 4,800,000 | 4.58 |
| K1/K2 candidates/counts | 4,916,000,000 | 4,688.26 |
| Bitmap | 1,936,800,000 | 1,847.08 |
| K3 row prefix / prime pos / count | 5,024,000,000 | 4,791.26 |
| K4 parent / flags / overflow | 2,464,200,000 | 2,349.09 |
| K5 output | 51,200,000 | 48.83 |
| FJ64 table | 524,288 | 0.50 |
| Total | 14,397,524,288 | 13,730.55 |

### Overlay Budget

| Allocation phase | 200k bytes | MiB | Notes |
|---|---:|---:|---|
| Phase 1 peak | 6,857,600,000 | 6,539.92 | K1/K2 plus bitmap. |
| Phase 2 full-200k peak | 9,481,000,000 | 9,041.79 | About 8.83 GiB; use only if accepted by runtime guard. |
| Phase 2 180k slab peak | 8,532,900,000 | 8,137.61 | Fits an 8.5 GiB binary cap with small static-table overhead if safety margin is adjusted. |
| Static FJ64 | 524,288 | 0.50 | Uploaded once; not in arena. |
| Campaign constants / sieve tables | < 10 MB | < 10 | Upload once; exact size depends on M2 constants code. |

The runtime guard should check `cudaMemGetInfo` after static-table upload and fail early if the selected `device_slab_tiles` cannot be allocated with at least 1 GiB remaining free on a 24 GB 4090. The failure mode should be a clear configuration error, not a late K4 allocation failure.

## 6. Stream Schedule

Use three CUDA streams and three pinned host buffer sets. The streams have roles, not fixed chunk identities:

| Stream role | Name | Work |
|---|---|---|
| H2D | `streams.h2d` | Async copy host input SoA to device input SoA for chunk/slab `n+1`. |
| Compute | `streams.compute` | K1-K5 launches for chunk/slab `n`. |
| D2H | `streams.d2h` | Async copy `d_output` TileOps for chunk/slab `n-1` to pinned output. |

Pinned host buffers are a three-slot ring:

| Ring slot state | Example at steady state |
|---|---|
| Filling / H2D pending | chunk `n+1` input SoA prepared by host. |
| Compute owned | chunk `n` device buffers in K1-K5. |
| D2H / ingest owned | chunk `n-1` output copied; chunk `n-2` ready for compositor. |

Steady-state pattern:

```text
iteration n:
  H2D stream:     copy input for chunk n+1
  compute stream: wait H2D(n), launch K1..K5 for chunk n
  D2H stream:     wait K5(n-1), copy TileOps for chunk n-1
  host:           ingest chunk n-2 columns into CPU compositor
```

Events:

| Event | Recorded on | Waited by |
|---|---|---|
| `h2d_done[slot]` | H2D stream after input copy | Compute stream before K1. |
| `k2_done[slot]` | Compute stream after K2 | Host/device allocator before Phase 2 remap if using explicit free/realloc. |
| `compute_done[slot]` | Compute stream after K5 | D2H stream before output copy. |
| `d2h_done[slot]` | D2H stream after output copy | Host before copying into final `tileops` vector and compositor ingest. |
| `ingest_done[slot]` | Host-side boolean/future | H2D filler before reusing pinned slot. |

K1-K5 stay ordered on the compute stream for a slab:

```text
K1 sieve -> K2 MR -> overlay/remap -> K3 compact -> K4 UF+flags -> K5 encode
```

Do not run K1/K2 for a different slab on the same device buffers while Phase 2 for the previous slab is still live. Multi-buffered device arenas are out of scope for M8 unless later tuning proves host overlap is blocked.

## 7. Column-Aligned Handoff

The CPU compositor ingests by complete column:

```cpp
compositor.ingest_column(i, tileops.data() + flat_index_of_column_start);
```

GPU chunks are flat-index contiguous, but a 200k tile boundary can split a column. M8 must keep a small host-side column completion tracker:

| Data | Purpose |
|---|---|
| `column_first_flat[i]` | First flat index for column `i`. |
| `column_count[i]` | Number of active tiles in column `i`. |
| `next_column_to_ingest` | Monotonic column cursor. |
| `completed_until_flat` | Highest contiguous flat index copied into final `tileops`. |

After each `d2h_done`, copy the pinned TileOps into `tileops[chunk_begin..chunk_end)`, update `completed_until_flat`, and ingest every column whose `[first, first + count)` range is now complete. This preserves CPU compositor semantics while allowing arbitrary 200k chunk boundaries.

If `compositor` latches `SPANNING`, `campaign_main_cuda` may short-circuit future GPU dispatch only if snapshot output is not requested. For M8 snapshot SHA parity runs, continue producing all TileOps and write the full snapshot.

## 8. Snapshot Handoff

The handoff contract is:

1. GPU produces one 256 B `campaign::TileOp` per active tile.
2. Host copies TileOps into the final `std::vector<campaign::TileOp>` in CPU flat-index order.
3. CPU `campaign::Compositor` ingests completed columns from that vector.
4. CPU `campaign::write_snapshot` writes the final output from the same vector.

`write_snapshot` must not know whether TileOps came from CPU or CUDA. The CUDA main should print the same hashes as CPU main where available:

| Field | Source |
|---|---|
| `constants_hash` | `constants.canonical_hash()` |
| `mr_witness_sha256` | `CampaignConstants::mr_witness_set_sha256()` |
| `grid_params_hash` in snapshot | Unmodified `write_snapshot` |

## 9. Error Handling

CUDA errors must surface before snapshot writing:

| Failure | Required behavior |
|---|---|
| Allocation exceeds budget | Print selected chunk/slab size, requested bytes, free bytes, and exit nonzero. |
| Kernel launch failure | Synchronize failing stream, print kernel/stage/chunk/slab, exit nonzero. |
| D2H copy failure | Print chunk/slab range and exit nonzero. |
| TileOp size mismatch | `static_assert(sizeof(cuda_campaign::TileOp) == sizeof(campaign::TileOp))`. |
| K_SQ mismatch | Preserve CPU main behavior: reject runtime `--k-sq` if it differs from compile-time `campaign::k_sq_value`. |

Release-mode compositor port mismatches remain CPU compositor policy. M8 should not reinterpret or repair GPU TileOps on host.

## 10. Verification Gate

M8 is complete when:

1. `planning/2026-04-21-host-pipeline-scaffold.md` exists and documents file ownership, buffer layout, stream schedule, memory budget, and snapshot handoff.
2. No source files are modified by this scaffold artifact task.
3. Later implementation workers can use this spec to create `campaign_main_cuda.cpp` and `host_driver.cpp` without making architecture decisions.
4. Runtime M8 implementation logs:
   - host chunk size,
   - device slab size,
   - Phase 1 peak bytes,
   - Phase 2 peak bytes,
   - pinned host bytes,
   - stream count and CUDA device name.
5. Functional ship gate remains the canonical M8 gate: K=36 R=80M snapshot SHA equals the CPU golden, with `cuda_vs_cpu_diff --verbose` reporting first byte divergence if not.

## 11. Implementation Notes for Wave 8

Keep the host pipeline boring. The only correctness-sensitive ordering is final flat-index TileOp order and column-complete compositor ingest. The GPU streams can overlap transfers and compute, but they must not reorder TileOps visible to `write_snapshot`.

The memory numbers above should be encoded as helper functions instead of comments:

```cpp
std::size_t phase1_bytes_for_tiles(std::size_t n);
std::size_t phase2_bytes_for_tiles(std::size_t n);
std::size_t pinned_bytes_for_tiles(std::size_t n, int ring_slots);
```

Those helpers should be unit-tested with the K=36 constants so future K=40 or buffer-layout changes cannot silently violate the 24 GB target.
