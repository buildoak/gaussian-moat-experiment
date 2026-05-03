# Wave 1 Performance Report - 2026-05-03

## Context

- Branch: `opt/performance-wave-1`
- Measured implementation commit: `c5151f2 Optimize streaming compositor group lookup`
- Previous measured implementation commit: `62324c9 Optimize streaming compositor frontier remap`
- Previous report commit: `59e2709 Record rejected compositor reserve patch`
- Baseline commit: `8d88f62 Expand performance optimization menu`
- Hardware: Vast.ai RTX 4090, 24564 MiB
- Driver / compiler: NVIDIA driver `560.35.03`, CUDA compiler `12.4.131`
- Build type: `Release`
- CMake CUDA arch: `89`

## Accepted Changes

1. `2fdef61 Add CUDA profile summarizer`
2. `39ed27c Optimize streaming compositor column lookups`
3. `f46f123 Summarize overflow stats on device`
4. `efe2a67 Add optional compositor overlap pipeline`
5. `c55b631 Parallelize face representative encoding`
6. `0277e8c Add CUDA stage timing profiles`
7. `f7edf6d Skip redundant MR trial division`
8. `2d26cce Tune MR kernel block size`
9. `7c52472 Optimize face representative extraction`
10. `be610f1 Tune UF kernel block size`
11. Parallelize grid tower construction and reduce I4 invariant verification to
    interval-boundary checks
12. Append column coordinates directly into CUDA batches instead of allocating a
    temporary vector per column
13. Add K1 launch/register build knobs plus a compile-time guard that prevents
    `K1_BLOCK_THREADS < ACTIVE_ROWS`
14. Replace streaming compositor frontier compaction's ordered root map with a
    dense `NodeId` remap vector
15. Replace streaming compositor's global current-group hash map with a dense
    per-current-column group table

## Rejected Experiments

These were tested after the accepted UF block-size candidate and deliberately
left out of the branch state:

| Experiment | Result | Decision |
|---|---|---|
| Dispatcher resource persistence | CUDA generation improved, but compositor timing regressed in the same run | Preserved locally as stash `wip dispatcher resource persistence mixed perf` |
| K1 split-prime `b_start mod p` precompute | CUDA CTest and diff probes passed, but legal smoke regressed to `9.590s` total / `6.503s` CUDA / `1.127s` K1 | Reverted |
| UF read-only final find | CUDA CTest and diff probes passed; SPANNING full improved slightly to `148.913s`, but MOAT full regressed to `149.882s` | Reverted |
| UF shared parent scratch | CUDA CTest and diff probes passed, but smoke UF regressed to about `1.032s` | Reverted |
| Face packed-unpack micro | CUDA CTest and diff probes passed, but face encode regressed on smoke | Reverted |
| Direct column append with exact per-column `reserve` | Correctness passed, but forced repeated reallocations and stalled CPU-side before first GPU dispatch | Reworked without the per-column reserve |
| K1 launch/register retuning | Smoke sweep tested `272/288/320` block sizes and `36/40/48` register caps; no variant beat the current `288`, maxrregcount `40` default on total time | Keep current default; retain knobs and guard |
| Streaming compositor no-coordinate-vector refactor | Local tests, CUDA CTest, and diff probes passed; large-radius sample improved slightly, but full MOAT regressed versus dense-remap baseline (`146.779s` vs `146.502s`) | Reverted |
| Streaming compositor reserve-only patch | Local compositor tests passed, but large-radius sample regressed versus dense-remap baseline (`83.972s` vs `83.338s`) | Reverted |

## Commands

Baseline:

```bash
scripts/run_tsuchimura_gate.sh \
  --cuda-bin ./build-k36/campaign_main_cuda \
  --chunk-size 200000 \
  --timing \
  --profile-dir /workspace/opt-wave1-baseline-20260502-224926/profiles
```

Candidate without overlap:

```bash
scripts/run_tsuchimura_gate.sh \
  --cuda-bin ./build-k36/campaign_main_cuda \
  --chunk-size 500000 \
  --timing \
  --profile-dir /workspace/opt-wave1-overflow-summary-20260503-001015/profiles
```

