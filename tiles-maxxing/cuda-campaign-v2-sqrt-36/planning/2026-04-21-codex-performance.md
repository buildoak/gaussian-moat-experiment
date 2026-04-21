---
date: 2026-04-21
engine: codex-gpt-5.4
angle: performance
status: complete
---

# 1. Executive Summary

Performance thesis: keep the v1 five-kernel split and one-block-per-tile model, but make K4/K5 do exactly the new v2 semantic work and no more. K1/K2/K3 are already the throughput engine: the blueprint marks them inherited unchanged (`methodology/lemmas_v2/campaign-blueprint.md:271-275`), v1 separated them to decouple register allocation (`docs/supportive/2026-04-10-multi-kernel-architecture.md:30-33`), and the prior launch shape is one block per tile, 288 threads per block, five sequential kernels (`docs/supportive/2026-04-10-multi-kernel-architecture.md:35-43`).

Biggest lever: preserve high-occupancy K4/K5 by keeping v2 dense-label, geo-flag, and face-strip-UF scratch small. Do not fuse K2 with anything: MR is register/ILP-bound, uses 46 registers in v1, and dominates timing (`docs/supportive/2026-04-10-multi-kernel-architecture.md:50-68`, `docs/supportive/2026-04-10-multi-kernel-architecture.md:146-155`). Do not let new v2 flag logic push K4 past the 40-register class or K5 past the 13 KB shared-memory budget promised by the blueprint (`methodology/lemmas_v2/campaign-blueprint.md:315-334`).

Target: match or exceed 155k tiles/s on RTX 4090 after byte parity. With v2 overheads, realistic steady state is 145k-165k tiles/s, with 155k as the commit target. Exact current grid analytics for K=36, `R_inner=80,000,000`, `R_outer=80,008,192`: `i_min=0`, `i_max=220,993`, `220,994` columns, `8,166,667` active tiles; this follows current `Grid::build` prefix totals (`tiles-maxxing/cpp-campaign-v2/src/grid.cpp:610-620`) and matches the blueprint's 220,994 tower note (`methodology/lemmas_v2/campaign-blueprint.md:151`).

# 2. Per-Stage Plan for Stages 5-9

## Stage 5: Per-Tile Sieve Candidate Generation (K1)

Stage I/O: CPU v2 `process_tile` starts with `sieve_tile(coord, constants)` (`tiles-maxxing/cpp-campaign-v2/src/process_tile.cpp:15-19`). The CPU sieve scans the halo-expanded closed tile from `coord - C` through `coord + S + C` (`tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:88-96`), filters split Gaussian primes by norm primality and annulus membership (`tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:97-113`), then sorts by `(a,b)` (`tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:116-119`).

