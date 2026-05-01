---
title: Profiling Anomalies Investigation
date: 2026-04-23
engine: codex
type: investigation
status: complete
refs:
  - /workspace/profiles/2026-04-23-octant-trio/
  - tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp
  - tiles-maxxing/cuda-campaign-v2-sqrt-36/src/host_driver.cpp
  - tiles-maxxing/cpp-campaign-v2/src/grid.cpp
  - tiles-maxxing/cpp-campaign-v2/src/compositor.cpp
---

PASS for Issue 1. FAIL for Issue 2.

## Findings

### critical

1. **`R=90M` fails because the current CUDA app still materializes the entire full-octant tile list and full `TileOp` array on the host before chunking, so memory grows as `O(total_tiles)` and explodes long before the GPU slab logic matters.**

Evidence:

- The remote repro at `/workspace/profiles/2026-04-23-octant-trio/r90m_profile.log` shows an immediate process abort with only:

  ```text
  terminate called after throwing an instance of 'std::bad_alloc'
  what():  std::bad_alloc
  ```

- A fresh repro on the same instance with

  ```bash
  ./gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k36/campaign_main_cuda \
    --k-sq=36 --r-inner=80000000 --r-outer=90000000 \
    --region full-octant --out /tmp/r90m-repro.snapshot.bin --chunk-size=200000
  ```

  failed the same way after `11.4s`, with no stdout emitted before abort.

- The failure happens before any summary print because `campaign_main_cuda` prints only at the end, after grid enumeration, CUDA dispatch, compositor, and snapshot write: [campaign_main_cuda.cpp](../../tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp) lines 427-518.

- The first unbounded host allocation is the full active-tile vector:
  - [grid.cpp](../../tiles-maxxing/cpp-campaign-v2/src/grid.cpp) lines 515-535: `Grid::enumerate_active_tiles()` does `out.reserve(total_tiles)` and then pushes every tile.
  - [campaign_main_cuda.cpp](../../tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp) line 428 stores that whole vector in `active_tiles`.

- The next unbounded host allocation is the full output `TileOp` vector:
  - [campaign_main_cuda.cpp](../../tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp) lines 436-443.
  - [host_driver.cpp](../../tiles-maxxing/cuda-campaign-v2-sqrt-36/src/host_driver.cpp) lines 1268-1276: `dispatch_tile_batch(...)` allocates `std::vector<campaign::TileOp> output(tiles.size())`.

- The `--chunk-size` setting does **not** prevent this. It only chunks GPU dispatch after those host vectors already exist.

2. **Even if the first `bad_alloc` were removed, the current compositor architecture still cannot scale past about `33.5M` tiles, so `R=90M` and certainly `R=800M` are out of range by design.**

Evidence:

- [compositor.h](../../tiles-maxxing/cpp-campaign-v2/include/campaign/compositor.h) lines 49-53 document the precondition:

  - `grid.total_tiles <= (2^32 / 128)` aka about `33,554,432` tiles.

- [compositor.cpp](../../tiles-maxxing/cpp-campaign-v2/src/compositor.cpp) lines 239-245 enforce the cap via:

  - `total_groups = total_tiles * 128`
  - reject if `total_groups > uint32_t::max()`

- `Compositor::init` then allocates:
  - `parent[total_tiles * 128]` at lines 250-255
  - `reach[total_tiles * 128]` at line 251
  - `tileops[total_tiles]` at line 252
  - `ingested[total_tiles]` at line 253

- For `R_inner=80,000,000`, `R_outer=90,000,000`, the octant shell-area estimate is:

  - `pi/8 * (R_outer^2 - R_inner^2) / 256^2 ≈ 10.19B tiles`

  This exceeds the compositor cap by about `304x`.

### warning

1. **The “tile count nearly doubled from a 0.01% radius increase” anomaly is a benchmarking interpretation error, not evidence of a non-linear enumeration bug. The controlling quantity here is shell width `R_outer - R_inner`, and that width almost doubled.**