Candidate with app-level CPU/GPU overlap:

```bash
./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=80000000 \
  --r-outer=80015790 \
  --region full-octant \
  --chunk-size=500000 \
  --overlap-compositor \
  --no-early-exit \
  --timing \
  --profile /workspace/opt-wave1-overlap-20260503-003309/profiles/R80015790_moat.profile.json
```

Stage-timed candidate:

```bash
./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=80000000 \
  --r-outer=80015790 \
  --region full-octant \
  --chunk-size=500000 \
  --overlap-compositor \
  --no-early-exit \
  --timing \
  --profile /workspace/opt-wave1-stage-timing-full-20260503-012121/profiles/R80015790_moat.profile.json
```

MR prefiltered candidate:

```bash
./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=80000000 \
  --r-outer=80015790 \
  --region full-octant \
  --chunk-size=500000 \
  --overlap-compositor \
  --no-early-exit \
  --timing \
  --profile /workspace/opt-wave1-mr-prefilter-full-20260503-013224/profiles/R80015790_moat.profile.json
```

MR block-size candidate:

```bash
./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=80000000 \
  --r-outer=80015790 \
  --region full-octant \
  --chunk-size=500000 \
  --overlap-compositor \
  --no-early-exit \
  --timing \
  --profile /workspace/opt-wave1-mr-block256-full-20260503-014542/profiles/R80015790_moat.profile.json
```

Face representative candidate:

```bash
./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=80000000 \
  --r-outer=80015790 \
  --region full-octant \
  --chunk-size=500000 \
  --overlap-compositor \
  --no-early-exit \
  --timing \
  --profile /workspace/opt-wave1-face-reps-full-20260503-020208/profiles/R80015790_moat.profile.json
```

UF block-size candidate:

```bash
./build-k36/campaign_main_cuda \
  --k-sq=36 \
  --r-inner=80000000 \
  --r-outer=80015790 \
  --region full-octant \
  --chunk-size=500000 \
  --overlap-compositor \
  --no-early-exit \
  --timing \
  --profile /workspace/opt-wave1-uf-block128-full-20260503-021501/profiles/R80015790_moat.profile.json
```

Chunk sweep after compositor optimization:

```bash
for chunk in 50000 100000 200000 500000; do
  ./build-k36/campaign_main_cuda \
    --k-sq=36 \
    --r-inner=80000000 \
    --r-outer=80015790 \
    --region full-octant \
    --chunk-size="$chunk" \
    --no-early-exit \
    --timing \
    --profile "/workspace/opt-wave1-chunk-sweep-20260502-234245/profiles/R80015790_moat_chunk${chunk}.profile.json"
done
```

## Correctness Gates

| Gate | Status | Evidence |
|---|---|---|
| CPU CTest | PASS | `108/108` passed on compositor patch, 2 pre-existing skips |
| CUDA CTest | PASS | `13/13` passed on overflow, overlap, stage timing, and MR prefilter patches |
| CPU/CUDA diff probe | PASS | `cuda_vs_cpu_diff --m4 --k5 --verbose --limit 16` passed for both Tsuchimura cases |
| Snapshot SHA smoke | PASS | CPU/CUDA snapshot SHA matched |
| Snapshot/overlap exclusion | PASS | `--overlap-compositor` with `--snapshot-out` exits `2` |
| Tsuchimura gate | PASS | `80015782 => SPANNING`, `80015790 => MOAT`, zero overflows |
| Stage profile gate | PASS | `/workspace/opt-wave1-stage-timing-full-20260503-012121`, all CUDA stage timing fields present |
| MR prefilter gate | PASS | `/workspace/opt-wave1-mr-prefilter-full-20260503-013224`, Tsuchimura verdicts correct, zero overflows |
| MR block-size gate | PASS | `/workspace/opt-wave1-mr-block256-full-20260503-014542`, Tsuchimura verdicts correct, zero overflows |
| Face reps gate | PASS | `/workspace/opt-wave1-face-reps-full-20260503-020208`, Tsuchimura verdicts correct, zero overflows |
| UF block-size gate | PASS | `/workspace/opt-wave1-uf-block128-full-20260503-021501`, Tsuchimura verdicts correct, zero overflows |
| Grid parallel gate | PASS | `/workspace/opt-wave1-grid-parallel-tsuchimura-full-20260503-025042`, Tsuchimura verdicts correct, zero overflows |
| Direct column-append gate | PASS | `/workspace/opt-wave1-direct-append-tsuchimura-full-20260503-030730`, Tsuchimura verdicts correct, zero overflows |
| Dense compositor remap gate | PASS | `/workspace/opt-wave1-compositor-dense-remap-tsuchimura-full-20260503-032726`, Tsuchimura verdicts correct, zero overflows |
| Dense compositor group-table gate | PASS | `/workspace/opt-wave1-compositor-dense-groups-tsuchimura-full-20260503-035240` plus MOAT repeat `/workspace/opt-wave1-compositor-dense-groups-moat-repeat-20260503-035751`, Tsuchimura verdicts correct, zero overflows |