Prior-kernel perf profile: v1 K1 uses 288 threads/block, one active row per thread, split/inert Barrett tables in constant memory, shared `atomicAdd` for candidate scatter (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_sieve.cu:1-4`, `tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_sieve.cu:13-19`, `tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_sieve.cu:116-162`). Measured Orin profile was 98.2 ms for 2000 tiles, 16.4% total, 30 registers (`docs/supportive/2026-04-10-multi-kernel-architecture.md:146-155`).

Occupancy: proposed `blockDim=288`, `gridDim=chunk_tiles`. With 30 regs/thread, registers/block = 8,640; RTX 4090 arithmetic ceiling is 5 blocks/SM by 1536-thread cap, 1,440 resident threads, 93.75% thread occupancy. Shared memory is only a 4 B per-block counter, so occupancy is thread-cap, not smem-cap.

Memory layout: keep v1 `d_cand_list[N * MAX_CANDIDATES_GPU]` as row-major per tile. V1 used 24,576 B/tile for 6144 packed candidates (`docs/supportive/2026-04-10-multi-kernel-architecture.md:103-107`); v2 CPU constants also set `MAX_PRIMES_GPU=6144` (`tiles-maxxing/cpp-campaign-v2/include/campaign/constants.h:57-62`). K1 writes are not fully coalesced because scatter is survivor-sparse, but they are append-only within a contiguous per-tile segment. Constant-memory table broadcast is the right tradeoff because all blocks walk the same split/inert table order.

V2 changes: for K=36, `C=6` and `SIDE_EXP=269` by CPU constants (`tiles-maxxing/cpp-campaign-v2/include/campaign/constants.h:102-114`), not v1's 271-row comment. Keep K1 mathematically aligned with CPU v2's annulus and octant clipping: candidates outside `x>=0`, `y>=x`, and `[R_inner_sq,R_outer_sq]` must be suppressed before K2 writes bitmap, matching the CPU sieve's octant lower bound `b_lo=max(a,b_begin,0)` and annulus check (`tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:94-103`).

Proposed launch config: `kernel_sieve_v2<<<chunk_tiles, 288, 0, stream>>>`, `--maxrregcount=40` initially, accept actual <=34. Chunk target 200,000 tiles per stream ring, matching blueprint burst guidance (`methodology/lemmas_v2/campaign-blueprint.md:108`).

Kernel fusion decision: do not fuse K1+K2. K1 is table-marking and scatter-heavy; K2 is Montgomery MR and register/ILP-heavy. The prior split exists specifically to avoid cross-phase register coupling (`docs/supportive/2026-04-10-multi-kernel-architecture.md:120-130`).

Byte-parity gate: for each golden tile, compare K1+K2+K3 compacted prime `(a,b,norm_sq,packed_pos)` order against CPU `sieve_tile`, whose final ordering is `(a,b)` (`tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:116-119`). Only then allow TileOp byte comparison.

## Stage 6: Miller-Rabin Prime Verification and Bitmap (K2)

Stage I/O: K2 consumes K1 candidate positions and emits the halo bitmap used by compact and UF. CPU v2 uses `campaign::is_prime` for axis and split norm checks (`tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:73-76`, `tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:97-103`).

Prior-kernel perf profile: v1 K2 runs FJ64_262k deterministic MR: base-2 plus table witness, table size 512 KB, cached in L2 (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_mr.cu:1-4`, `docs/supportive/2026-04-10-multi-kernel-architecture.md:50-66`). It zeroes bitmap cooperatively and sets primes by global `atomicOr` (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_mr.cu:14-17`, `tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_mr.cu:40-69`). Measured Orin profile was 323.9 ms for 2000 tiles, 54.0% total, 46 regs (`docs/supportive/2026-04-10-multi-kernel-architecture.md:146-155`).

Occupancy: proposed `blockDim=288`, actual target 46-48 regs/thread, no cap unless sm_89 measurement proves a 44 cap wins. At 46 regs/thread, registers/block = 13,248; RTX 4090 arithmetic ceiling is 4 blocks/SM, 1,152 resident threads, 75% thread occupancy. This is acceptable because MR needs ILP; v1 notes capping spills and hurts (`docs/supportive/2026-04-10-multi-kernel-architecture.md:67-68`).

Memory layout: `d_bitmap[N * BITMAP_WORDS]`, one bit per halo lattice point. V1 bitmap was 9,756 B/tile for 2439 words (`docs/supportive/2026-04-10-multi-kernel-architecture.md:108`); v2 K=36 `SIDE_EXP=269` makes `BITMAP_WORDS_PER_ROW=9`, `BITMAP_WORDS=2421`, 9,684 B/tile. K2 writes contiguous zero-fill and sparse atomic bit sets. FJ64 table should stay global read-only, warmed in L2; copying it into constant memory is wrong at 512 KB.

V2 changes: the annulus-thickness and octant gates reduce candidates on border tiles but introduce divergence near `y=x` and the inner/outer arcs. Implement gates before MR when `norm_sq` is out of annulus or off-octant; this saves expensive Montgomery work and is byte-equivalent to CPU v2's `norm_in_annulus` and `b>=a` behavior (`tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:55-58`, `tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:94-103`).

Proposed launch config: `kernel_mr_v2<<<chunk_tiles, 288, 0, stream>>>`, no `--maxrregcount` first pass; tune 44 vs uncapped on 4090 after parity. `gridDim=chunk_tiles`. Keep `d_fj64_table` uploaded once at worker init; v1 allocates and uploads once in `TileBatchDeviceMemory::allocate` (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu:283-286`).

