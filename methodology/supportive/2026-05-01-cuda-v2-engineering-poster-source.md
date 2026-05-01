---
title: "Poster 1 Source: CUDA v2 Engineering Architecture"
date: 2026-05-01
type: poster-source
status: source-artifact
scope: tiles-maxxing/cuda-campaign-v2-sqrt-36
---

# Poster 1 Source: CUDA v2 Engineering Architecture

## Authority Note

This is an engineering explainer for poster generation, not a mathematical proof.
The CUDA code path is treated as implementation evidence. The C++ v2 path is used
only to explain the reference data structures and byte-parity semantics the CUDA
path mirrors. Inferred semantics are labeled as such.

Primary inspected surface:

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/`
- `tiles-maxxing/cpp-campaign-v2/` for `TileCoord`, `Prime`, `TileOp`,
  `Grid`, `CampaignConstants`, `DSU`, compositor, and CPU TileOp semantics
- nearby CUDA planning, profiling, tests, and goldens under the CUDA tree

No CUDA build, CUDA test, benchmark, or remote run was executed for this artifact.

## Poster Thesis

The current CUDA architecture is a byte-parity TileOp factory wrapped around the
C++ campaign engine.

CPU owns:

- campaign input validation
- snapped grid construction
- region clipping
- active-tile ordering
- compositor verdicts
- snapshot writing

CUDA owns:

- per-tile Gaussian-prime discovery
- per-tile local connectivity
- dense wire-label assignment
- geo flag accumulation
- face-port extraction
- 256-byte `TileOp` emission

Poster copy fragment:

> The GPU does not decide the theorem. It manufactures the exact 256-byte local
> witness objects the CPU campaign engine already knows how to stitch.

## One-Screen Architecture

Candidate poster diagram:

```text
CLI radii + region
  |
  v
CPU: CampaignConstants + Grid + Region
  |     refs: apps/campaign_main_cuda.cpp:391-428
  v
active TileCoord[] in canonical flat-index order
  |
  v
CUDA host driver
  - upload constants/tables once
  - H2D TileCoord slabs
  - launch K1..K5 kernels
  - D2H TileOp slabs
  refs: src/host_driver.cpp:960-963, 1031-1237
  |
  v
GPU K1 sieve candidates
  -> K2 Miller-Rabin bitmap
  -> K3 compact bitmap to prime list
  -> K4 union/find + dense labels + geo flags
  -> K5 face ports + TileOp pack
  refs: src/host_driver.cpp:1046-1117
  |
  v
CPU: Compositor ingests columns
  |
  v
CPU: Snapshot writer emits stable binary artifact
  refs: apps/campaign_main_cuda.cpp:450-474