## Full-Run Timing

Baseline was commit `8d88f62` with chunk `200000`. The current candidate uses
chunk `500000` and `--overlap-compositor`.

| Case | Metric | Baseline | Candidate | Delta |
|---|---|---:|---:|---:|
| SPANNING full | total seconds | 342.003 | 145.638 | -57.4% |
| SPANNING full | CUDA K1-K5 seconds | 180.726 | 141.570 | -21.7% |
| SPANNING full | compositor seconds | 155.930 | 24.651 | -84.2% |
| SPANNING full | pipeline tiles/s | 45,160 | 106,050 | +134.8% |
| MOAT full | total seconds | 424.070 | 145.339 | -65.7% |
| MOAT full | CUDA K1-K5 seconds | 176.305 | 141.277 | -19.9% |
| MOAT full | compositor seconds | 242.593 | 24.366 | -90.0% |
| MOAT full | pipeline tiles/s | 36,439 | 106,321 | +191.8% |

The final total is lower than `cuda_k1_k5 + compositor` because
`--overlap-compositor` runs one GPU batch ahead while the main thread ingests the
previous complete-column batch.

## Incremental Timing

| Stage | SPANNING full total | MOAT full total |
|---|---:|---:|
| Baseline `8d88f62`, chunk 200k | 342.003 | 424.070 |
| Compositor cursor, chunk 200k | 284.002 | 283.135 |
| Overflow summary, chunk 500k | 268.715 | 268.825 |
| Overlap flag, chunk 500k | 179.721 | 180.464 |
| Overlap repeat, chunk 500k | 179.350 | 180.194 |
| Face reps parallel, chunk 500k | 168.540 | 169.173 |
| Stage-timed profile, chunk 500k | 167.388 | 166.564 |
| MR prefilter, chunk 500k | 162.566 | 163.833 |
| MR block 256, chunk 500k | 162.244 | 163.427 |
| Face reps one-pass, chunk 500k | 150.468 | 150.357 |
| UF block 128, chunk 500k | 149.040 | 149.207 |
| Parallel grid build, chunk 500k | 147.796 | 148.148 |
| Direct column append, chunk 500k | 147.894 | 146.942 |
| Dense compositor remap, chunk 500k | 147.171 | 146.502 |
| Dense compositor group table, chunk 500k | 145.638 | 145.339 |

Repeat evidence used run directory:
`/workspace/opt-wave1-overlap-repeat-20260503-004319`.

Face-representative parallelization evidence used run directory:
`/workspace/opt-wave1-face-parallel-20260503-005905`.

Stage-timing evidence used run directory:
`/workspace/opt-wave1-stage-timing-full-20260503-012121`.

MR prefilter evidence used run directory:
`/workspace/opt-wave1-mr-prefilter-full-20260503-013224`.

MR block-size sweep evidence used run directory:
`/workspace/opt-wave1-mr-launch-sweep-20260503-014029`.

MR block-size gate evidence used run directory:
`/workspace/opt-wave1-mr-block256-full-20260503-014542`.