Kernel fusion decision: do not fuse K2 with K3. K3 is ~0.3% on Orin (`docs/supportive/2026-04-10-multi-kernel-architecture.md:146-155`), so fusion buys little and risks MR register pressure.

Byte-parity gate: bitmap popcount must equal CPU sieve prime count for sampled tiles; compacted positions after K3 must decode to CPU primes exactly.

## Stage 7: Compact Bitmap to Prime List (K3)

Stage I/O: K3 emits ordered prime positions used by K4/K5. CPU v2's downstream TileOp builder assumes prime vectors are sorted deterministically; it sorts again as a guard (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:215-227`).

Prior-kernel perf profile: v1 K3 uses per-row popcount, Hillis-Steele exclusive scan in shared memory, then ctz/ffs scatter (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_compact.cu:1-3`, `tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_compact.cu:13-31`, `tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_compact.cu:54-107`). Measured Orin profile was 1.9 ms for 2000 tiles, 0.3% total, 21 regs (`docs/supportive/2026-04-10-multi-kernel-architecture.md:146-155`).

Occupancy: `blockDim=288`, 21 regs/thread = 6,048 regs/block; 5 blocks/SM by thread cap, 93.75% occupancy. Dynamic smem is `(ACTIVE_ROWS+1)*2`; for v2 K=36, 270*2 = 540 B.

Memory layout: read K2 bitmap sequentially by row. Write `d_row_prefix[N*(SIDE_EXP+1)]` and `d_prime_pos[N*MAX_PRIMES_GPU]`. Use `uint16_t` row-prefix because `MAX_PRIMES_GPU=6144` fits; use `uint32_t` packed positions to avoid K5 division ambiguity. K3's global writes are contiguous per row and per tile.

V2 changes: set capacity to CPU v2 `MAX_PRIMES_GPU=6144`, not v1's older 2560 path; CPU constants explicitly size this from empirical profiling (`tiles-maxxing/cpp-campaign-v2/include/campaign/constants.h:57-62`). If `prime_count > 6144`, set a per-tile overflow flag for K5 to emit `OVERFLOW_BIT`; do not clamp silently.

Proposed launch config: `kernel_compact_v2<<<chunk_tiles, 288, 540, stream>>>`, `--maxrregcount=32`. Keep separate.

Kernel fusion decision: leave separate until after v2 parity. K3 is too cheap to optimize before K4/K5 are stable.

Byte-parity gate: decoded `(row,col)` sequence must match CPU `packed_pos_for`, whose packing is `row * SIDE_EXP + col` (`tiles-maxxing/cpp-campaign-v2/src/sieve.cpp:60-70`).

## Stage 8: Local UF, Dense Labels, Geo Flags (K4)

Stage I/O: CPU v2 local connectivity unions every pair of primes within squared distance `K` (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:46-50`, `tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:159-169`), then dense-remaps raw roots into labels 1..128 with overflow at cap (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:181-203`). CPU geo flags are computed before TileOp build (`tiles-maxxing/cpp-campaign-v2/src/process_tile.cpp:22-28`) using norm-form predicates (`tiles-maxxing/cpp-campaign-v2/src/geo_tests.cpp:22-67`).

Prior-kernel perf profile: v1 K4 uses 288-thread lock-free union-find, backward-offset neighbor discovery, bitmap lookup, row-prefix popcount lookup, `atomicCAS` smaller-root-wins, and final path compression (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_uf.cu:1-5`, `tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_uf.cu:41-75`, `tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_uf.cu:110-137`). Measured Orin profile was 161.5 ms for 2000 tiles, 26.9% total, 34 regs (`docs/supportive/2026-04-10-multi-kernel-architecture.md:146-155`).