```

## Runtime Data Model

### `Grid`

Reference: `cpp-campaign-v2/include/campaign/grid.h:48-167`.

`Grid` is the CPU-side snapped-tile tower table for the canonical octant. Its
active tile list is emitted in canonical column-major order, `i` ascending then
`j` ascending. This order is the snapshot order and the CUDA output order.

Key fields:

- `i_min`, `i_max`
- `j_low[]`, `j_high[]`
- `tower_offset[]`
- `total_tiles`
- optional `explicit_tiles` for sparse regions

CUDA host use:

- `campaign_main_cuda` builds and verifies the grid at
  `apps/campaign_main_cuda.cpp:400-417`.
- It clips the grid to the requested region and sorts `active_tiles` by `(i,j)`
  at `apps/campaign_main_cuda.cpp:419-433`.

Poster callout:

> The campaign is a flat stream only after the CPU has built the exact grid.
> CUDA receives tiles, not a geometry mandate.

### `TileCoord`

Reference: `cpp-campaign-v2/include/campaign/grid.h:40-53`.

`TileCoord` carries:

- `i`, `j`: snapped grid indices
- `a_lo`, `b_lo`: world-space lower corner of the proper tile

CUDA materialization:

- Device buffers store `campaign::TileCoord` directly:
  `src/host_driver.cpp:386-411`.
- Kernels reconstruct halo coordinates from `a_lo`, `b_lo`, and `C`.

Example:

- K1 computes `a_start = coord.a_lo - C`, `b_start = coord.b_lo - C` at
  `src/kernel_sieve.cu:141-145`.
- K2 does the same at `src/kernel_mr.cu:40-42`.
- K4 reconstructs `(a,b)` from packed positions at
  `src/kernel_uf_v2.cu:118-124`.

### `CampaignConstants`

Reference: `cpp-campaign-v2/include/campaign/campaign_constants.h:54-151`.

`CampaignConstants` is the immutable runtime bundle derived from
`R_inner`, `R_outer`, and `K_SQ`.

It carries:

- squared radii
- prefilter bounds
- 128-bit geo-test bounds as hi/lo limbs
- reflected compile-time values such as `K_SQ`, `S`, `C`, offsets
- canonical hash and pinned MR witness hash surfaces

CUDA materialization:

- `DeviceCampaignConstants` mirrors the CPU struct in
  `include/cuda_campaign/campaign_constants.cuh:10-33`.
- A `static_assert` requires identical size to CPU `CampaignConstants` at
  `include/cuda_campaign/campaign_constants.cuh:28-29`.
- Upload to `__constant__` happens at `src/constants_upload.cu:169-176`.

### `Prime`

Reference: `cpp-campaign-v2/include/campaign/sieve.h:47-62`.

CPU `Prime` contains:

- `a`, `b`
- `norm_sq`
- `packed_pos = row * SIDE_EXP + col`

CUDA does not store `Prime` structs in production. It materializes the same
information in flat buffers:

- K2 bitmap: one bit per halo lattice position
- K3 `d_prime_pos`: compacted `packed_pos[]`
- K3 `d_prime_count`: number of compacted primes
- K4 reconstructs coordinates and norms when needed

Inferred semantics:

- `d_prime_pos` is the GPU equivalent of the CPU `Prime::packed_pos` stream.
- K3 row-major scatter gives the same canonical ordering the CPU obtains after
  sorting by `(a,b)`, because row is `b` and col is `a` in the canonical octant.

Relevant refs:

- CPU packing: `cpp-campaign-v2/src/sieve.cpp:60-70`.
- CPU final sort: `cpp-campaign-v2/src/sieve.cpp:116-120`.
- CUDA K3 scatter: `src/kernel_compact.cu:94-120`.

### `TileOp`

Reference:

- CPU layout: `cpp-campaign-v2/include/campaign/tileop.h:46-69`
- CUDA mirror: `include/cuda_campaign/tileop.cuh:11-31`

Wire format:

| Offset | Field | Size | Meaning |
|---:|---|---:|---|
| 0 | `n[4]` | 4 | port counts for faces I,O,L,R |
| 4 | `face_groups[192]` | 192 | dense group labels for ports |
| 196 | `inner_flags[16]` | 16 | bitset for labels touching inner boundary |
| 212 | `outer_flags[16]` | 16 | bitset for labels touching outer boundary |
| 228 | `tile_flags` | 1 | overflow, empty, tower-closing bits |
| 229 | `reserved[27]` | 27 | zero tail |

Design fact:

- CPU and CUDA both lock `sizeof(TileOp) == 256` and field offsets with
  `static_assert`s.
- `dispatch_tile_batch` asserts CPU/CUDA `TileOp` sizes match before running at
  `src/host_driver.cpp:957-958`.

Poster copy fragment:

> The 256-byte TileOp is the boundary object. Every CUDA optimization is
> subordinate to preserving these bytes.

## Compile-Time Constants and Capacity Choices

Reference: `include/cuda_campaign/constants.cuh:9-80`.

Important current CUDA constants:

- `K_SQ` is inherited from the CPU build.
- `S = 256`.
- `C = floor_isqrt(K_SQ)`.
- `SIDE_EXP = S + 1 + 2*C`; for K=36, `SIDE_EXP = 269`.
- `BITMAP_WORDS_PER_ROW = 9`; `BITMAP_WORDS = 2421`.
- `BLOCK_THREADS = 288`.
- `MAX_PRIMES_GPU = 8192` in the current CUDA path.
- `MAX_CANDIDATES_GPU = 16384`.
- `FJ64_TABLE_SIZE = 262144`.

Engineering note:

- CPU v2 still declares `MAX_PRIMES_GPU = 6144` in
  `cpp-campaign-v2/include/campaign/constants.h:57-62`.
- CUDA currently overrides to `8192` with a comment that fixed K1 can yield up
  to about 8000 primes: `include/cuda_campaign/constants.cuh:14-15`.
- This is not a math claim; it is an engineering capacity change. It increases
  memory pressure and should be part of the poster's "scale tax" story.

Static guard examples:

- K=36 requires `C == 6`, `SIDE_EXP == 269`,
  `NUM_BACKWARD_OFFSETS == 56` at `include/cuda_campaign/constants.cuh:70-72`.
- K=40 currently requires `C == 6`, `SIDE_EXP == 269`,
  `NUM_BACKWARD_OFFSETS == 64` at `include/cuda_campaign/constants.cuh:73-75`.
- `COLLAR == C` is enforced at `include/cuda_campaign/constants.cuh:76`.

## One-Time Device Loading

CUDA constant/table setup lives in `src/constants_upload.cu`.

Device constants:

- `c_campaign_constants`
- `c_split_barrett`
- `c_inert_barrett`
- `c_trial_primes`
- `c_bk_dr`, `c_bk_dc`

References:

- symbol declarations: `src/constants_upload.cu:22-27`
- sieve-table build and validation: `src/constants_upload.cu:63-129`
- FJ64 SHA-256 verification: `src/constants_upload.cu:132-143`
- constants upload: `src/constants_upload.cu:169-176`
- sieve/trial-prime upload: `src/constants_upload.cu:178-193`
- backward-offset upload: `src/constants_upload.cu:195-218`
- FJ64 table malloc/copy/free: `src/constants_upload.cu:220-234`

Poster diagram block:

```text
Host once per run:

CampaignConstants
  -> c_campaign_constants (__constant__)

split/inert Barrett tables
  -> c_split_barrett / c_inert_barrett (__constant__)