Evidence:

- Remote logs confirm both runs used the same moat-anchored inner radius:

  - `r80m_profile.log`: `R_inner: 80000000, R_outer: 80008192`, `active tiles: 8166667`
  - `r80015782_spanning.log`: `R_inner: 80000000, R_outer: 80015782`, `active tiles: 15444921`

- So the shell widths were:

  - baseline: `80008192 - 80000000 = 8192`
  - Tsuchimura spanning run: `80015782 - 80000000 = 15782`

- Width ratio:

  - `15782 / 8192 = 1.9265`

- Observed tile ratio:

  - `15444921 / 8166667 = 1.8912`

- For a thin annulus with fixed `R_inner`, octant area scales approximately with shell width:

  - `Area_octant = pi/8 * (R_outer^2 - R_inner^2) = pi/8 * (R_outer - R_inner) * (R_outer + R_inner)`

  Since `R_outer + R_inner` barely changes between the two runs, tile count should scale roughly with `R_outer - R_inner`.

- The simple shell-area tile estimates are close to the logged counts:

  - baseline estimate: `~7.85M` tiles vs observed `8.17M`
  - Tsuchimura estimate: `~15.13M` tiles vs observed `15.44M`

  This is within a few percent and is consistent with boundary/discretization effects, not a doubling bug.

2. **The profiling artifacts are missing command-line provenance, which makes future anomaly triage harder than it needs to be.**

Evidence:

- The profile logs under `/workspace/profiles/2026-04-23-octant-trio/` record radii and timings but not the full invoked command line, build path, git revision, or hostname/GPU ID.
- I was able to recover the effective parameters from the log bodies, but not the exact shell wrapper that produced them.

## Root Causes

### Issue 1: tile count discrepancy

No code bug found in tile enumeration for this specific discrepancy.

The apparent anomaly comes from comparing:

- a baseline shell of width `8192`
- against a Tsuchimura verification shell of width `15782`

at the same `R_inner = 80,000,000`.

That is not a `0.01% radius perturbation` in the quantity that controls work. It is an `~92.6%` increase in shell width, so an `~89%` increase in tile count is physically plausible and matches the shell-area model.

The “8.17M looks like `8192^2 / 8`” observation is a coincidence. The code does not derive tile count from shell width squared. Tile count comes from `Grid::build(...)` producing `j_low/j_high/tower_offset`, then `Grid::enumerate_active_tiles()` enumerating exactly `total_tiles` active tile coordinates in canonical order: [grid.cpp](../../tiles-maxxing/cpp-campaign-v2/src/grid.cpp) lines 565 onward and 515-535.

### Issue 2: `std::bad_alloc` at `R=90M`

The immediate root cause is full-octant host materialization:

1. build the full grid
2. allocate `active_tiles` for every tile in the octant
3. allocate `tileops` for every tile in the octant
4. later allocate compositor state proportional to `128 * total_tiles`

At `R=90M`, the tile count is on the order of `10.19B`, so the host-memory requirement is catastrophic before the GPU dispatch loop starts doing useful work.

The likely first-failing allocation is `active_tiles.reserve(total_tiles)` inside `Grid::enumerate_active_tiles()`, because that happens before the `TileOp` output vector and before compositor init. The observed abort timing and empty stdout are consistent with that path.

## Memory Scaling Model

### Host state that scales with full-octant tile count

Per tile, before compositor:

- `TileCoord`: 24 B
- `TileOp`: 256 B
- subtotal: `280 B / tile`

Per tile, compositor:

- `parent`: `128 * 4 = 512 B`
- `reach`: `128 * 1 = 128 B`
- stored `tileops`: `256 B`
- `ingested`: `1 B`
- subtotal: `897 B / tile`

Combined host-resident subtotal:

- `1177 B / tile`, ignoring vector overhead and allocator slack

Examples:

- `8,166,667` tiles: about `9.61 GB`
- `15,444,921` tiles: about `18.18 GB`
- `10.19B` tiles (`R=90M` estimate): about `11.99 TB`

So the current driver is only viable because the validated `R≈80M` moat runs are still barely inside workstation-scale host memory. `R=90M` is already far beyond that.

### GPU state

The GPU driver itself is slabbed correctly. The GPU workspace scales with `device_slab_tiles`, not with total campaign tiles:

- [host_driver.cpp](../../tiles-maxxing/cuda-campaign-v2-sqrt-36/src/host_driver.cpp) lines 223-283 compute slab budgeting from `cudaMemGetInfo`.
- `phase1_bytes_for_tiles()` and `phase2_bytes_for_tiles()` at lines 481-526 show per-tile slab costs.

Current per-tile GPU slab costs are approximately:

- phase 1: `75,256 B / tile`
- phase 2: `477,731 B / tile`

This is large but bounded by slab size. It is **not** the cause of the `R=90M` full-octant `bad_alloc`.

## Proposed Fixes

### Fix for Issue 1

No enumeration fix is indicated.

Operational fixes:

1. When comparing profile runs, normalize by shell width `delta = R_outer - R_inner`, not by absolute `R_outer`.
2. Add `delta_r`, `grid.total_tiles`, and a simple shell-area estimate to the profile output so the logs explain their own scaling.
3. Record the full command line, binary path, git commit, and host/GPU identity in every profile log.

### Fix for Issue 2

This needs an architectural change, not a tuning pass.

1. **Stream tiles instead of materializing `active_tiles`.**
   - Replace `grid.enumerate_active_tiles()` as the primary full-octant path.
   - Iterate columns or bounded column bands directly from the tower table.

2. **Stream `TileOp` production directly into downstream consumers.**
   - `dispatch_tile_batch(...)` should accept a callback / sink, not return `std::vector<TileOp>`.
   - Chunking must become end-to-end, not “GPU-only after full host preload”.

3. **Redesign the compositor to keep only live frontier state.**
   - The current `tile_idx * 128 + group` global-ID model is fundamentally capped at `~33.5M` tiles.
   - A scalable compositor must retire groups once they can no longer connect to future columns, instead of storing all groups for all tiles.

4. **Stream snapshot emission.**
   - Do not require a full `tileops` vector in memory just to write the snapshot.
   - Emit the snapshot payload incrementally in canonical tile order, or decouple snapshot generation from verdict runs.

5. **Add an early preflight guard.**
   - Before allocating anything large, estimate memory from `grid.total_tiles`.
   - Fail fast with a diagnostic if the run exceeds host-memory budget or compositor-cap budget.

## What I Checked

- Remote logs in `/workspace/profiles/2026-04-23-octant-trio/`:
  - `r80m_profile.log`
  - `r80015782_spanning.log`
  - `r80015790_moat.log`
  - `r90m_profile.log`
- Remote repro of the `R=90M` command on `ssh4.vast.ai`.
- Local code paths:
  - [campaign_main_cuda.cpp](../../tiles-maxxing/cuda-campaign-v2-sqrt-36/apps/campaign_main_cuda.cpp)
  - [host_driver.cpp](../../tiles-maxxing/cuda-campaign-v2-sqrt-36/src/host_driver.cpp)
  - [grid.cpp](../../tiles-maxxing/cpp-campaign-v2/src/grid.cpp)
  - [compositor.cpp](../../tiles-maxxing/cpp-campaign-v2/src/compositor.cpp)
- Shell-area sanity calculations for `R_outer = 80,008,192`, `80,015,782`, `80,015,790`, and `90,000,000`.

## Assumptions

- The remote binary at `/workspace/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k36/campaign_main_cuda` matches the checked-in source tree closely enough for line-level reasoning to apply.
- The `R=90M` target is intended as a full-octant moat-style run with `R_inner=80,000,000`, not as a narrow-shell profiling surrogate.