Occupancy: baseline 34 regs/thread = 9,792 regs/block; 5 blocks/SM by thread cap, 93.75% occupancy. V2 target is <=40 regs/thread. At 40 regs/thread, 11,520 regs/block, still 5 blocks/SM by registers and thread cap. Shared memory budget for K4 must stay <=8 KB/block: 32 B group flags, <=512 B dense-label hash/table metadata, and small counters. Do not stage the full parent or prime arrays in shared memory; global parent is already the v1 design (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_uf.cu:91-107`).

Memory layout: inputs are K2 bitmap, K3 row-prefix, K3 prime-pos, prime-count. Output `d_parent[N*MAX_PRIMES_GPU]` and new `d_group_flags[N*32]`. Parent remains `uint16_t`; dense labels are 1-based in TileOp but can be stored 0-based internally. `d_group_flags` is 32 B/tile: 128 labels * 2 bits.

V2 changes: add geo tests after path compression, exactly as blueprint K4 specifies (`methodology/lemmas_v2/campaign-blueprint.md:277-315`). Use constant campaign fields for `R_inner_sq`, `R_outer_sq`, prefilter bounds, and i128 limits (`methodology/lemmas_v2/campaign-blueprint.md:190-204`). The prefilter must use `ceil_isqrt(K)`, not `C=floor_isqrt(K)` (`methodology/lemmas_v2/campaign-blueprint.md:76-84`). Atomic OR flags into dense label buckets, not raw roots; CPU packs group flags by dense label (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:242-251`).

Register-pressure strategy: compute norm once per prime in the final compression pass. Reuse `a,b,norm_sq` for inner and outer eps. Implement 128-bit comparison as two 64-bit limbs or inline unsigned-multiply high/low; do not introduce generic `__int128` emulation helpers that spill. If actual regs exceed 44, split dense-label/flag packing into a tiny K4b kernel before touching K2.

Proposed launch config: `kernel_uf_flags_v2<<<chunk_tiles, 288, 2048, stream>>>`, `--maxrregcount=40`. `gridDim=chunk_tiles`. Expected actual regs 38-42. If occupancy query drops below 4 blocks/SM on 4090, remove in-kernel dense compaction and write raw root flags for a K4b compactor.

Kernel fusion decision: do not fuse K4+K5 initially. K4 is atomic-CAS and bitmap-lookup heavy; K5 is sort/encode/control-flow heavy. Fusion would keep parent, flags, face lists, and TileOp encode live together, recreating the v1 monolithic register-coupling problem.

Byte-parity gate: for each test tile, compare dense label count, label assigned to every CPU prime, and `inner_flags`/`outer_flags` bitsets before comparing complete 256 B TileOps. Overflow must emit the CPU zero-payload `OVERFLOW_BIT` behavior (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:151-154`, `tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:238-240`).

## Stage 9-GPU: Face-Strip UF, Port Ordinal Encoding, 256 B TileOp (K5)

Stage I/O: CPU v2 per-face ports are face-strip components, sorted by `(h,p_perp,label)` (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:92-148`), then `n[4]` and concatenated `face_groups` are written in face order I,O,L,R (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:254-278`). TileOp layout is fixed 256 B with offsets locked by static asserts (`tiles-maxxing/cpp-campaign-v2/include/campaign/tileop.h:46-69`).

Prior-kernel perf profile: v1 K5 has 288 threads/block, dynamic shared memory for face lists/scratch/data, parallel extraction, then serial encode (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_face_encode.cu:1-3`, `tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_face_encode.cu:452-502`). Measured Orin profile was 14.6 ms for 2000 tiles, 2.4% total, 40 regs (`docs/supportive/2026-04-10-multi-kernel-architecture.md:146-155`).