trial primes
  -> c_trial_primes (__constant__)

backward neighbor offsets
  -> c_bk_dr / c_bk_dc (__constant__)

FJ64 262k witness table
  -> d_fj64_table (global memory, 512 KB)
```

Engineering rigor:

- FJ64 table bytes are hashed before upload and must match the CPU pinned
  witness hash. This directly addresses silent MR witness drift.

## Kernel Pipeline

### K1: Candidate Sieve

Kernel:

- `src/kernel_sieve.cu:123-201`
- launcher: `src/kernel_sieve.cu:205-216`

Inputs:

- `d_coords`
- constant split/inert Barrett tables
- candidate capacity

Outputs:

- `d_cand_list[N * MAX_CANDIDATES_GPU]`
- `d_total_cands[N]`, clamped to capacity
- `d_raw_total_cands[N]`, unclamped diagnostic count
- `d_k1_overflow[N]`

Mechanics:

- one CUDA block per tile
- 288 threads per block
- active rows are the 269 halo rows
- each row builds local bitmap words `ws[9]`
- split primes mark two residue classes
- inert primes mark axis-aligned residues when applicable
- survivors are counted and scattered into a per-tile candidate list

Key refs:

- row sieve and residue marking: `src/kernel_sieve.cu:14-59`
- survivor count: `src/kernel_sieve.cu:61-73`
- candidate scatter: `src/kernel_sieve.cu:75-121`
- overflow/clamped writes: `src/kernel_sieve.cu:167-199`

Poster callout:

> K1 is cheap arithmetic plus table-driven elimination: reduce the lattice
> before spending MR cycles.

### K2: Miller-Rabin Bitmap

Kernel:

- `src/kernel_mr.cu:30-87`
- launcher: `src/kernel_mr.cu:91-100`

Inputs:

- `d_coords`
- `d_cand_list`
- `d_total_cands`
- `d_fj64_table`

Output:

- `d_bitmap[N * BITMAP_WORDS]`, one bit per halo lattice point

Mechanics:

- zeroes each tile bitmap cooperatively
- reconstructs candidate `(a,b)` from packed K1 row/col
- early exits outside canonical octant
- early exits outside annulus
- handles axis Gaussian primes separately
- for non-axis points: accepts `norm == 2`, or `norm % 4 == 1` plus FJ64 primality
- writes the canonical bitmap with row=`b`, col=`a`

Key refs:

- octant gate: `src/kernel_mr.cu:21-23`, `src/kernel_mr.cu:63-65`
- annulus gate: `src/kernel_mr.cu:25-28`, `src/kernel_mr.cu:67-72`
- canonical bitmap orientation: `src/kernel_mr.cu:74-85`
- axis prime branch: `src/kernel_mr.cu:75-80`
- split-prime norm branch: `src/kernel_mr.cu:82-85`

Optimized MR path:

- `is_prime_fj64_gpu` first rejects small/even/trial-prime composites:
  `include/cuda_campaign/gpu_math.cuh:167-177`.
- It runs base-2 Miller-Rabin first:
  `include/cuda_campaign/gpu_math.cuh:179-186`.
- It hashes `n` through two 64-bit mixer constants and indexes the 262,144-entry
  witness table:
  `include/cuda_campaign/gpu_math.cuh:188-193`.
- The table is uploaded as global device memory because it is 512 KB:
  `src/constants_upload.cu:220-227`.

Poster copy fragment:

> The hottest kernel is not guessing primes. It is proving enough non-primality
> fast enough, with cheap gates before expensive witnesses.

### K3: Compact Bitmap to Prime Positions

Kernel:

- `src/kernel_compact.cu:43-121`
- launcher: `src/kernel_compact.cu:129-146`

Inputs:

- `d_bitmap`

Outputs:

- `d_row_prefix[N * 270]`
- `d_prime_pos[N * MAX_PRIMES_GPU]`
- `d_prime_count[N]`

Mechanics:

- per-row popcount
- shared-memory exclusive scan over 269 rows
- row-prefix writeback
- bit scan via `__ffs`
- packed positions emitted as `row * SIDE_EXP + col`

Key refs:

- shared exclusive scan: `src/kernel_compact.cu:20-39`
- row popcount: `src/kernel_compact.cu:64-79`
- row-prefix writeback: `src/kernel_compact.cu:81-91`
- prime-position scatter: `src/kernel_compact.cu:94-120`

Poster callout:

> K3 turns a bitmap into a deterministic prime stream. Downstream byte parity
> begins here.

### K4: Local Connectivity, Dense Labels, Geo Flags

Kernel:

- `src/kernel_uf_v2.cu:162-349`
- launcher: `src/kernel_uf_v2.cu:353-379`

Inputs:

- `d_bitmap`
- `d_row_prefix`
- `d_prime_pos`
- `d_prime_count`
- `d_coords`
- optional prior K1 overflow

Outputs:

- `d_parent[N * MAX_PRIMES_GPU]`
- `d_prime_geo_bits[N * MAX_PRIMES_GPU]`
- `d_wire_label_by_raw_root[N * MAX_PRIMES_GPU]`
- `d_max_label[N]`
- `d_overflow[N]`
- `d_group_flags[N * 256]`

Mechanics:

1. Initialize parent array for each compacted prime.
2. For each prime, walk `NUM_BACKWARD_OFFSETS` integer neighbor offsets.
3. If a neighbor bit is set in the bitmap, map that bitmap position back to
   compacted prime index using row prefix + popcounts.
4. Atomic-union the two prime indices with smaller-root-wins.
5. Path-compress parent array.
6. Compute geo bits for each prime using integer norm-form tests.
7. Serially dense-remap visible roots in prime-index order.
8. Atomically OR group geo bits into dense-label buckets.

Key refs:

- bitmap test and compact index lookup:
  `src/kernel_uf_v2.cu:15-43`
- atomic find/union:
  `src/kernel_uf_v2.cu:45-77`
- neighbor walk:
  `src/kernel_uf_v2.cu:247-268`
- path compression:
  `src/kernel_uf_v2.cu:271-273`
- geo bits:
  `src/kernel_uf_v2.cu:84-125`, `src/kernel_uf_v2.cu:276-285`
- serial dense remap:
  `src/kernel_uf_v2.cu:288-328`
- group flag OR:
  `src/kernel_uf_v2.cu:338-348`

Reference CPU semantics:

- smaller-root-wins DSU:
  `cpp-campaign-v2/include/campaign/union_find.h:3-12`,
  `cpp-campaign-v2/src/union_find.cpp:55-65`
- local UF and dense visible root remap:
  `cpp-campaign-v2/src/tileop.cpp:166-232`

Important inferred semantics:

- The serial dense-remap block in K4 is intentionally not parallelized because
  CPU labels are assigned by first visible root in ascending prime-index order.
- That seriality is a correctness feature, not a missed parallelization trick.

Poster callout:

> K4 is where graph structure becomes wire labels. The fastest wrong label is
> still a snapshot mismatch.

### K5a: Face-Strip Encoding Prep

Kernel:

- `src/kernel_face_encode_v2.cu:237-355`
- launcher: `src/kernel_face_encode_v2.cu:359-370`

Inputs:

- `d_coords`
- `d_prime_pos`
- `d_prime_count`
- `d_remap_overflow`
- `d_parent`
- `d_wire_label_by_raw_root`
- `d_group_flags`

Outputs:

- partial `d_tileops`
- debug/intermediate face arrays:
  - `d_face_indices`
  - `d_face_counts`
  - `d_face_roots`
  - `d_face_reps`
  - `d_face_rep_counts`

Mechanics:

- writes zero-payload `OVERFLOW_BIT` TileOp if K4 overflowed
- writes zero-payload `EMPTY_BIT` TileOp if no primes exist
- initializes a normal zero TileOp otherwise
- unpacks dense group flags into `inner_flags` and `outer_flags`
- uses four warps to filter face-strip prime indices for faces I,O,L,R
- thread 0 runs per-face mini DSU and representative selection

Key refs:

- zero/flag TileOp writer: `src/kernel_face_encode_v2.cu:11-18`
- `inner_flags` / `outer_flags` unpacking:
  `src/kernel_face_encode_v2.cu:20-41`
- face coordinates:
  `src/kernel_face_encode_v2.cu:56-93`
- face-strip predicate:
  `src/kernel_face_encode_v2.cu:131-136`
- warp face filtering:
  `src/kernel_face_encode_v2.cu:138-163`
- face DSU and representative selection:
  `src/kernel_face_encode_v2.cu:165-235`
- overflow/empty/normal branches:
  `src/kernel_face_encode_v2.cu:275-295`
- warp-per-face launch inside block:
  `src/kernel_face_encode_v2.cu:331-352`

Reference CPU semantics:

- face-strip filter:
  `cpp-campaign-v2/src/tileop.cpp:86-97`
- face sub-DSU and port representatives:
  `cpp-campaign-v2/src/tileop.cpp:99-155`

Poster copy fragment:

> K5 replaces the old one-dimensional port intuition with actual face-strip
> connected components.

### K5b: Face Sort and TileOp Pack

Kernel:

- `src/kernel_face_sort_pack.cu:52-151`
- launcher: `src/kernel_face_sort_pack.cu:155-161`

Inputs:

- `d_face_reps`
- `d_face_rep_counts`
- partial `d_tileops`

Output:

- completed `d_tileops[N]`

Mechanics:

- shared `sort_scratch[4][256]`
- one warp per face
- sentinel-fill unused slots
- bitonic sort by `(h, p_perp, global_wire_label)`
- set `n[face]`
- thread 0 concatenates sorted face labels into `face_groups`
- if any face has more than 255 ports or total ports exceed 192, emit
  zero-payload `OVERFLOW_BIT`

Key refs:

- sort key comparator:
  `src/kernel_face_sort_pack.cu:19-24`
- zero payload helpers:
  `src/kernel_face_sort_pack.cu:33-50`
- overflow precheck:
  `src/kernel_face_sort_pack.cu:83-101`
- bitonic sort:
  `src/kernel_face_sort_pack.cu:103-138`
- face-group packing:
  `src/kernel_face_sort_pack.cu:141-150`

Reference CPU semantics:

- port sort and final pack:
  `cpp-campaign-v2/src/tileop.cpp:150-155`,
  `cpp-campaign-v2/src/tileop.cpp:320-347`

Poster callout:

> K5b is a byte-order machine: sort ports, count ports, write labels, leave
> every unused byte zero.

## Host Orchestration

### Public Host API

Reference: `include/cuda_campaign/host_driver.h:14-68`.

`DispatchConfig` controls:

- host chunk size
- device slab size override
- device memory budget
- safety margin
- number of ring slots

Defaults:

- `host_chunk_tiles = 200000`
- `device_budget_bytes = 8.5 GiB`
- `device_safety_bytes = 1 GiB`
- `ring_slots = 3`

`DispatchStats` reports:

- tile/chunk/slab counts
- peak phase byte estimates
- pinned host bytes
- stream count
- device name
- overflow counters
- first overflow diagnostics

### Device Workspace

Reference: `src/host_driver.cpp:386-430`.

Current production `DeviceWorkspace` allocates flat arrays for:

- coords
- candidate list
- K1 counts and overflow flags
- bitmap
- K3 row prefixes and prime positions
- K4 parent, geo bits, labels, overflows, group flags
- K5 face intermediate arrays
- face representatives

The byte-estimation helper includes the same pooled buffers at
`src/host_driver.cpp:223-260`.

Improvement area:

- Planning documents call for a two-phase overlay so K1/K2 memory can be reused
  for K3/K4/K5 buffers. The current `DeviceWorkspace` allocates the flat pool
  and adapts slab size to fit budget instead. This is simpler and robust, but it
  increases per-tile memory pressure and may reduce slab size on smaller GPUs.

### Dispatch Lifecycle

Production path: `src/host_driver.cpp:929-1277`.

Steps:

1. validate pointers/config/count:
   `src/host_driver.cpp:935-955`
2. assert CPU/CUDA TileOp layout parity:
   `src/host_driver.cpp:957-958`
3. upload constants and FJ64 table:
   `src/host_driver.cpp:960-963`
4. create H2D, compute, and D2H streams:
   `src/host_driver.cpp:966`
5. choose device slab capacity from memory budget:
   `src/host_driver.cpp:967-970`
6. allocate pinned host ring slots:
   `src/host_driver.cpp:971-979`
7. for each host chunk and device slab:
   - copy coords into pinned slot
   - async H2D coords
   - compute stream waits on H2D event
   - launch K1..K5
   - optionally download diagnostics for stats
   - async D2H TileOps
   - mark pending ring slot
8. drain pending D2H into final host `TileOp` array:
   `src/host_driver.cpp:1246-1248`
9. synchronize streams and return stats:
   `src/host_driver.cpp:1250-1259`

Important nuance:

- The host driver overlaps H2D, compute, and D2H inside CUDA streams.
- `campaign_main_cuda` currently waits for `dispatch_tile_batch` to return the
  full `tileops` vector, then runs the CPU compositor afterward at
  `apps/campaign_main_cuda.cpp:435-469`.
- Therefore, current code has stream overlap for transfers but not full
  GPU-compute versus CPU-compositor overlap.

Poster callout:

> The host is a conveyor: tiles in, TileOps out. The compositor still consumes a
> finished byte stream in canonical order.

## Campaign Lifecycle

Candidate poster swimlane:

```text
CPU lane:
  parse CLI
  validate K_SQ / radii / annulus thickness
  build CampaignConstants
  build Grid
  verify Grid invariants
  resolve Region
  enumerate active TileCoord[]

