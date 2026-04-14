# sqrt(40) Campaign Plan -- R=600M Verification

Date: 2026-04-13

## Objective

Verify two things after the O(N^2) spanning-check fix in `compositor.cpp`:

1. **Early termination works.** At R=600M (well inside the Tsuchimura connected bound of ~830M for K_SQ=40), the compositor should detect SPANNING and exit early -- likely within the first few percent of towers.

2. **Full sweep shows constant per-tower time.** Processing ALL towers (no early exit) proves the O(N^2) fix holds at scale. Per-tower compositor time should be flat from tower 1 to tower 1,657,306. This is the performance regression test.

K_SQ=40 is the well-optimized path: overflow rate near zero (0 overflows in the 75M-tile octant dump at R=850M), GPU caps fully adequate, no data loss concerns. The only variable under test is the compositor fix.

---

## Grid Geometry

Computed via the `compute_grid()` algorithm in `tiles-compositor/src/grid.cpp`:

| Parameter | Value |
|---|---|
| R | 600,000,000 |
| K_SQ | 40 |
| TILE_SIDE (S) | 256 |
| num_towers | ~1,657,306 |
| total_tiles | ~59,856,476 |
| avg tiles/tower | ~36.1 |
| tiles_per_tower range | 32 (at y-axis) to 46 (at 45 degrees) |

**Memory budget:**

| Component | Size |
|---|---|
| TileOp data (total_tiles * 128B) | ~7.14 GiB |
| Union-find parent + root_reach (~8 groups/tile avg * 5B) | ~2.2 GiB |
| Working buffers (burst I/O, tower buffers) | ~0.5 GiB |
| **Total** | **~10 GiB** |

Fits comfortably on RTX 4090 (24 GB VRAM) with room to spare. Peak RSS will be dominated by the compositor's `parent_` and `root_reach_` vectors.

**Tile density note:** At R=600M, prime density is ~1/ln(R^2) ~ 1/40.5, giving ~1,630 primes per tile. At K_SQ=40 with COLLAR=7, this yields median ~10 groups/tile and max ~24 groups/tile (from the 75M-tile census at R=850M). Well within the 127-group TileOp cap.

---

## Run 1: Early Termination (SPANNING detection)

### Command

```bash
cd /root/tiles-compositor/build-k40
./campaign 600000000 --k-sq 40 --cuda --burst-size 4096 --progress-interval 1000
```

### Expected Behavior

- Grid prints: ~1,657,306 towers, ~59.9M total tiles.
- Tower j=0 processed by C++ path (under 1 second).
- CUDA bursts begin. Each burst processes 4,096 towers.
- **SPANNING detected early** -- likely within the first 1-5 bursts (first 4K-20K towers, ~1-3% of total). At R=600M, prime connectivity is dense; the connected component spanning inner-to-outer boundary should form quickly.
- Campaign exits with verdict `SPANNING`, exit code 0.
- `towers_processed` in the JSON output will be much less than `num_towers`.

### Pass Criteria

1. Exit code 0 (SPANNING verdict).
2. `towers_processed` < 100,000 (less than ~6% of total towers). If spanning takes longer, it still passes but warrants investigation.
3. `overflow_count` = 0. K_SQ=40 at R=600M should produce zero overflows.
4. Wall time under 2 minutes (dominated by the towers processed before spanning is found).
5. No crashes, no assertion failures.

### What a Failure Looks Like

- Exit code 1 (MOAT verdict): the spanning detection or boundary collection has a bug. R=600M is well inside the connected region -- MOAT here would be a false negative.
- `towers_processed` = `num_towers`: early exit did not trigger. Either `check_spanning_incremental()` never returns true, or the inner/outer boundary marking is incomplete.
- Crash in compositor: likely a malformed TileOp reaching `assert_not_overflow()` or a union-find out-of-bounds.

---

## Run 2: Full Sweep (all towers, no early exit)