Face block-size sweep evidence used run directory:
`/workspace/opt-wave1-face-block-sweep-20260503-015740`.

Face representative extraction evidence used run directory:
`/workspace/opt-wave1-face-reps-full-20260503-020208`.

UF block-size sweep evidence used run directory:
`/workspace/opt-wave1-uf-block-sweep-20260503-021044`.

UF block-size gate evidence used run directory:
`/workspace/opt-wave1-uf-block128-full-20260503-021501`.

Larger-radius readiness sample evidence used run directory:
`/workspace/opt-wave1-r1100m-sample-20260503-023454`.

Grid I4 interval-check sample evidence used run directory:
`/workspace/opt-wave1-grid-i4-r1100m-sample-20260503-024408`.

Parallel grid-build larger-radius sample evidence used run directory:
`/workspace/opt-wave1-grid-parallel-r1100m-sample-20260503-024806`.

Parallel grid-build Tsuchimura evidence used run directories:
`/workspace/opt-wave1-grid-parallel-tsuchimura-20260503-024959` for early
SPANNING and `/workspace/opt-wave1-grid-parallel-tsuchimura-full-20260503-025042`
for the two full cases.

Direct column-append larger-radius sample evidence used run directory:
`/workspace/opt-wave1-direct-append2-r1100m-sample-20260503-030439`.

Direct column-append Tsuchimura evidence used run directory:
`/workspace/opt-wave1-direct-append-tsuchimura-full-20260503-030730`.

K1 launch/register sweep evidence used run directory:
`/workspace/opt-wave1-k1-launch-sweep-20260503-031539`.

Dense compositor remap larger-radius sample evidence used run directory:
`/workspace/opt-wave1-compositor-dense-remap-r1100m-sample-20260503-032435`.

Dense compositor remap Tsuchimura evidence used run directory:
`/workspace/opt-wave1-compositor-dense-remap-tsuchimura-full-20260503-032726`.

Dense compositor group-table larger-radius sample evidence used run directory:
`/workspace/opt-wave1-compositor-dense-groups-r1100m-sample-20260503-034950`.

Dense compositor group-table Tsuchimura evidence used run directory:
`/workspace/opt-wave1-compositor-dense-groups-tsuchimura-full-20260503-035240`.

Dense compositor group-table MOAT repeat evidence used run directory:
`/workspace/opt-wave1-compositor-dense-groups-moat-repeat-20260503-035751`.

Rejected no-coordinate-vector compositor evidence used run directories:
`/workspace/opt-wave1-compositor-no-coords2-r1100m-sample-20260503-033700`
and `/workspace/opt-wave1-compositor-no-coords-tsuchimura-full-20260503-033947`.

Rejected compositor reserve-only evidence used run directory:
`/workspace/opt-wave1-compositor-reserve-r1100m-sample-20260503-034606`.

## CUDA Stage Timing

The profile schema now includes `cuda_stage_timings_seconds`. `--profile` no
longer implies expensive first-overflow diagnostics; pass
`--overflow-diagnostics` when those tile-level diagnostics are needed.

| Case | H2D | K1 sieve | MR | Compact | UF | Face encode | Face sort/pack | Overflow summary | D2H |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| SPANNING early | 0.006 | 2.841 | 7.745 | 0.105 | 3.302 | 3.878 | 0.141 | 0.001 | 0.039 |
| SPANNING full | 0.039 | 21.969 | 59.615 | 0.804 | 25.439 | 29.868 | 1.088 | 0.004 | 0.223 |
| MOAT full | 0.038 | 22.051 | 59.766 | 0.807 | 25.525 | 29.956 | 1.091 | 0.004 | 0.218 |

After `f7edf6d`, MR drops to `56.839s` on SPANNING full and `56.945s`
on MOAT full. The optimization skips the 12-prime trial division only on the
non-axis K1-presieved path; axis candidates keep the full rational primality
path.