GPU lane:
  upload constants and tables
  for each chunk/slab:
    H2D TileCoord[]
    K1 candidates
    K2 prime bitmap
    K3 compacted primes
    K4 parent labels + flags
    K5 TileOp bytes
    D2H TileOp[]

CPU lane:
  compositor.ingest_column(...)
  compositor.finalize()
  write_snapshot(...)
  print timings, hashes, overflows, verdict
```

References:

- CLI and validation: `apps/campaign_main_cuda.cpp:264-389`
- grid/region/tile enumeration: `apps/campaign_main_cuda.cpp:400-433`
- CUDA dispatch call: `apps/campaign_main_cuda.cpp:435-447`
- compositor: `apps/campaign_main_cuda.cpp:450-469`
- snapshot: `apps/campaign_main_cuda.cpp:471-479`
- printed stats/hashes/verdict:
  `apps/campaign_main_cuda.cpp:482-535`

## Tile Lifecycle

Candidate diagram:

```text
TileCoord(i,j,a_lo,b_lo)
  |
  | K1: halo rows -> candidate packed(row,col)
  v
d_cand_list + d_total_cands
  |
  | K2: octant/annulus gates + FJ64 MR
  v
d_bitmap, canonical row=b col=a
  |
  | K3: row prefix + bit scan
  v