### The Problem: No `--no-early-exit` Flag

The campaign binary has no CLI flag to disable early termination. Early exit is hardcoded in the CUDA burst loop (`campaign.cpp` line 456: `!spanning_found` in the for-loop condition) and after each tower ingestion (line 609-614: `check_spanning_incremental()` -> `break`).

### Workaround Options

**Option A: Patch campaign.cpp (2 lines).** Comment out or guard the spanning break logic:

In the CUDA path, change the tower-ingestion spanning check (around line 609-614):
```cpp
// TEMPORARY: disable early exit for full-sweep mode
// if (compositor.check_spanning_incremental()) {
//     compositor.collect_outer_boundary(j);
//     spanning_found = true;
//     spanning_tower = j;
//     break;
// }
```

And change the burst-loop condition (line 456):
```cpp
for (int32_t burst_start = 0; burst_start < remaining_towers /* && !spanning_found */;
```

Then rebuild `campaign`. This is the cleanest approach. The spanning check itself (`check_spanning_incremental()`) is now O(1) so calling it is harmless -- only the `break` needs suppression.

**Option B: Use `--mode dump_with_stats`** -- this mode is listed in the CLI help but the implementation currently falls back to verdict_only with a warning. Not useful.

**Option C: Set R above the Tsuchimura bound.** At R > 830M, the region may have a moat, so spanning might not be found and all towers would be processed. But this changes the test conditions (more towers, different geometry, and the verdict is unknown).

**Recommendation: Option A.** Minimal, reversible, and keeps R=600M as the test parameter. Build a second binary `campaign-fullsweep` with the patch, run it alongside the unmodified `campaign`.

### Command (after patching)

```bash
cd /root/tiles-compositor/build-k40
./campaign-fullsweep 600000000 --k-sq 40 --cuda --burst-size 4096 --progress-interval 5000
```

Use `--progress-interval 5000` for this run (every 5000 towers) to reduce progress output volume while still giving enough data points to track per-tower timing.

### Expected Behavior and Wall Time

**GPU time:** ~59.9M tiles / 155K tiles/sec = ~386 seconds (~6.4 minutes). This is a hard floor -- every tile must be computed.

**Compositor time (post-fix):** With the O(N^2) fix, `check_spanning_incremental()` is O(1) -- just returns `spanning_detected_`. The per-tower cost is dominated by `ingest_tower()` which runs: compute_offsets, match_io_within_tower, pre_flatten, match_lr_with_previous, collect_inner_boundary, collect_outer_boundary_ingest. These are all O(tiles_per_tower * groups_per_tile) per tower.

From the K_SQ=36 postmortem, the baseline per-tower cost at early towers (before O(N^2) kicked in) was 1.4 ms/tower, of which ~0.2 ms was GPU amortized, leaving ~1.2 ms compositor. K_SQ=40 has fewer groups per tile than K_SQ=36, so expect 0.5-1.0 ms/tower compositor time.

| Component | Estimate |
|---|---|
| GPU total | ~386s (6.4 min) |
| Compositor total (0.5 ms/tower) | ~829s (13.8 min) |
| Compositor total (1.0 ms/tower) | ~1,657s (27.6 min) |
| **Total wall (sequential)** | **20 - 34 minutes** |

Note: the current campaign binary runs GPU bursts and compositor ingestion sequentially (no overlap). The GPU burst finishes, results are read, then the compositor processes each tower. So total wall time is GPU + compositor, not max(GPU, compositor).

### Pass Criteria

1. `towers_processed` = `num_towers` (all 1,657,306 towers processed).
2. `overflow_count` = 0.
3. **Constant per-tower time.** This is the key metric. Extract per-5000-tower batch times from the progress output. Compute ms/tower for each batch. The value should be **flat** (within +/-20%) from first batch to last batch. If per-tower time doubles or triples over the course of the run, the O(N^2) fix is incomplete.
4. Wall time under 40 minutes. If it exceeds 60 minutes, the compositor is degrading.
5. Peak RSS under 15 GB (reported in the JSON output).