After `2d26cce`, MR uses a `256` thread launch by default. The full gate puts
MR at `56.764s` on SPANNING full and `56.918s` on MOAT full. The sweep also
tested 256/288/320 blocks and 44/48 register caps; uncapped 256 was the
cleanest candidate.

After `7c52472`, face encode uses a `128` thread launch and a one-pass
best-representative extraction. Face encode drops to `17.845s` on SPANNING full
and `17.906s` on MOAT full.

After `be610f1`, UF uses a `128` thread launch by default. UF drops to
`24.156s` on SPANNING full and `24.218s` on MOAT full.

After the parallel grid-build candidate, grid initialization drops to `0.234s`
on SPANNING full and `0.216s` on MOAT full. CUDA stage buckets are effectively
unchanged, as expected; this is a CPU-side pre-dispatch optimization.

After the direct column-append candidate, the app avoids allocating a temporary
`std::vector<TileCoord>` for every column during CUDA batch assembly. The full
SPANNING run is noise-flat at `147.894s`; the full MOAT run improves to
`146.942s`.

K1 launch/register sweep results:

| K1 block | maxrregcount | Total seconds | K1 seconds | CUDA K1-K5 seconds |
|---:|---:|---:|---:|---:|
| 272 | 40 | 7.624 | 1.127 | 5.931 |
| 288 | 40 | 7.593 | 1.128 | 5.900 |
| 320 | 40 | 7.976 | 1.124 | 6.238 |
| 288 | 36 | 8.466 | 1.126 | 5.901 |
| 288 | 48 | 7.705 | 1.125 | 5.913 |

Decision: no K1 launch/register default change. The sweep did expose a safety
constraint, so `K1_BLOCK_THREADS` is now explicit and compile-time checked
against `ACTIVE_ROWS`.

After the dense compositor remap candidate, `compact_to_frontier()` uses a
vector indexed by dense `NodeId` roots instead of an ordered map. This preserves
first-seen frontier compact ordering while removing tree lookups from the hot
compaction path. Full MOAT compositor time drops to `68.905s`.

After the dense compositor group-table candidate, the current-column group
lookup uses a dense vector slot:
`(tile_index - current_group_base_index) * 128 + group_label - 1`. This is valid
because current-column nodes are either compacted into the frontier after the
column or discarded. Previous-column stitching still goes through frontier
nodes. Full MOAT repeat compositor time drops to `24.366s`.

Current bottleneck read: MR is still the largest CUDA kernel bucket, followed
by K1 sieve, UF, then face encode. The overlapped full pipeline is now bounded
by roughly `141s` CUDA generation; compositor ingestion is mostly hidden behind
the CUDA batch stream and no longer dominates host-side runtime.

## Larger Radius Readiness

This is a narrow legal-annulus sample, not an external truth gate. It checks
whether the current pipeline remains stable at a larger radius and whether
overflow pressure appears before attempting wider, expensive bands.

| Radius / width | Candidate | Active tiles | Total seconds | Grid-init seconds | CUDA K1-K5 seconds | Compositor seconds | Pipeline tiles/s | Overflowed tiles |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `R=1,100,000,000`, width `500` | UF block 128 | 10,888,283 | 105.638 | 24.532 | 76.895 | 42.728 | 103,072 | 0 |
| `R=1,100,000,000`, width `500` | I4 interval only | 10,888,283 | 106.663 | 24.421 | 78.015 | 42.776 | 102,081 | 0 |
| `R=1,100,000,000`, width `500` | Parallel grid build | 10,888,283 | 86.152 | 2.508 | 79.397 | 42.429 | 126,384 | 0 |
| `R=1,100,000,000`, width `500` | Direct column append | 10,888,283 | 84.213 | 2.645 | 77.348 | 42.801 | 129,294 | 0 |
| `R=1,100,000,000`, width `500` | Dense compositor remap | 10,888,283 | 83.338 | 2.569 | 76.878 | 35.225 | 130,652 | 0 |
| `R=1,100,000,000`, width `500` | Dense compositor group table | 10,888,283 | 82.570 | 2.670 | 77.020 | 13.546 | 131,867 | 0 |

Stage breakdown:

| Candidate | H2D | K1 sieve | MR | Compact | UF | Face encode | Face sort/pack | Overflow summary | D2H |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| UF block 128 | 0.027 | 15.462 | 33.340 | 0.295 | 8.951 | 5.997 | 0.757 | 0.003 | 0.160 |
| Parallel grid build | 0.028 | 15.471 | 33.346 | 0.295 | 8.953 | 6.001 | 0.757 | 0.003 | 0.165 |
| Direct column append | 0.026 | 15.457 | 33.330 | 0.295 | 8.947 | 5.997 | 0.757 | 0.003 | 0.154 |
| Dense compositor remap | 0.026 | 15.468 | 33.344 | 0.295 | 8.953 | 6.002 | 0.757 | 0.003 | 0.154 |

Read: overflow remains clean at this radius and throughput is stable at about
`132k` pipeline tiles/s after the dense compositor group table. The
interval-only I4 change was correctness-preserving but not performance-material
by itself; the large grid win comes from computing independent column towers in
parallel before the deterministic prefix-sum pass. Direct column append adds a
smaller host-loop win by eliminating per-column temporary vector allocation.
Dense compositor remap cuts frontier compaction overhead, and the dense group
table cuts the hot current-group lookup without changing TileOp math.

## Chunk Sweep

Post-compositor full MOAT chunk sweep at `39ed27c`:

| Chunk | Total seconds | CUDA seconds | Compositor seconds | Pipeline tiles/s |
|---:|---:|---:|---:|---:|
| 50,000 | 285.894 | 191.913 | 91.229 | 54,050 |
| 100,000 | 284.318 | 189.453 | 92.154 | 54,350 |
| 200,000 | 283.578 | 185.679 | 92.146 | 54,492 |
| 500,000 | 270.074 | 173.459 | 91.391 | 57,216 |

Decision: use `--chunk-size 500000` as the current steady-state K36 default
candidate for full runs. Early-exit responsiveness remains close across
chunks; smaller chunks avoid extra produced tiles before early exit.

## Safety

- Overflow counters: all four counters remained zero in all accepted full
  Tsuchimura gates and profile summaries.
- Verdicts: external truth cases preserved.
- Grid invariants: focused tests cover both upper and lower diagonal-orphan
  witnesses after replacing the per-tile I4 scan with interval-boundary checks.
- Column ordering: direct append emits the same column-major `(i,j,a_lo,b_lo)`
  sequence as `Grid::enumerate_column_tiles()` for non-sparse tower grids.
- Frontier compaction: dense remap preserves first-seen compact component order
  because entries are still processed in `next_frontier` order.
- Current-group lookup: dense current-column group table preserves per-tile
  group identity inside a column; previous-column stitching still uses frontier
  nodes, not stale global group ids.
- Snapshot mode affected: overlap is rejected with snapshot mode; serial
  snapshot SHA smoke passed.
- Byte layout affected: no TileOp layout changes.
- Methodology/canon affected: no verification stack changes.

## Decision

Accept the compositor cursor lookup, device-side overflow summary, explicit
`--overlap-compositor` app-level pipeline, face representative
parallelization, profile-only CUDA stage timing attribution, and the
K1-presieved MR primality path, the MR 256-thread launch default, and one-pass
face representative extraction, the UF 128-thread launch default, and the
parallel grid tower build with interval-boundary I4 verification, and direct
column append for CUDA batch assembly, dense streaming-compositor frontier root
remapping, and dense streaming-compositor current-group lookup. Keep K1 at
`288` threads and `--maxrregcount=40`, with explicit build knobs for future
sweeps.

Next high-leverage targets:

1. Continue MR arithmetic work only if a concrete Montgomery shortcut is found;
   launch/register tuning is exhausted for now.
2. Revisit dispatcher resource persistence only after isolating why one run
   improved CUDA time but worsened compositor time.
3. Inspect K1 next; it now accounts for about `22.0s` on full MOAT and
   `15.5s` on the `R=1.1B`, width `500` sample.
4. Decide whether `--overlap-compositor` should become default after another
   verification repeat.