d_prime_pos[] + d_prime_count
  |
  | K4: backward-offset neighbor search + atomic smaller-root UF
  v
d_parent[] raw roots
  |
  | K4: visible-root dense remap + geo flags
  v
d_wire_label_by_raw_root[] + d_group_flags[]
  |
  | K5a: face-strip mini-DSU + representatives
  v
d_face_reps[] + d_face_rep_counts[]
  |
  | K5b: sort by (h,p_perp,label), pack
  v
TileOp{256 bytes}
```

## Batch / Slab Lifecycle

In current implementation:

- host chunks are logical chunks requested by `DispatchConfig`
- device slabs are the actual per-launch capacity chosen to satisfy device
  memory budget
- a host chunk may be internally split into multiple device slabs
- three pinned ring slots carry H2D/D2H staging buffers
- D2H completion is event-tracked and drained into the final output vector

Relevant refs:

- slab sizing: `src/host_driver.cpp:262-284`
- ring slot: `src/host_driver.cpp:349-361`
- pending D2H: `src/host_driver.cpp:363-383`
- main slab loop: `src/host_driver.cpp:999-1240`

Poster copy fragment:

> The unit of math is a tile. The unit of GPU throughput is a slab.

## Error, Overflow, and Conservative Handling

### K1 Candidate Overflow

K1 tracks raw candidate count separately from clamped candidate count:

- raw count: `d_raw_total_cands`
- clamped count: `d_total_cands`
- overflow bit: `d_k1_overflow`

Refs:

- overflow is set on over-capacity append:
  `src/kernel_sieve.cu:167-181`
- final clamped/raw counts:
  `src/kernel_sieve.cu:187-199`

### K4 Prime/Group Overflow

K4 propagates prior K1 overflow:

- reads `d_prior_overflow` at `src/kernel_uf_v2.cu:204-210`
- exits early if prior overflow is set:
  `src/kernel_uf_v2.cu:229-231`

K4 sets overflow if:

- `prime_count > MAX_PRIMES_GPU`:
  `src/kernel_uf_v2.cu:233-238`
- dense visible label count exceeds `MAX_GROUPS_PER_TILE`:
  `src/kernel_uf_v2.cu:315-327`

### K5 Port Overflow

K5b emits overflow if:

- any face has more than 255 ports
- total ports exceed `MAX_PORTS_PER_TILE == 192`

Refs:

- `src/kernel_face_sort_pack.cu:83-101`

### Overflow TileOp Semantics

CPU reference:

- zeroed `TileOp` with only `OVERFLOW_BIT` set:
  `cpp-campaign-v2/src/tileop.cpp:158-162`
- CPU header promises overflow zeroes payload fields:
  `cpp-campaign-v2/include/campaign/tileop.h:81-86`

CUDA:

- `write_flag_tileop` zeroes all 256 bytes then sets `tile_flags`:
  `src/kernel_face_encode_v2.cu:11-18`
- K5b also zeroes and sets `OVERFLOW_BIT` on port overflow:
  `src/kernel_face_sort_pack.cu:94-97`

Conservative behavior:

- `test_k1_overflow_spanning.cpp` verifies forced K1 overflow propagates to
  `OVERFLOW_BIT` and makes the compositor return `SPANNING`:
  `tests/test_k1_overflow_spanning.cpp:41-63`.

Poster copy fragment:

> Overflow is not silence. Overflow is an explicit conservative witness.

## Engineering Rigor

### Layout Rigor

- CPU `TileOp` locks offsets:
  `cpp-campaign-v2/include/campaign/tileop.h:60-69`.
- CUDA `TileOp` mirrors and locks offsets:
  `include/cuda_campaign/tileop.cuh:18-31`.
- Host dispatch asserts CPU/CUDA `TileOp` sizes match:
  `src/host_driver.cpp:957-958`.

### Constant Rigor

- CUDA constants import CPU constants:
  `include/cuda_campaign/constants.cuh:5-22`.
- K-specific geometry static asserts:
  `include/cuda_campaign/constants.cuh:70-79`.
- `DeviceCampaignConstants` size mirrors CPU:
  `include/cuda_campaign/campaign_constants.cuh:28-29`.

### Witness Rigor

- CPU pins the FJ64 witness table SHA-256:
  `cpp-campaign-v2/include/campaign/campaign_constants.h:25-29`.
- CUDA verifies table hash before upload:
  `src/constants_upload.cu:132-143`.
- Campaign main prints `mr_witness_sha256`:
  `apps/campaign_main_cuda.cpp:497-499`.

### Runtime Rigor

- Every kernel launch in host orchestration is followed by `cudaGetLastError`
  through `check_last_launch`: `src/host_driver.cpp:200-202`,
  `src/host_driver.cpp:1046-1117`.
- `DispatchStats` records overflow counts and first overflow diagnostics:
  `include/cuda_campaign/host_driver.h:23-50`,
  `src/host_driver.cpp:1122-1223`.
- Optional `CUDA_CAMPAIGN_GROUP_DUMP` emits a CSV for first K4 group overflow:
  `src/host_driver.cpp:286-347`,
  `src/host_driver.cpp:1206-1220`.

### Verification Surfaces

Tests and gates:

- CMake builds CUDA library, apps, and tests:
  `CMakeLists.txt:24-120`, `CMakeLists.txt:122-238`.
- `cuda_vs_cpu_diff` compares parent parity, M4 debug parity, and full K1-K5
  `TileOp` bytes:
  `apps/cuda_vs_cpu_diff.cpp:382-399`,
  `apps/cuda_vs_cpu_diff.cpp:402-478`.
- full TileOp parity test includes byte-offset diagnostics:
  `tests/test_full_tileop_parity.cpp:47-122`.
- K1 overflow conservative spanning:
  `tests/test_k1_overflow_spanning.cpp:41-63`.
- snapshot SHA gate compares CPU and CUDA snapshots:
  `scripts/run_snapshot_sha_gate.sh:211-262`,
  `tests/test_snapshot_sha_R80M.cpp:244-259`.
- CUDA golden manifest stores per-batch SHA hashes:
  `golden/manifest.json:1-92`.
- golden validator regenerates batch JSON and `cmp`s against tracked goldens:
  `golden/validate_golden.sh:40-79`.

Poster copy fragment:

> Correctness is not inferred from speed. It is fenced by byte equality, hashes,
> static offsets, and conservative failure modes.

## Performance Profile

Source: `profiling/2026-04-23-kernel-profile.md`.

Run context:

- RTX 4090
- K=36
- `R_inner=85,000,000`
- `R_outer=85,008,192`
- full octant
- 8,677,267 active tiles
- overflows all zero
- verdict `SPANNING`

Throughput:

- chunk sizes 50k, 100k, 200k were materially similar:
  `profiling/2026-04-23-kernel-profile.md:30-36`
- CUDA-only throughput was about 96.8k tiles/s for the profiled settings.

Kernel breakdown:

| Kernel | GPU time share | Note |
|---|---:|---|
| `kernel_mr` | 38.9% | primary bottleneck |
| `kernel_face_encode_v2` | 28.7% | secondary bottleneck |
| `kernel_uf_v2` | 16.5% | material |
| `kernel_sieve` | 14.7% | material |
| `kernel_face_sort_pack` | 0.7% | not first-order |
| `kernel_compact` | 0.5% | not first-order |

Source lines: `profiling/2026-04-23-kernel-profile.md:54-64`.

Transfer summary:

- D2H total about 0.105 s
- H2D total about 0.011 s
- transfer time was not a bottleneck in that profile

Source lines: `profiling/2026-04-23-kernel-profile.md:65-74`.

Optimization recommendation from profile:

- prioritize MR hot path first
- use Nsight Compute before generic register tuning
- second priority is `kernel_face_encode_v2` rewrite/specialization

Source lines: `profiling/2026-04-23-kernel-profile.md:76-88`.

Poster copy fragment:

> The profile says the expensive truth is primality, then face semantics.
> Transfers are already background noise.

## Improvement Areas

### 1. Two-Phase Device Memory Overlay

Current code:

- allocates a flat `DeviceWorkspace` containing K1/K2 and K3/K4/K5 buffers
  simultaneously: `src/host_driver.cpp:386-430`
- sizes slabs using pooled flat allocation:
  `src/host_driver.cpp:223-260`, `src/host_driver.cpp:262-284`

Planning intent:

- host-pipeline notes call for overlaying K1/K2 candidate/count buffers with
  later K3/K4/K5 buffers:
  `planning/2026-04-21-host-pipeline-scaffold.md:149-205`
- performance plan also marks two-phase overlay as mandatory for memory pressure:
  `planning/2026-04-21-codex-performance.md:160-176`,
  `planning/2026-04-21-codex-performance.md:196-204`

Poster framing:

> Simpler allocation got the pipeline working. Overlay is the next memory-leverage
> move.

### 2. MR Hot Path Specialization

Current code already has good early exits:

- octant/annulus before MR: `src/kernel_mr.cu:63-72`
- axis branch: `src/kernel_mr.cu:75-80`
- norm class check before MR: `src/kernel_mr.cu:82-85`
- trial primes before Montgomery witnesses:
  `include/cuda_campaign/gpu_math.cuh:167-177`

But profile says MR is still the largest share. Next work should be evidence-led:

- Nsight Compute on `kernel_mr`
- separate integer throughput, occupancy, register pressure, table/cache behavior
- only then tune register caps or specialize candidates

### 3. Face Encode Cost

`kernel_face_encode_v2` is second-largest in profile. Likely reasons visible in
code:

- face filtering is warp-parallel
- face DSU and representative construction are serial under `tid == 0`:
  `src/kernel_face_encode_v2.cu:340-352`
- the O(n^2) face DSU loop is simple and deterministic:
  `src/kernel_face_encode_v2.cu:179-188`

Possible poster-safe phrasing:

> K5 is semantically dense: it pays for the move from old face heuristics to true
> face-strip connected components.

### 4. Compositor Overlap

Current `campaign_main_cuda` dispatches all TileOps before CPU compositor ingest:

- dispatch: `apps/campaign_main_cuda.cpp:435-447`
- compositor after dispatch: `apps/campaign_main_cuda.cpp:450-469`

The host driver overlaps H2D/compute/D2H, but CPU compositor overlap is not yet
visible in this top-level flow.

Improvement direction:

- column-aligned chunks or completed-column buffering
- ingest CPU compositor while later GPU slabs compute
- preserve final `tileops` vector order for snapshot parity

### 5. Capacity and Memory Documentation Drift

Current CUDA uses:

- `MAX_PRIMES_GPU = 8192`
- `MAX_CANDIDATES_GPU = 16384`

Several planning docs discuss 6144-prime sizing because that was the CPU v2
constant and early CUDA target. Poster should present 8192/16384 as current code
facts and 6144 as historical/reference context.

## Visual Design Blocks

### Block A: "Boundary Object"

```text
TileOp = 256 bytes