Occupancy: target 40 regs/thread gives 5 blocks/SM by register and thread caps. Shared memory target 13 KB/block from blueprint (`methodology/lemmas_v2/campaign-blueprint.md:334`); 5 blocks = 65 KB/SM, below the 100 KB 4090 budget. If measured smem grows beyond 20 KB, occupancy falls to <=5 by smem but remains acceptable; if it exceeds 25 KB, K5 becomes a real bottleneck and needs scratch packing.

Memory layout: keep per-face lists in shared memory as SoA or tightly packed structs with 4-byte alignment. Avoid v1 bit-steal fields: v2 group labels are full 8-bit bytes and flags live in fixed tail arrays (`methodology/lemmas_v2/campaign-blueprint.md:219-232`). K5 writes exactly 256 contiguous bytes per tile, so D2H copies are coalesced at 256 B/tile.

V2 changes: delete dead-end pruning and 1-D greedy clustering; blueprint calls both out as removed/replaced (`methodology/lemmas_v2/campaign-blueprint.md:317-331`). Face-strip UF must include primes with `-C <= p_perp <= C`; CPU uses inclusive checks (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:86-90`). Overflow on `sum(n)>192` or `max_label>128` emits zero payload plus `OVERFLOW_BIT` (`tiles-maxxing/cpp-campaign-v2/include/campaign/tileop.h:81-86`, `tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:268-270`).

Sort strategy: per face, observed ports are small; blueprint sizing says project-scale K=36 max `sum(n)=91` and sample peak 104 (`methodology/lemmas_v2/campaign-blueprint.md:234-236`). Use insertion sort or bitonic sort over face-component representatives in shared memory; single-thread sort is acceptable for parity bring-up, then switch only if K5 exceeds 5% of total on 4090. Canonical key is `(h,p_perp,global_wire_label)`, matching CPU comparator (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:143-147`).

