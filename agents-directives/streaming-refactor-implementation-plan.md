# CUDA Campaign Streaming Refactor Implementation Plan

Date: 2026-05-02

This plan covers the bundled refactor for `cuda-campaign-v2-sqrt-36`:
optional snapshots, default verdict-only mode, column-complete streaming,
sound `SPANNING` early exit, timing/profile output, and stronger verification
on the Vast.ai 4090 gate.

## Final Verdict

Proceed with the bundled refactor, but gate it internally. The unsafe parts
must be proven before the CUDA campaign main is trusted:

- no shared `DeviceWorkspace` across overlapped CUDA streams unless lifetime is
  fixed per in-flight slot
- no tile-level early exit
- no `MOAT` early exit
- no sparse/explicit streaming unless canonicalized or rejected
- no golden-as-truth correctness story

## Implementation Sequence

### 1. Snapshot/default CLI foundation

Files:

- `tiles-maxxing/cpp-campaign-v2/include/campaign/snapshot.h`
- `tiles-maxxing/cpp-campaign-v2/src/snapshot.cpp`
- `tiles-maxxing/cpp-campaign-v2/apps/campaign_main.cpp`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp`

Changes:

- Add `SnapshotWriter`.
- Add one canonical grid-hash helper.
- Add frozen timestamp support for byte-stable tests.
- Make verdict-only the default mode.
- Add `--snapshot-out PATH`.
- Keep `--out PATH` as a real compatibility alias for `--snapshot-out PATH`.
- Error if `--out` and `--snapshot-out` are both supplied with different paths.

### 2. Streaming compositor, CPU-only first

Files:

- `tiles-maxxing/cpp-campaign-v2/include/campaign/streaming_compositor.h`
- `tiles-maxxing/cpp-campaign-v2/src/streaming_compositor.cpp`
- `tiles-maxxing/cpp-campaign-v2/CMakeLists.txt`

Changes:

- Implement `StreamingCompositor` beside the existing `Compositor`.
- Keep the existing `Compositor` as the small-grid oracle.
- Do not mutate the full-buffer compositor until streaming equivalence is
  tested.

### 3. Streaming proof harness before CUDA integration

Files:

- `tiles-maxxing/cpp-campaign-v2/tests/test_streaming_compositor.cpp`
- optional test-only hooks in `tiles-maxxing/cpp-campaign-v2/include/campaign/compositor.h`

Changes:

- Compare streaming verdict against full compositor on small grids.
- Compare projected frontier after every column.
- Test empty-column gap behavior.

### 4. CUDA dispatcher safety refactor

Files:

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/include/cuda_campaign/host_driver.h`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/src/host_driver.cpp`

Changes:

- Introduce a reusable persistent dispatcher.
- Upload constants and FJ table once per dispatcher.
- Fix `DeviceWorkspace` lifetime by either:
  - giving each in-flight slot its own workspace, or
  - explicitly serializing workspace reuse.
- Split cheap overflow counters from expensive first-overflow diagnostics.
- Keep the existing `dispatch_tile_batch(...)` API as a compatibility wrapper.

### 5. CUDA main streaming integration

File:

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp`

Changes:

- Build app-level batches from complete grid columns.
- Treat `--chunk-size N` as a max tile target, but never split a column at the
  app/compositor boundary.
- Dispatch CUDA TileOps for the batch.
- Append snapshot data if snapshot mode is enabled.
- Ingest complete columns immediately.
- Early-exit only after a complete column when:
  - `has_spanning()` is true
  - snapshot mode is disabled
  - `--no-early-exit` is absent

### 6. Diagnostics, diff, and gates

Files:

- `tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/cuda_vs_cpu_diff.cpp`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/run_snapshot_sha_gate.sh`
- `tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/run_tsuchimura_gate.sh`

Changes:

- Add sampled CPU/CUDA diff probes.
- Add targeted diff support with `--start-index N --limit N`; add `--tile i,j`
  if practical.
- Ensure Tsuchimura gates require expected verdict plus zero overflow counters.
- Keep goldens as smoke/regression only.

## Worker Breakdown

- Worker A: snapshot/default CLI foundation. CPU-only. Can run immediately.
- Worker B: `StreamingCompositor` plus projected-frontier tests. CPU-only. Can
  run in parallel with A.
- Worker C: CUDA dispatcher safety/profile refactor. Can run in parallel with A
  and B, but final confidence needs GPU.
- Worker D: CUDA main streaming integration. Depends on A, B, and C.
- Worker E: diff/gate scripts and Vast.ai procedure. Mostly after A and C;
  final gate depends on D.
- Worker F: adversarial audit after D. Reviews dataflow, early-exit boundaries,
  overflow handling, and sparse-region behavior.

## CLI/API Contract

Campaign binaries:

- required: `--k-sq`, `--r-inner`, `--r-outer`, `--region`
- optional snapshot mode: `--snapshot-out PATH`
- compatibility alias: `--out PATH`
- error: both snapshot flags supplied with different paths
- `--no-early-exit`
- `--chunk-size N`
- `--timing`
- `--profile PATH`
- no `--vanilla`

Snapshot API:

- `grid_params_hash(const Grid&)`
- `SnapshotWriter(path, grid, constants, options)`
- `append_column(i, span<const TileOp>)`
- `close()`
- `write_snapshot(...)` delegates to `SnapshotWriter`
- timestamp can be frozen through options or environment for byte-stable tests

Streaming API:

- `StreamingCompositor::init(const Grid&)`
- `ingest_column(i, span<const TileOp>)`
- `has_spanning()`
- `finalize()`
- test-only `project_frontier()`

`project_frontier()` should return canonical equality and reach state keyed by
`(j, port_ordinal)`.

## Correctness Invariants

- `SPANNING` early-exit only after a whole column/tower has been produced,
  downloaded, optionally snapshotted, and ingested.
- `MOAT` requires all active columns.
- Snapshot mode disables early exit and writes every TileOp.
- Current and prior frontier state is keyed by `(j, port_ordinal)`.
- Frontier roots are canonically remapped after each column.
- Equality among live ports and reach bits must match the full compositor on
  small-grid oracle tests.
- Empty column gaps clear frontier.
- Overflow never silently proves Tsuchimura. Tsuchimura gates require expected
  verdict plus zero overflow counters.
- CUDA output order must match input order.
- App-level column order must match `Grid::flat_index`.
- Explicit/sparse regions are rejected in streaming mode unless canonicalized
  and tested.

## Test And Gate Plan

CPU CTest:

- existing compositor tests still pass
- streaming verdict parity against full compositor
- projected frontier parity after every column
- empty column gap behavior
- overflow conservative `SPANNING`
- vector snapshot writer versus streaming writer byte identity
- frozen manifest timestamp or schema-field manifest comparison
- CLI no-snapshot default and `--out` alias conflict tests

CUDA CTest:

- existing K1-K5 parity tests
- dispatcher multi-chunk/ring-slot parity
- `campaign_main_cuda` no-snapshot smoke
- `campaign_main_cuda --snapshot-out` smoke
- snapshot SHA smoke parity CPU versus CUDA

Required probes:

```bash
./build-k36/cuda_vs_cpu_diff --r-inner 80000000 --r-outer 80015782 --m4 --k5 --verbose --limit 16
./build-k36/cuda_vs_cpu_diff --r-inner 80000000 --r-outer 80015782 --m4 --k5 --verbose --sample 1024
```

## Vast.ai 4090 Procedure

Build and test:

```bash
cd /workspace/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36
cmake -S . -B build-k36 -DK_SQ=36 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-k36 -j"$(nproc)"
ctest --test-dir build-k36 --output-on-failure

./build-k36/cuda_vs_cpu_diff --r-inner 80000000 --r-outer 80015782 --m4 --k5 --verbose --limit 16
./build-k36/cuda_vs_cpu_diff --r-inner 80000000 --r-outer 80015782 --m4 --k5 --verbose --sample 1024
scripts/run_snapshot_sha_gate.sh --smoke --chunk-size 64
```

Tsuchimura external truth:

```bash
./build-k36/campaign_main_cuda --k-sq=36 --r-inner=80000000 --r-outer=80015782 --region full-octant --chunk-size=200000 --timing --profile /workspace/r80015782.early.json

./build-k36/campaign_main_cuda --k-sq=36 --r-inner=80000000 --r-outer=80015782 --region full-octant --chunk-size=200000 --no-early-exit --timing --profile /workspace/r80015782.full.json

./build-k36/campaign_main_cuda --k-sq=36 --r-inner=80000000 --r-outer=80015790 --region full-octant --chunk-size=200000 --timing --profile /workspace/r80015790.json
```

Acceptance:

- `R_outer=80,015,782` returns `SPANNING`
- `R_outer=80,015,790` returns `MOAT`
- all overflow counters are zero in full runs
- only after that, run the R=1.1B target verdict-only, no snapshot, with
  profile output

## Deferred Non-Goals

- no tile-level early exit
- no `MOAT` early exit
- no large-R snapshot by default
- no sparse/explicit streaming support unless canonicalization is implemented
  and tested
- no performance claims until profile JSON and 4090 logs exist
- no reliance on goldens for correctness authority

## Decision Points

Recommended defaults:

1. Run R=1.1B only after Tsuchimura passes and the adversarial audit signs off.
2. Start with explicit serialized workspace reuse if per-slot CUDA workspaces
   threaten correctness or implementation time; upgrade to per-slot overlap only
   after the first correct 4090 profile shows dispatch is the bottleneck.
3. Preserve logs and profile JSON from Vast.ai. Preserve large snapshots only
   when a specific parity investigation needs them.
