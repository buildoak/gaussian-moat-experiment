---
date: 2026-03-25
engine: codex
status: complete
type: architecture
campaign: k40-tile-ub
---
# High-Performance Tile UB Pipeline Architecture
Geometry note: the current CUDA code in `src/tile_main.cu` uses inclusive bounds, so `nominal_extent = side + 1` and `side_exp = side + 1 + 2c`. For `k_sq=40`, `c=7`, `--side=2000` means `side_exp=2015` and `total_points=4,060,225`. Some campaign notes use `2014^2`; the pipeline should standardize this at the coordinator boundary, but the architecture below is unchanged either way.
## 1. System Diagram
```text
Control flow
------------
Rust Tile Coordinator
    |  build scan-order batches: [(tile_id, a_lo, b_lo, side); N]
    v
CUDA Batch Engine
    |  launch sieve+MR kernel -> launch GPU UF/classify kernel
    |  recycle device buffers, enqueue D2H copy
    v
Rust Compositor
    |  consume tiles in row order, union seam matches, checkpoint progress
    v
UB verdict / super-tile operator / restart state

Data flow
---------
TileJob[N] ----------------------> device job table
device job table ---------------> CUDA batch kernels
GPU bitmaps + parent/rank ------> GPU face-port compaction
binary face-port stream --------> Rust compositor
row buffers + global UF --------> final band result / super-tile summaries
```
Primary split: CUDA owns per-tile primality + connectivity; Rust owns inter-tile composition, streaming state, checkpointing, and campaign orchestration.

## 2. CUDA Kernel Redesign

Current bottleneck: `classify_components()` in [`src/tile_main.cu`](/Users/otonashi/thinking/building/gaussian-moat-cuda/src/tile_main.cu) is CPU-side and dominates wall time at `side=2000`.

### Option A: GPU union-find

- Design: keep the existing row-sieve + MR bitmap kernel, then run a second GPU connected-components phase over the expanded tile bitmap.
- Working set per tile at current geometry:
  - Bitmap: `4,060,225 / 8 = 507,529` bytes
  - Parent: `4,060,225 * 4 = 16.24 MB`
  - Rank or hook label: `4.06 MB` if kept as `u8`
  - Operational total: about `20.8 MB/tile` plus compacted output
- Algorithm candidates:
  - ECL-CC style label propagation / hooking over the prime bitmap
  - HCBF / afforest-like union-hook compression specialized to a fixed offset stencil
- Fit: global memory is sufficient on Jetson, 3090, 4090, and A100.
- Expected gain: replace `200-800 ms` CPU UF with roughly `5-20 ms` GPU UF, making the full per-tile pipeline mostly GPU-bound again.
- Challenges:
  - Atomic contention on hot roots
  - Deterministic component labeling for binary output
  - Efficient face classification and compaction on device

### Option B: Face-port extraction only

- Design: CUDA emits only face-zone primes plus enough local connectivity metadata to let Rust reconstruct tile-internal components.
- Benefit: less GPU work and smaller device-side state.
- Problem: the compositor now has to solve intra-tile connectivity, not just seam composition.
- Correctness risk: easy to under-encode a component that touches multiple faces through collar-only paths.
- Net: smaller CUDA rewrite, larger system complexity, harder proof surface.

### Recommendation

Primary path: Option A. It preserves the current operator contract: one tile in, exact face-component operator out. Rust stays a seam compositor instead of becoming a second local graph engine.

### Batch mode

- Launch shape: `grid = N * side_exp` blocks, `blockDim = 256`, one block per row per tile.
- Tile `t` owns disjoint slices of:
  - primality bitmap
  - UF parent/rank arrays
  - compacted face-port output
- Shared memory per block stays the current row sieve bitmap from [`src/row_sieve.cuh`](/Users/otonashi/thinking/building/gaussian-moat-cuda/src/row_sieve.cuh); no per-tile shared-memory explosion.
- Buffers are allocated once at startup and reused across all batches.

Batch-size envelope:

| Platform | VRAM basis | Parent-only ceiling | Realistic working ceiling |
|----------|------------|---------------------|---------------------------|
| Jetson Orin | 7.4 GB shared | `~460` tiles at `16 MB/tile` | `~100-200` tiles after OS + rank/bitmap headroom |
| RTX 3090 | 24 GB | `~1500` | `~900-1100` |
| RTX 4090 | 24 GB | `~1500` | `~900-1100` |
| A100 80GB | 80 GB | `~5000` | `~3700-4000` |

Use the parent-only number as the hard upper bound from the campaign notes; size production batches from the full working set.

### Output format

Switch from JSON to packed binary. Per tile:

- `TileHeader`
- `face_inner[num_face_ports[0]]`
- `face_outer[num_face_ports[1]]`
- `face_left[num_face_ports[2]]`
- `face_right[num_face_ports[3]]`

Estimated output:

- `~800` ports/face at `R~1e9`
- `4 * 800 * 12 = 38.4 KB/tile` payload
- Header and padding are negligible relative to port payload

### Memory recycling

- Pre-allocate device buffers for bitmap, parent, rank/label, port counts, compacted ports, and D2H staging.
- Use `cudaMemsetAsync` or kernel-side initialization; no steady-state `cudaMalloc`/`cudaFree`.
- Double-buffer batch `n` and batch `n+1` so Rust can compose while the GPU fills the next output region.

## 3. Rust Compositor Architecture

Streaming model:

1. Tile coordinator enumerates tiles in scan order: left-to-right within a row, bottom-to-top across rows.
2. CUDA returns face-port operators in the same order.
3. Compositor assigns each tile-local component a global UF ID.
4. Seam matches:
   - horizontal: current tile `L` ports vs cached right-neighbor boundary
   - vertical: current tile `I` ports vs previous-row `O` cache
5. After seam unions, discard tile interior state and retain only the faces needed by future tiles.

Resident state:

| Item | Estimate |
|------|----------|
| Global UF | `5.5M` entries for `~10` boundary components/tile over `550K` tiles |
| UF bytes | `5.5M * 8 = 44 MB` for compressed `u32 parent + u32 rank/size` |
| Current-row right-face cache | `20K * 800 * 12 = 192 MB` |
| Previous-row outer-face cache | `192 MB` |
| Tile/job metadata + maps | `<100 MB` |
| Total | `~500 MB` |

This fits comfortably on all host targets.

Composition contract:

- Reuse the existing distance-based seam rule already implemented in [`tile-probe/crates/moat-kernel/src/compose.rs`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tile-probe/crates/moat-kernel/src/compose.rs).
- Preserve the full `FacePort { a, b, component }` interface from [`tile-probe/crates/moat-kernel/src/tile.rs`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tile-probe/crates/moat-kernel/src/tile.rs).
- Enforce `W >= c` and gapless stride `s_b = W`, `s_a = H` for UB soundness.

Pipeline parallelism:

- Within a row, tile `j` depends only on tile `j-1` and row `i-1`.
- Practical structure:
  - GPU batch `n` computes tiles `T[k..k+N)`
  - Rust thread composes batch `n-1`
  - D2H copies overlap both via streams + pinned host buffers
- This is enough; a persistent kernel is optional.

## 4. Interface Specification

Binary stream is little-endian and append-only. The stream order is the coordinator scan order.

`TileHeader` (`28 bytes`):

```c
struct TileHeader {
    uint32_t tile_id;
    uint32_t num_face_ports[4];   // I, O, L, R
    uint32_t num_components;      // local component count after GPU UF
    uint32_t flags_reserved;      // versioning, clipping flags, padding
};
```

`FacePort` (`12 bytes`):

```c
struct FacePort {
    int32_t a;
    int32_t b;
    uint32_t component_id;
};
```

Transport options:

- Same process: shared pinned host buffer via FFI. Fastest, lowest copy count, highest integration complexity.
- Separate processes: binary pipe. Recommended first implementation. `38 KB/tile` is tiny; even `1000 tiles/s` is only `~38 MB/s`.
- File-backed staging: mmap ring buffer if backpressure or restart semantics need explicit persistence.

Recommendation: start with a pipe-compatible record format even if the first implementation is in-process FFI. It keeps the interface observable and testable.

## 5. Performance Model

Per-tile model for `side=2000`, `k_sq=40`, with GPU UF replacing CPU UF:

| Phase | Jetson Orin | RTX 3090 | RTX 4090 | A100 |
|-------|-------------|----------|----------|------|
| GPU sieve + MR | 40 ms | 3 ms | 2 ms | 4 ms |
| GPU UF | 30 ms | 3 ms | 2 ms | 3 ms |
| D2H transfer | 1 ms | <1 ms | <1 ms | <1 ms |
| Rust compose | 1 ms | 1 ms | 1 ms | 1 ms |
| Total / tile | ~72 ms | ~7 ms | ~5 ms | ~8 ms |

Throughput:

| Platform | Tiles/sec | 550K tiles | 163B tiles |
|----------|-----------|------------|------------|
| Jetson Orin | 14 | 11 hours | infeasible |
| RTX 3090 | 143 | 64 min | infeasible |
| RTX 4090 | 200 | 46 min | 25.8 years |
| A100 | 125 | 73 min | infeasible |
| 8x A100 | 1000 | 9 min | 5.2 years |

Implication:

- Flat `163B`-tile evaluation is not a real campaign plan.
- Super-tile aggregation is mandatory.
- If `K=100`, the evaluation count drops from `163B` base tiles to `16.3M` super-tile operators, which moves the problem from impossible to schedulable.

## 6. Optimization Priorities

1. GPU union-find: high impact, medium effort. Removes the dominant `70-80%` bottleneck.
2. Batch processing: high impact, low effort. Eliminates process-launch overhead and amortizes constant-memory setup.
3. Binary output format: medium impact, low effort. Cuts parsing overhead and keeps the pipeline streamable.
4. Persistent kernel: medium impact, high effort. Useful only after batches are already large and steady-state.
5. Shared-memory UF for small tiles: low impact, high effort. Only helps small calibration tiles.
6. Multi-GPU partitioning: high impact, high effort. Necessary only after a single-GPU pipeline is stable and super-tiles exist.

## 7. Implementation Phases

| Phase | Scope | Acceptance test |
|------|-------|-----------------|
| 1. MVP | Binary output from `tile_cuda`; Rust binary reader; streaming compositor | Compose a `10x10` tile grid and match current Rust reference output |
| 2. GPU UF | Device-side UF, face classification, GPU face-port emission | `100` random tiles match CPU reference exactly |
| 3. Batch mode | Multi-tile launches, pre-allocated buffer pool, overlapped D2H | `100`-tile batch matches single-tile reference tile-for-tile |
| 4. Integration | Rust coordinator -> CUDA batch -> compositor -> UB verdict; checkpoint/restart | Small band `[1000, 2000]` reproduces reference UB result |
| 5. Scale | Super-tile aggregation, multi-GPU, campaign telemetry | Sustained long-run execution with restart and progress accounting |

Expected cadence:

- Phase 1: 2 sessions
- Phase 2: 2 sessions
- Phase 3: 1 session
- Phase 4: 1 session
- Phase 5: ongoing

## 8. Risks and Mitigations

1. GPU UF correctness. Concurrent UF is easy to get subtly wrong. Mitigation: exhaustive equivalence tests on small tiles, then randomized differential tests on large tiles against the current CPU reference.
2. Jetson memory pressure. Unified memory means the practical batch size is much smaller than the theoretical ceiling. Mitigation: cap batch size conservatively and support `side=1000` fallback profiles.
3. Face-port explosion. At `R~1e9`, Gaussian prime density is about `1 / ln R ~= 1 / 20.7`; for a face zone of `2000 * 7 = 14,000` points, expected ports are only `~676`, so the stream remains small. Mitigation: keep output buffers sized for `~2x` this expectation.
4. Global compositor growth. A flat `163B`-tile run would blow up the global UF. Mitigation: hierarchical super-tiles; with `K=100`, boundary state is on the order of `~1.6M` super-tile components, not billions.
5. Soundness regression. Any shortcut in face extraction can break the overlap proof. Mitigation: property tests against the proven Rust composition semantics, plus assertions for `W >= c`, region clipping, and gapless stride.

Bottom line: the fastest credible path is GPU UF + batched CUDA + binary face-port streaming into a scan-order Rust compositor. That gets single-tile cost into the `5-8 ms` class on desktop GPUs, but campaign feasibility still depends on super-tile aggregation rather than raw tile throughput.