[0..3]     n[4]
[4..195]   face_groups[192]
[196..211] inner_flags[16]
[212..227] outer_flags[16]
[228]      tile_flags
[229..255] reserved zero
```

Caption:

> Every kernel exists to fill this object deterministically.

### Block B: "Five Kernel Story"

```text
K1: candidate sieve
K2: primality bitmap
K3: compact prime stream
K4: local UF + labels + geo bits
K5: face ports + byte pack
```

Caption:

> Split kernels preserve separate register and memory personalities.

### Block C: "Trust Ladder"

```text
static_assert layout
  -> constant hash checks
  -> K1-K4 parity
  -> K5 byte parity
  -> snapshot SHA gate
  -> Tsuchimura known-answer gate
```

Caption:

> Trust is accumulated by gates, not granted by GPU speed.

### Block D: "Hot Path"

```text
candidate
  -> octant gate
  -> annulus gate
  -> axis branch?
  -> norm class gate
  -> trial-prime reject
  -> base-2 MR
  -> hashed FJ64 witness MR
  -> bitmap bit
```

Caption:

> The MR path is optimized as a sequence of cheap exits before expensive proof.

### Block E: "Conservative Overflow"

```text
too many candidates
or too many primes
or too many groups
or too many ports
  -> zero TileOp payload
  -> tile_flags = OVERFLOW_BIT
  -> compositor treats as spanning