Proposed launch config: `kernel_face_encode_v2<<<chunk_tiles, 288, 13*1024, stream>>>`, `--maxrregcount=40`, `cudaFuncSetAttribute(...MaxDynamicSharedMemorySize...)` as v1 already does (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu:323-327`).

Kernel fusion decision: keep separate. K5 is the only kernel whose semantics changed substantially; it needs independent profiling and independent register cap.

Byte-parity gate: complete 256 B TileOp equality against CPU snapshot. Required fields: `n[4]`, `face_groups[192]`, `inner_flags[16]`, `outer_flags[16]`, `tile_flags`, and zero reserved tail (`tiles-maxxing/cpp-campaign-v2/include/campaign/tileop.h:51-57`).

## Stage 9-Host: Sequential Compositor

Stage I/O: host compositor remains CPU sequential DSU over TileOp groups. It marks per-tile reach from TileOp flags (`tiles-maxxing/cpp-campaign-v2/src/compositor.cpp:148-165`), stitches I/O and L/R by strict ordinal equality (`tiles-maxxing/cpp-campaign-v2/src/compositor.cpp:183-228`), and ingests column by column (`tiles-maxxing/cpp-campaign-v2/src/compositor.cpp:260-318`).

Performance plan: do not port compositor to CUDA in this task. GPU produces bursts of TileOps; host ingests completed columns while next burst runs. Overflow handling is already conservative and skips stitching when either neighbor has `OVERFLOW_BIT` (`tiles-maxxing/cpp-campaign-v2/src/compositor.cpp:187-194`, `tiles-maxxing/cpp-campaign-v2/src/compositor.cpp:216-218`), avoiding the sparse overflow branch from becoming a hot exception path.

Launch/transfer config: D2H TileOp output is 256 B/tile. At 200k-tile chunks, output is 51.2 MB/chunk; H2D inputs are 24 B/tile if using blueprint `TileInput`, 4.8 MB/chunk (`methodology/lemmas_v2/campaign-blueprint.md:181-187`). On PCIe 4090 this transfer volume is not the bottleneck if double-buffered.

Byte-parity gate: full snapshot hash equality. CPU main writes snapshots after tile loop and compositor (`tiles-maxxing/cpp-campaign-v2/apps/campaign_main.cpp:425-459`); CUDA path must emit TileOps in `Grid::flat_index` order, matching `Grid::enumerate_active_tiles` (`tiles-maxxing/cpp-campaign-v2/src/grid.cpp:520-535`).

# 3. Repo Layout for `cuda-campaign-v2-sqrt-36/`

Proposed file tree:

```text
tiles-maxxing/cuda-campaign-v2-sqrt-36/
  CMakeLists.txt
  planning/
    2026-04-21-codex-performance.md
  include/
    cuda_campaign/constants.cuh
    cuda_campaign/types.cuh
    cuda_campaign/gpu_math.cuh
    cuda_campaign/pipeline.h
  src/
    main.cu
    host_driver.cpp
    kernel_sieve.cu
    kernel_mr.cu
    kernel_compact.cu
    kernel_uf_flags.cu
    kernel_face_encode.cu
    constants_upload.cu
  tests/
    compare_snapshots.py
    golden_driver.cpp
```

CMake/toolchain: build with CUDA separable compilation so each kernel gets an independent register cap, preserving the v1 split rationale (`docs/supportive/2026-04-10-multi-kernel-architecture.md:120-130`). Default `K_SQ=36`, `CMAKE_CUDA_ARCHITECTURES=89` for 4090, and local override for 80/86/87/89. Per-TU flags: K1 `--maxrregcount=40`, K2 uncapped first pass, K3 `--maxrregcount=32`, K4 `--maxrregcount=40`, K5 `--maxrregcount=40`.

CPU oracle linking: link `cpp-campaign-v2` static library or compile against its headers for `Grid`, `CampaignConstants`, snapshot writer, and comparator. CPU TileOp layout must be included directly because offsets are locked in `include/campaign/tileop.h:46-69`. The diff harness should call CPU `process_tile` for small regions and compare raw TileOps; full R=80M uses prebuilt CPU golden snapshots.

# 4. Host Integration

Use blueprint `TileInput` (24 B: `a_lo,b_lo,i,j`) instead of v1's coordinate-only structs because K4/K5 need grid indices and campaign constants (`methodology/lemmas_v2/campaign-blueprint.md:181-187`). Upload `CampaignConstants` once to `__constant__` memory (`methodology/lemmas_v2/campaign-blueprint.md:190-204`) and upload sieve/MR tables once, following v1 constant/table upload patterns (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_sieve.cu:13-19`, `tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu:283-286`).

Pipeline depth: use three CUDA streams and three pinned host buffer sets: stream A computing chunk `n`, stream B D2H-copying chunk `n-1`, host compositor ingesting chunk `n-2`. V1 stream mode processes 200k chunks but uses synchronous copies around a synchronized pipeline (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu:726-846`); v2 should replace that with `cudaMemcpyAsync` and events while preserving the same chunk size.

Device memory per 200k chunk estimate:

| Buffer | Bytes/tile | 200k chunk |
|---|---:|---:|
| `d_inputs` | 24 | 4.8 MB |
| `d_cand_list` | 24,576 | 4.92 GB |
| `d_bitmap` | 9,684 | 1.94 GB |
| `d_row_prefix` | 540 | 108 MB |
| `d_prime_pos` | 24,576 | 4.92 GB |
| `d_prime_count` | 4 | 0.8 MB |
| `d_parent` | 12,288 | 2.46 GB |
| `d_group_flags` | 32 | 6.4 MB |
| `d_output` | 256 | 51.2 MB |

Total flat allocation is about 14.4 GB plus FJ64/table overhead, safe on a 24 GB 4090 but too high for smaller cards. Preserve v1's two-phase buffer overlay idea (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu:240-252`) so K1/K2 buffers and K3/K4/K5 buffers reuse memory where lifetimes do not overlap. With overlay, peak target is under 8.5 GB for 200k chunks.

Pinned memory: allocate `TileInput[chunk]` and `TileOp[chunk]` with `cudaHostAlloc` or `cudaMallocHost`; keep compositor input in the pinned output buffer until column ingestion completes. Do not copy TileOps into `std::vector` before compositor unless the synthesizer chooses snapshot-first orchestration.