### Monitoring: How to Verify the O(N^2) Fix

The progress output prints `tower N/M (X%) elapsed Ys` every 5000 towers. From consecutive lines, compute:

```
batch_time = elapsed[i] - elapsed[i-1]
ms_per_tower = batch_time * 1000 / 5000
```

Plot or tabulate ms_per_tower across the full run. Expected pattern:

- **PASS (O(1) fix works):** ms_per_tower stays in range [0.5, 2.0] throughout.
- **FAIL (still O(N^2)):** ms_per_tower grows linearly, e.g., 1.0 at 0-5K, 2.0 at 50K, 10.0 at 250K, etc.

For quick diagnosis during the run: compare the first 3 progress lines with lines from 50% and 75% completion. If the ratio exceeds 2x, something is wrong.

### Per-Tower Timing Emission

The campaign binary does not emit per-tower timing. The progress output gives cumulative elapsed time at intervals. This is sufficient to compute batch-level ms/tower.

If finer granularity is needed, add a `Clock::now()` call around `compositor.ingest_tower()` and print per-tower compositor time to stderr. This is a 3-line change in campaign.cpp's tower ingestion loop:

```cpp
const auto t0 = Clock::now();
compositor.ingest_tower(j, ...);
const auto dt = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
if (j % 1000 == 0) std::fprintf(stderr, "tower %d compositor %.2f ms\n", j, dt);
```

This would confirm per-tower O(1) behavior directly. Optional but valuable for the first run after the fix.

---

## Deploy Notes

### Critical: CMakeLists.txt Path Fix Required

`tiles-compositor/CMakeLists.txt` line 65 contains a stale path:

```cmake
set(TILE_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../tiles-maxxing/tile-cpp")
```

Now that tiles-compositor lives INSIDE tiles-maxxing, this resolves to `tiles-maxxing/tiles-maxxing/tile-cpp` which does not exist. The correct path:

```cmake
set(TILE_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../tile-cpp")
```

This affects both local and remote builds. The `campaign` target links against `${TILE_CPP_ROOT}/build-k${K_SQ}/libtile.a` -- without this fix, cmake will configure but the link step will fail with "libtile.a not found."

On the remote (vast.ai), the layout is:
- `/root/tile-cpp/` (synced by deploy.sh)
- `/root/tiles-compositor/` (synced by deploy.sh)
- `/root/tile_cuda_multi_kernel/` (synced by deploy.sh)

So the remote path should be `../tile-cpp` relative to `/root/tiles-compositor/`, which resolves to `/root/tile-cpp/`. The fix `../tile-cpp` works for both local and remote.

### deploy.sh Status

The deploy script (`vast-ai/deploy.sh`) is otherwise correct:
- `$TILES_DIR` resolves to `tiles-maxxing/` (parent of `vast-ai/`).
- `$TILES_DIR/tiles-compositor/` correctly finds the compositor inside tiles-maxxing.
- `$TILES_DIR/tile-cpp/` correctly finds tile-cpp.
- Remote rsync destinations (`/root/tiles-compositor/`, `/root/tile-cpp/`, `/root/tile_cuda_multi_kernel/`) are independent flat directories, which is fine.

**The only fix needed is the CMakeLists.txt TILE_CPP_ROOT path.**

### Build Commands on Remote

After deploying and fixing the CMakeLists.txt path:

```bash
# Build tile-cpp (libtile.a) -- already handled by deploy.sh Step 3
# Produces: /root/tile-cpp/build-k40/libtile.a

# Build tiles-compositor (libcompositor.a + campaign binary)
cd /root/tiles-compositor
rm -rf build-k40 && mkdir build-k40 && cd build-k40
cmake .. -DCMAKE_BUILD_TYPE=Release -DK_SQ=40
make -j$(nproc)

# Verify campaign binary exists
ls -la campaign
```