```

Caption:

> Overflow is a deliberate conservative signal.

## Poster Copy Fragments

- "CUDA is not the theorem engine. CUDA is the TileOp foundry."
- "The CPU defines the grid and verdict. The GPU manufactures local witnesses."
- "One tile, one block, five kernels, one 256-byte output."
- "The byte layout is the contract. Performance negotiates around it."
- "K1 removes obvious composites. K2 spends the expensive primality proof."
- "K3 turns a bitmap into a deterministic stream."
- "K4 turns local geometry into stable wire labels."
- "K5 turns face-strip connectivity into portable ports."
- "Conservative overflow means no silent optimism."
- "The profile points at MR first, face encoding second."

## Suggested Poster Hierarchy

1. Hero title:
   "CUDA v2: A TileOp Factory for the Gaussian Moat Campaign"

2. Main diagram:
   CPU grid -> CUDA K1-K5 -> CPU compositor/snapshot

3. Left rail:
   Data structures and byte layout

4. Center rail:
   Kernel pipeline

5. Right rail:
   verification gates and rigor

6. Bottom strip:
   performance profile and next improvements

## Source Index

Most important source anchors:

- CUDA constants: `include/cuda_campaign/constants.cuh`
- CUDA TileOp layout: `include/cuda_campaign/tileop.cuh`
- kernel launch API: `include/cuda_campaign/kernels.cuh`
- host dispatch API: `include/cuda_campaign/host_driver.h`
- constants/table upload: `src/constants_upload.cu`
- K1 sieve: `src/kernel_sieve.cu`
- K2 MR: `src/kernel_mr.cu`
- K3 compact: `src/kernel_compact.cu`
- K4 UF/labels/flags: `src/kernel_uf_v2.cu`
- K5 face encode: `src/kernel_face_encode_v2.cu`
- K5 sort/pack: `src/kernel_face_sort_pack.cu`
- host orchestration: `src/host_driver.cpp`
- campaign CLI: `apps/campaign_main_cuda.cpp`
- diff tool: `apps/cuda_vs_cpu_diff.cpp`
- golden dump: `apps/cuda_golden_dump.cpp`
- profile: `profiling/2026-04-23-kernel-profile.md`
- CPU TileOp reference: `../cpp-campaign-v2/include/campaign/tileop.h`,
  `../cpp-campaign-v2/src/tileop.cpp`
- CPU grid reference: `../cpp-campaign-v2/include/campaign/grid.h`
- CPU sieve reference: `../cpp-campaign-v2/include/campaign/sieve.h`,
  `../cpp-campaign-v2/src/sieve.cpp`
- CPU compositor reference: `../cpp-campaign-v2/include/campaign/compositor.h`

## Final Poster Takeaway

The architecture is serious because it separates concerns:

- C++ owns campaign semantics, global stitching, and durable output.
- CUDA owns local per-tile throughput.
- The `TileOp` byte layout is the shared contract.
- Static asserts, witness hashes, parity tests, goldens, snapshot SHA gates, and
  conservative overflow form the trust boundary.

Best single-sentence poster close:

> This CUDA path is engineered to be fast only after it is byte-identical.