# 5. Milestone Sequence

1. **M1: v2 scaffold and K1-K3 parity.** Goal: compile sm_89 worker with K1/K2/K3 and CPU oracle diff for compacted primes. Byte gate: 100 sampled tiles exactly match CPU `sieve_tile` packed positions. Estimate: 1.5 days. Throughput delta: reach 80k+ tiles/s partial pipeline on 4090.

2. **M2: K4 UF parity without geo flags.** Goal: parent roots and dense labels match CPU local DSU for sampled tiles. Byte gate: per-prime dense label table equals CPU `dense_remap_roots` behavior (`tiles-maxxing/cpp-campaign-v2/src/tileop.cpp:172-203`). Estimate: 1 day. Throughput delta: partial pipeline 150k+ tiles/s if K4 remains <=40 regs.

3. **M3: K4 geo flags.** Goal: emit `d_group_flags` by dense label. Byte gate: CPU `inner_flags`/`outer_flags` match for synthetic and R=80M boundary tiles. Estimate: 0.75 day. Throughput delta: expected -1% to -2% e2e from K4 ALU.

4. **M4: K5 v2 face-strip UF and 256 B pack.** Goal: complete TileOps. Byte gate: 5-tile K=36 golden and 100 interior sampled tiles raw 256 B match CPU. Estimate: 2 days. Throughput delta: expected -3% to -6% vs v1 K5 due no-prune face-strip UF plus 256 B D2H.

5. **M5: async host pipeline.** Goal: three-stream H2D/compute/D2H with host compositor overlap. Byte gate: full snapshot ordering equals CPU for small full-octant and region tests. Estimate: 1 day. Throughput delta: +5% to +12% over synchronous v1 stream mode when chunks are <200k or compositor overlaps effectively.

6. **M6: 4090 tuning pass.** Goal: tune K2 cap 44 vs uncapped, K4 register cap, K5 smem packing. Byte gate: no changes without snapshot parity. Estimate: 1 day. Throughput delta: recover target 155k tiles/s.

7. **M7: full K=36 R=80M golden run.** Goal: produce byte-identical TileOp snapshot for 8,166,667 tiles. Byte gate: snapshot tile hash and manifest fields equal CPU golden. Estimate: 1 campaign run plus diff. Expected wall clock at 155k tiles/s GPU stage: 52.7 s compute, about 60-80 s including grid, transfers, compositor, snapshot I/O.

# 6. Performance Risks

1. **K4 occupancy cliff from geo flags.** If i128 emulation plus dense-label compaction pushes K4 above ~48 regs/thread, resident blocks drop and K4 becomes the bottleneck. Mitigation: split dense flag compaction into K4b before reducing K2 ILP.

2. **K5 shared-memory bank conflicts in face-strip UF.** Four face lists and small UF arrays can alias banks if stored as AoS with odd sizes. Mitigation: use 32-bit lanes for representative keys and pad per-face arrays to 32-entry boundaries.

3. **`MAX_PRIMES_GPU=6144` memory inflation.** CPU v2 raised the cap (`tiles-maxxing/cpp-campaign-v2/include/campaign/constants.h:57-62`); naive flat buffers at 200k chunks consume too much memory. Mitigation: v1 two-phase overlay (`tiles-maxxing/campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu:240-252`) is mandatory, not optional.

4. **Overflow branch divergence at R=80M K=36.** Overflow is sparse but real near axis-adjacent dense tiles; the debug doc found overflow at `(0,312509)` (`tiles-maxxing/cpp-campaign-v2/docs/2026-04-21-lemma4-debug.md:18-25`). Mitigation: K5 emits overflow from one tid after shared counters; compositor skips overflow stitching already (`tiles-maxxing/cpp-campaign-v2/src/compositor.cpp:187-194`, `tiles-maxxing/cpp-campaign-v2/src/compositor.cpp:216-218`).

