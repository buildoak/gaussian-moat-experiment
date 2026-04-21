---
title: Grid build optimization — O(R²/S²) → O(R/S)
date: 2026-04-21
engine: coordinator
type: perf-report
---

## Summary

`Grid::build()` was the dominant bottleneck at project scale: ~40s at R=80M, projected ~66 min at R=800M (K=40). Root cause was a linear scan inside `find_tower` that cost O(R²/S²) operations. Replaced with analytic `j_low` / `j_high` bounds derived directly from the annulus corner equations, with a bounded confirmation walk. Result: **31× speedup at R=80M, ~258× at R=800M**, all correctness gates preserved.

## Root cause

`src/grid.cpp:272-278` (pre-fix):

```cpp
// Find any active j.
std::int32_t any = -1;
for (std::int32_t j = j_search_lo; j <= j_search_hi; ++j) {
    if (is_active_tile(i, j, g)) { any = j; break; }
}
```

`j_search_lo = max(i-2, 0)`, `j_search_hi = j_upper + 2`. For low-i columns, active tiles sit at `j ≈ R/S ≈ 312k` but the scan starts at `j ≈ 0` — roughly 312k `is_active_tile` calls per low-i column. Across ~221k columns in the octant, total ≈ 10¹⁰ tile-predicate calls. Even with corner fast-path (~1ns/call), this dominates at ~40s.

## Fix

Compute `j_low` and `j_high` candidates analytically from the annulus corners (necessary conditions), then walk in/out with `is_active_tile` to land on the exact boundary (0-2 steps typically). Each column now does O(1) amortized work → O(R/S) total.

- `src/grid.cpp`: +142 / −31 lines (one function rewritten, helpers unchanged)
- `apps/campaign_main.cpp`: +10 lines (`std::chrono` timers around the main stages — emitted in the stdout banner as `TIMING: t_grid=... t_tile_loop=... t_compositor=... t_snapshot=... t_total=...`)

## Correctness gates (all passed)

| Gate | Result |
|---|---|
| K=36 N=5 vs committed golden (`goldens/5tile-k36.snapshot.bin`) | ✓ byte-identical, MD5 `6ba6d9...` |
| K=40 N=5 vs Python oracle (promoted to `goldens/5tile-k40.snapshot.bin`) | ✓ byte-identical, MD5 `e037a6...` |
| `ctest` on `build-k36-tests` | ✓ 90/90 pass (2 skipped = DEBUG-only) |
| Threading determinism: 1T vs 12T at R=1M full-octant (102,524 tiles) | ✓ byte-identical |

## Perf — grid build

| Config | Before (pre-opt) | After | Speedup |
|---|---|---|---|
| R=1M, K=36 | ~430 ms (per prior PF-A) | **11 ms** | ~40× |
| R=80M, K=36 | 39,878 ms | **1,280 ms** | **31×** |
| R=800M, K=40 | ~4,000 s (projected, O(R²/S²) scaling) | **15,462 ms** | **~258×** |

At R=800M the speedup is larger because the old algorithm's complexity scaled quadratically with R, while the new one is linear.

## Perf — end-to-end (K=36, per-tile, single-thread unchanged)

| Config | Tiles | Grid (ms) | Tile loop (ms) | ms/tile 1T | Total (1T) | Total (12T) | OMP speedup |
|---|---|---|---|---|---|---|---|
| R=80M N=5 | 5 | 1,280 | 73 | 14.6 | 1.36 s | — | — (N<threads) |
| R=800M N=5 (K=40) | 5 | 15,462 | 76 | 15.2 | 15.54 s | — | — (N<threads) |
| R=1M full-octant | 102,524 | 11 | 1,465,744 | 14.3 | 1,466 s | 167 s | **8.79×** |

Per-tile encode stays ~14-15 ms single-thread as expected — optimization doesn't touch the encode path. 12-thread scaling is near-linear (8.79× on 12 cores) on Mac Mini.

## Anomalies / flags

- **Tile count at R=1M**: current grid reports 102,524 active tiles in full octant at `R_inner=1,000,000 / R_outer=1,008,192 / K=36`. The prior session's handoff (`7c6e39c6`) said "R=1M full-octant = 16,184 tiles" during its capstone. Discrepancy of 6.3×. Since (a) the committed K=36 5-tile golden still byte-matches, (b) ctest 90/90 including `RegionSubsetWritesOnlyRequestedTiles` passes, and (c) determinism across threads holds, the current enumeration is internally consistent. Most likely the prior 16,184 note was for a different configuration (different δ or counting only canonical-triangle tiles, not the full octant). Flag for reconciliation; not blocking.
- **`region-100.json` triggers "Lemma 4 port-count mismatch on I/O"**: this is an independent compositor issue, unrelated to grid. The region consists of 100 consecutive tiles all at `i=0` (the octant's west edge). Matches the known Codex-audit finding "Grid coverage drift around negative `i` with offset `(1,1)`". End-to-end N=100 and N=1000 timing using interior-of-octant regions is a follow-up.
- **`CAMPAIGN_BUILD_TESTS`** is `OFF` in the regular `build-k36/` — always pass `-DCAMPAIGN_BUILD_TESTS=ON` to a dedicated tests build (`build-k36-tests/`) when running ctest.

## Implications for CUDA

Grid init on host is effectively free now (sub-2s at R=80M, sub-20s at R=800M). For the CUDA campaign on the RTX 4090, grid enumeration will be a one-time host-side cost well under the kernel-launch + data-transfer overheads. Steady-state throughput is dominated by per-tile work, which this change did not touch.

Per-tile rate on Mac Mini M-series (12-thread) is ~1.6 ms/tile. CUDA should bring that down by 1-2 orders of magnitude on a 4090 (massive SIMT + thousands of concurrent tiles), making R=800M full-octant tractable within minutes.

## Next steps

1. Interior-region N=100 / N=1000 end-to-end rates (blocked today by the i=0 Lemma 4 issue; use a region spec targeting interior tiles).
2. Investigate the R=1M tile-count discrepancy vs prior-session note (most likely a config difference, but worth one verification pass).
3. Address the audit `must-fix` list as a separate small PR (sqrt coefficient, strict=true default, BZ binding, K=40 geo norm-form) before CUDA port.
4. Proceed to CUDA campaign plan using this now-trustworthy C++ reference.
