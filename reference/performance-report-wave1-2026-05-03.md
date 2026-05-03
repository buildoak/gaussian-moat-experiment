# Wave 1 Performance Report - 2026-05-03

## Context

- Branch: `opt/performance-wave-1`
- Candidate commit: `7c52472 Optimize face representative extraction`
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

The dispatcher resource-persistence experiment was tested but not accepted:
CUDA generation improved, but compositor timing regressed in the same run. It is
preserved locally as stash `wip dispatcher resource persistence mixed perf`.

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

## Full-Run Timing

Baseline was commit `8d88f62` with chunk `200000`. The final face-reps
candidate was commit `7c52472` with chunk `500000` and
`--overlap-compositor`.

| Case | Metric | Baseline | Candidate | Delta |
|---|---|---:|---:|---:|
| SPANNING full | total seconds | 342.003 | 150.468 | -56.0% |
| SPANNING full | CUDA K1-K5 seconds | 180.726 | 143.058 | -20.8% |
| SPANNING full | compositor seconds | 155.930 | 89.790 | -42.4% |
| SPANNING full | pipeline tiles/s | 45,160 | 102,646 | +127.3% |
| MOAT full | total seconds | 424.070 | 150.357 | -64.5% |
| MOAT full | CUDA K1-K5 seconds | 176.305 | 142.890 | -19.0% |
| MOAT full | compositor seconds | 242.593 | 90.088 | -62.9% |
| MOAT full | pipeline tiles/s | 36,439 | 102,773 | +182.0% |

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

Current bottleneck read: MR is still the largest CUDA kernel bucket, followed
by UF and K1 sieve, then face encode. The overlapped full pipeline is now
bounded by roughly `143s` CUDA generation and `90s` compositor ingestion, with
the compositor mostly hidden behind the CUDA batch stream.

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
- Snapshot mode affected: overlap is rejected with snapshot mode; serial
  snapshot SHA smoke passed.
- Byte layout affected: no TileOp layout changes.
- Methodology/canon affected: no verification stack changes.

## Decision

Accept the compositor cursor lookup, device-side overflow summary, explicit
`--overlap-compositor` app-level pipeline, face representative
parallelization, profile-only CUDA stage timing attribution, and the
K1-presieved MR primality path, the MR 256-thread launch default, and one-pass
face representative extraction.

Next high-leverage targets:

1. Continue MR arithmetic work only if a concrete Montgomery shortcut is found;
   launch/register tuning is exhausted for now.
2. Revisit dispatcher resource persistence only after isolating why one run
   improved CUDA time but worsened compositor time.
3. Inspect UF and K1 next; they now account for about `25.5s` and `22.0s`
   respectively on full MOAT.
4. Decide whether `--overlap-compositor` should become default after another
   verification repeat.