5. **Host compositor fails to overlap.** If compositor or snapshot I/O exceeds GPU chunk time, e2e falls below kernel throughput. Mitigation: feed column-aligned bursts so compositor can ingest while next chunk computes; if snapshot write is bottleneck, write TileOps from pinned buffers with a separate disk thread.

6. **MR witness drift.** v1 had CPU/CUDA witness divergence called out as an avoid pattern (`methodology/lemmas_v2/v1-prior-art-study.md:23-33`). Mitigation: snapshot manifest includes witness hash from CPU main (`tiles-maxxing/cpp-campaign-v2/apps/campaign_main.cpp:467-476`); CUDA worker must report the same hash.

# 7. Open Questions for the Coordinator

1. Should the CUDA worker use FJ64 as the operational MR oracle even if CPU v2 uses a different deterministic witness set, provided snapshot parity is proven? Performance strongly prefers FJ64, but the v1 prior-art study flags silent witness drift as a process failure (`methodology/lemmas_v2/v1-prior-art-study.md:30-33`).

2. Is the full R=80M CPU golden snapshot already materialized, or should CUDA parity use CPU-on-demand for sampled chunks plus final hash only after a long CPU run?

3. Can chunk boundaries be forced to column boundaries for compositor overlap, or must the worker accept arbitrary flat-index chunks? Column boundaries simplify host ingestion and avoid buffering partial columns.

4. Is 200k tiles/chunk fixed for 24 GB 4090, or should the canonical plan set adaptive chunk sizing from free device memory after table allocation?

# 8. END-OF-PLAN THROUGHPUT ENVELOPE TABLE

Assumptions: RTX 4090 sm_89, 288 threads/block, 200k-tile chunks, three-stream async host pipeline, v2 K=36 `SIDE_EXP=269`, full octant `R_inner=80,000,000`, `R_outer=80,008,192`, exact active tiles `8,166,667`.

| Stage/kernel | Launch config | Occupancy ceiling | Expected standalone throughput | Expected e2e contribution | Bottleneck note |
|---|---|---:|---:|---:|---|
| K1 sieve | `<<<N,288,0>>>`, rreg 40 | 5 blocks/SM, 93.75% | 650k-800k tiles/s | 18%-20% time | constant-table and sparse scatter |
| K2 MR | `<<<N,288,0>>>`, uncapped/44 tune | 4 blocks/SM, 75% at 46 regs | 260k-320k tiles/s | 45%-52% time | primary bottleneck |
| K3 compact | `<<<N,288,540>>>`, rreg 32 | 5 blocks/SM, 93.75% | >5M tiles/s | <1% time | not worth fusing |
| K4 UF + geo flags | `<<<N,288,2048>>>`, rreg 40 | 5 blocks/SM if <=40 regs | 480k-600k tiles/s | 24%-30% time | secondary bottleneck, CAS + geo ALU |
| K5 face encode | `<<<N,288,13KB>>>`, rreg 40 | 5 blocks/SM, 93.75% | 1.2M-1.8M tiles/s | 4%-7% time | port-sort/control-flow tail |
| H2D/D2H | 24 B input, 256 B output/tile | PCIe/pinned | >1M tiles/s effective | hidden by streams | visible only if sync copies remain |
| Host compositor | column ingest, CPU DSU | sequential | 180k-300k tiles/s expected | overlapped target | risk if no column-aligned bursts |
| **End-to-end steady state** | 200k chunks, 3 streams | K2-limited | **145k-165k tiles/s** | **target 155k tiles/s** | match v1 bar |
| **Full K=36 R=80M GPU stage** | 8,166,667 tiles | - | - | **49.5-56.3 s** | at 145k-165k tiles/s |
| **Full campaign wall clock** | grid + GPU + compositor + snapshot | - | - | **60-80 s realistic** | grid init ~1.28 s at R=80M (`tiles-maxxing/cpp-campaign-v2/docs/2026-04-21-grid-optimization.md:42-48`) |
