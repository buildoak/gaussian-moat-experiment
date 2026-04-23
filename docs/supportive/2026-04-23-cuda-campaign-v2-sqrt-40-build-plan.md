---
title: CUDA Campaign v2 sqrt-40 Build Plan
date: 2026-04-23
engine: codex
type: design-note
status: complete
refs: [tiles-maxxing/cuda-campaign-v2-sqrt-36, tiles-maxxing/cpp-campaign-v2, docs/supportive/2026-04-14-k40-vs-k36-campaign-delta.md, docs/supportive/2026-04-20-codex-tileop-sizing-study.md, legacy/research/2026-03-26-k40-pipeline-audit.md]
---

# CUDA Campaign v2 sqrt-40 Build Plan

## Summary

Do not port the stale `campaign-sqrt-40/` tree forward. The current v2 CUDA implementation is already parameterized through `-DK_SQ=N`; the approved K=40 setup is a sibling `tiles-maxxing/cuda-campaign-v2-sqrt-40/` tree copied from the validated v2 CUDA source with only naming/default changes.

One source fix is required before that build is trustworthy: `include/cuda_campaign/constants.cuh` has a wrong K=40 static assertion for `NUM_BACKWARD_OFFSETS`. Under the v3 snapped-grid rules, `C=floor_isqrt(40)=6`, `SIDE_EXP=269`, and the backward half-plane count for `dr^2 + dc^2 <= 40` is 64, not 56. The stale planning note that K=40 "re-derives to 56" is incorrect.

## Audit Findings

### Existing `tiles-maxxing/campaign-sqrt-40/`

This is v1-era code, not the target for new K=40 exploration.

- It uses `K_SQ_VAL` and `COLLAR=ceil_isqrt(K_SQ)`, so K=40 has `COLLAR=7` and `SIDE_EXP=271`.
- It emits the old 128 B `TileOp` format with `MAX_TOTAL_PORTS_GPU=128`.
- It has historical K40 validation value as a performance and overflow reference, but its geometry and compositor model are superseded by blueprint v3.
- Its K5 face-prime overflow path historically clamped without poisoning. Even if K40 did not hit that path in known runs, it should not be copied.

### Delta Doc: `2026-04-14-k40-vs-k36-campaign-delta.md`

Useful for historical context only. It accurately describes the old split where K40 used 128 B TileOps and K36 moved to 256 B, but v3 now mandates one 256 B format for both K values.

Carry forward:

- K40 tends to have fewer components and ports than K36 because the larger step radius merges more local clusters.
- Historical K40 overflow was near zero at operating radii.
- K40 R=600M and R=850M/900M results are useful smoke expectations, not ground truth for v3 moat proof.

Do not carry forward:

- `COLLAR=7` for v3 face strips.
- 128 B TileOps.
- Dual standard/extended TileOp paths.
- Staircase or narrow-shell verdict semantics.

### Legacy K40 Research

Legacy fat-stripe work found a likely blocked regime:

- 800M connected and 850M blocked in the audited Jetson sweep.
- 1.03B to 1.09B blocked in Mac fat-stripe probes.
- ISE notes place the rough transition around 839M.

These are reconnaissance signals only. The audit found off-axis geometry and checkpointing weaknesses, and the old predicate was not the v3 full-annulus flag-driven proof. Use them to choose first radii, not as validation oracles.

## K36 to K40 Differences in v2

### Build Parameter Flow

`K_SQ` enters at CMake:

```bash
cmake -S tiles-maxxing/cuda-campaign-v2-sqrt-40 -B build-k40 \
  -DK_SQ=40 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89
```

The CUDA CMake target passes `K_SQ=${K_SQ}` to `campaign_cuda`, `campaign_main_cuda`, `cuda_vs_cpu_diff`, and test binaries. The CUDA target also pulls in `../cpp-campaign-v2`, whose `campaign/constants.h` derives `campaign::k_sq_value`, `C`, `SIDE_EXP`, `TILEOP_SIZE`, port budgets, and geo-test constants from the same compile definition.

Runtime `--k-sq` must match the compile-time value. `campaign_main_cuda` rejects mismatches.

### Geometry and Connectivity

For v3:

| Item | K=36 | K=40 |
|---|---:|---:|
| `C = floor_isqrt(K)` | 6 | 6 |
| `HALO`, `COLLAR` | 6 | 6 |
| `SIDE_EXP = 257 + 2C` | 269 | 269 |
| full in-disc neighbor offsets | 112 | 128 |
| backward offsets | 56 | 64 |
| prefilter ceil sqrt | 6 | 7 |

The important split is that face-strip/halo depth remains 6 for both K values, but K40 still includes extra offsets inside the radius-squared disk, such as norm 37 and 40 offsets. The geo prefilter must use `ceil_isqrt(K)`: 6 for K36, 7 for K40.

### TileOp, Ports, and Overflow

TileOp v3 is unchanged between K values:

- 256 B fixed format.
- `MAX_PORTS_PER_TILE=192`.
- `MAX_GROUPS_PER_TILE=128`.
- per-group `inner_flags` and `outer_flags`.
- overflow is conservative SPANNING-biased via `OVERFLOW_BIT`.

Sizing evidence says K40 is lower risk than K36. The 2026-04-20 sizing study at R=800M saw K40 max `sum(n)=89` and `max_label=62`, versus K36 max `sum(n)=104` and `max_label=80`. Both are below v3 budgets, but K40 has more headroom.

## Adaptability Assessment

We cannot "just rebuild" today because of the K40 backward-offset assertion bug. After that fix, the current source should support a K40 build without copying kernel logic.

Required code changes:

1. `tiles-maxxing/cuda-campaign-v2-sqrt-36/include/cuda_campaign/constants.cuh`
   - Change the K40 `NUM_BACKWARD_OFFSETS` assertion from 56 to 64.
   - Add or adjust a test/gate that verifies K36=56 and K40=64.

2. Dedicated K40 product path:
   - Copy `tiles-maxxing/cuda-campaign-v2-sqrt-36/` to `tiles-maxxing/cuda-campaign-v2-sqrt-40/`.
   - Make only naming/default changes: default `K_SQ=40`, project name, README/build snippets, artifact paths.
   - Do not fork algorithmic code.

No planned changes:

- TileOp layout.
- port and group budgets.
- compositor verdict logic.
- geo-test formulas.
- `MAX_PRIMES_GPU=8192` and `MAX_CANDIDATES_GPU=16384`, unless K40 validation shows overflow.

## Implementation Plan

1. Fix the K40 constants gate.
   - Update `constants.cuh` static assert to expect K40 `NUM_BACKWARD_OFFSETS == 64`.
   - Add a small compile-time or unit-test check for the K36/K40 pair so this does not regress.

2. Build CPU reference for K40.
   - Configure `tiles-maxxing/cpp-campaign-v2/build-k40` with `-DK_SQ=40`.
   - Run `ctest`.
   - Confirm the committed 5-tile K40 golden still passes.

3. Build CUDA v2 for K40 on a CUDA host.
   - Use RTX 4090 CMake architecture `89`.
   - Use source path `tiles-maxxing/cuda-campaign-v2-sqrt-40` with output directory `build-k40`.
   - Run all CUDA tests under `ctest --test-dir build-k40 --output-on-failure`.

4. Run CPU/CUDA byte-parity gates.
   - `cuda_vs_cpu_diff --r-inner 800000000 --r-outer 800010000 --limit 1024`.
   - `scripts/run_snapshot_sha_gate.sh --full --k 40 --r-inner 800000000 --r-outer 800010000`.
   - Record constants hash, snapshot hash, and overflow counters.

5. Only after cross-validation passes, run first full-annulus K40 sanity points.
   - Connected-side sanity: `--r-inner 800000000 --r-outer 800010000`.
   - Blocked-side reconnaissance: `--r-inner 800000000 --r-outer 850000000`, then `900000000` or `1050000000` if needed.
   - These are exploratory because K40 has no Tsuchimura-style exact boundary in this repo.

Moat hunting is explicitly out of scope until the K40 CPU/CUDA cross-validation gates pass.

## First Validation Target

The first target for K40 is cross-checking against the C++ reference, not moat hunting.

- CPU/CUDA TileOp byte parity at K=40 must pass for representative tile batches.
- Snapshot SHA equality with the `cpp-campaign-v2` K40 goldens must pass, starting with `goldens/5tile-k40.manifest.json` and `goldens/5tile-k40.snapshot.bin`.
- The full C++ and CUDA test suites must pass for K=40 builds.

Only after these checks pass should K40 campaign runs be used for connected/blocked reconnaissance or moat-boundary searches.

## Validation Gates

Build gates:

- `cpp-campaign-v2` K40 config and build pass.
- CUDA K40 config and build pass.
- `ctest` pass for CPU and CUDA K40 builds.
- BZ check passes for K40 using `scripts/bz_config.json` (`800000000` to `800010000` currently present).

Correctness gates:

- CPU/CUDA TileOp parity at K40 for at least 1024 tiles near R=800M.
- Full snapshot SHA equality for the 5-tile K40 golden region.
- Full-octant smoke snapshot equality at `R_inner=800000000`, `R_outer=800010000`.
- Runtime `--k-sq=36` against a K40 binary must fail with the existing mismatch guard.

Campaign gates:

- `k1_cand_overflow_count == 0`.
- `k4_prime_overflow_count == 0`.
- `k4_group_overflow_count == 0`.
- `k5_port_overflow_count == 0`.
- If any overflow occurs, verdict remains sound but the run is not accepted as a clean K40 validation point.

Performance gates:

- Measure CUDA tile throughput and total pipeline throughput at the 800M smoke radius.
- Expect K40 to be comparable to or better than K36 v2 because components and ports are fewer, while K4 does 64 backward offsets instead of 56.
- If throughput falls materially below the current 63K total tiles/s baseline, collect per-kernel timings before changing code.

## Risk Areas

- Backward offset bug: current K40 constant gate is wrong. This is the only definite source blocker found.
- Historical doc drift: many pre-v3 K40 docs say `COLLAR=7`; v3 requires `C=6` for face strips and halo, while geo prefilter still uses `ceil_isqrt(40)=7`.
- False validation oracle: legacy 800M connected / 850M blocked results came from fat-stripe or v1-style paths. They guide radius choice but cannot certify the v2 result.
- Run size: full annuli from 800M to 1.1B are much larger than the K36 Tsuchimura reproduction. Use tmux and pinned vast.ai SSH endpoints.
- Snapshot I/O: v3 K40 uses 256 B per tile, unlike old K40's 128 B. Disk and transfer plans should assume the doubled format.
- Memory: per-tile device buffers are dominated by candidate/prime arrays, not TileOps. `MAX_PRIMES_GPU=8192` is expected to hold, but must be watched at 1B-scale radii.

## Open Questions

1. R_inner anchoring is deferred and must not be hardcoded into the K40 setup.
2. What K40 verdict should be considered the first post-validation reconnaissance target: reproduce old connected at 800M, find first MOAT by sweeping 800M to 900M, or jump to 1.05B to confirm the blocked regime?
3. Should K40 campaign outputs be written under a new K40 artifact root inside `cuda-campaign-v2-sqrt-40/`, or under a separate shared campaign-output location?