### Post-Deploy Verification

Before running the campaign:

```bash
# Quick grid sanity check
./dump_grid 600000000 | head -5
# Should show: num_towers ~1,657,306, total_tiles ~59,856,476

# Run with a tiny R to verify pipeline works end-to-end
./campaign 10000 --k-sq 40 --cuda --burst-size 64 --progress-interval 10
# Should complete in under 1 second with SPANNING verdict
```

---

## Risk Assessment

### Low Risk

- **Overflow tiles.** K_SQ=40 at R=600M/850M has a proven zero-overflow track record (75M-tile octant census). Not a concern.
- **GPU throughput.** 155K tiles/sec on RTX 4090 is well-characterized (tuning sweep 2026-04-11). Consistent across batch sizes >= 10K.
- **Memory.** ~10 GiB total, well within 24 GB VRAM.

### Medium Risk

- **Compositor baseline unknown at K_SQ=40.** The 1.2 ms/tower baseline from the K_SQ=36 postmortem may not transfer directly. K_SQ=40 has fewer groups per tile (median 10 vs higher at K_SQ=36) so compositor work should be less, but the `pre_flatten_tower()` step (path-compress all groups in the tower) scales with total groups and may be a surprise bottleneck.
- **Union-find memory growth.** The `parent_` and `root_reach_` vectors grow unbounded. At ~60M tiles * ~10 groups/tile = ~600M entries * 5 bytes = ~3 GB. The OOM cap in compositor.cpp triggers at 2 billion entries. At 600M entries we have 3.3x headroom.

### Higher Risk

- **The O(N^2) fix correctness.** The fix replaced `has_spanning()` (which rebuilt hash sets from scratch every call) with `check_spanning_incremental()` (which returns a cached `spanning_detected_` flag set during `unite()` and `mark_inner()`/`mark_outer()`). The correctness depends on:
  - `spanning_detected_` being set whenever a union merges two trees whose combined reachability is REACH_BOTH (inner + outer). This happens in `unite()` at line 264.
  - `mark_inner()`/`mark_outer()` checking after updating a root's reach flags (lines 272, 280).
  - The debug-mode cross-check in `finalize()` (line 222-229) compares the incremental flag against the set-intersection method. This validates correctness but only runs at finalize time, not during the sweep.
  
  If a bug exists where `spanning_detected_` is not set when it should be (e.g., a race in root compression during `find()`), the early-exit run would fail to detect spanning. The full-sweep run would show correct behavior in finalize but incorrect behavior in incremental checks. **Run 1 (early exit) is the primary correctness test.**

- **Full-sweep patch correctness.** Option A (commenting out the break) means the compositor continues ingesting towers after spanning is found. The `collect_outer_boundary(j)` call that normally happens at the spanning tower (line 611-612) would be skipped for intermediate towers. This is fine because `collect_outer_boundary_ingest()` already handles intermediate towers' boundaries during `ingest_tower()`. The explicit `collect_outer_boundary()` is only needed for the rightmost tower (the last one processed). With the patch, that would be the actual last tower, and the code after the loop (line 705-708) handles the `!spanning_found` case which would trigger `collect_outer_boundary(num_towers - 1)`. But wait: `spanning_found` would still be set to true (we only commented out the `break`, not the flag set). So line 705 would NOT call `collect_outer_boundary`. We need to also suppress the `spanning_found = true` assignment, OR move the final `collect_outer_boundary` call to always run on the last tower. Safer to suppress both `spanning_found = true` and the `break`.

- **vast.ai instance cost.** At $0.28/hr, the full-sweep run (20-34 min) costs ~$0.10-$0.16. The early-exit run costs pennies. Total campaign budget: under $0.50. Risk: instance eviction during the full-sweep run (rare on reserved instances, but possible on interruptible ones). Use `tmux` and reserve a non-interruptible instance.
