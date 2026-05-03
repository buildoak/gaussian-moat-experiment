# Wave 1 Performance Report - 2026-05-03

## Context

- Branch: `opt/performance-wave-1`
- Candidate commit: `0277e8c Add CUDA stage timing profiles`
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
| CUDA CTest | PASS | `13/13` passed on overflow and overlap patches |
| CPU/CUDA diff probe | PASS | `cuda_vs_cpu_diff --m4 --k5 --verbose --limit 16` passed for both Tsuchimura cases |
| Snapshot SHA smoke | PASS | CPU/CUDA snapshot SHA matched |
| Snapshot/overlap exclusion | PASS | `--overlap-compositor` with `--snapshot-out` exits `2` |
| Tsuchimura gate | PASS | `80015782 => SPANNING`, `80015790 => MOAT`, zero overflows |
| Stage profile gate | PASS | `/workspace/opt-wave1-stage-timing-full-20260503-012121`, all CUDA stage timing fields present |

## Full-Run Timing

Baseline was commit `8d88f62` with chunk `200000`. The final stage-timed
candidate was commit `0277e8c` with chunk `500000` and
`--overlap-compositor`.

| Case | Metric | Baseline | Candidate | Delta |
|---|---|---:|---:|---:|
| SPANNING full | total seconds | 342.003 | 167.388 | -51.1% |
| SPANNING full | CUDA K1-K5 seconds | 180.726 | 160.027 | -11.5% |
| SPANNING full | compositor seconds | 155.930 | 89.987 | -42.3% |
| SPANNING full | pipeline tiles/s | 45,160 | 92,270 | +104.3% |
| MOAT full | total seconds | 424.070 | 166.564 | -60.7% |
| MOAT full | CUDA K1-K5 seconds | 176.305 | 159.117 | -9.8% |
| MOAT full | compositor seconds | 242.593 | 90.006 | -62.9% |
| MOAT full | pipeline tiles/s | 36,439 | 92,773 | +154.6% |

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

Repeat evidence used run directory:
`/workspace/opt-wave1-overlap-repeat-20260503-004319`.

Face-representative parallelization evidence used run directory:
`/workspace/opt-wave1-face-parallel-20260503-005905`.

Stage-timing evidence used run directory:
`/workspace/opt-wave1-stage-timing-full-20260503-012121`.

## CUDA Stage Timing

The profile schema now includes `cuda_stage_timings_seconds`. `--profile` no
longer implies expensive first-overflow diagnostics; pass
`--overflow-diagnostics` when those tile-level diagnostics are needed.

| Case | H2D | K1 sieve | MR | Compact | UF | Face encode | Face sort/pack | Overflow summary | D2H |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| SPANNING early | 0.006 | 2.841 | 7.745 | 0.105 | 3.302 | 3.878 | 0.141 | 0.001 | 0.039 |
| SPANNING full | 0.039 | 21.969 | 59.615 | 0.804 | 25.439 | 29.868 | 1.088 | 0.004 | 0.223 |
| MOAT full | 0.038 | 22.051 | 59.766 | 0.807 | 25.525 | 29.956 | 1.091 | 0.004 | 0.218 |

Current bottleneck read: MR is the largest CUDA kernel bucket, followed by
face encode, then UF and K1 sieve. The overlapped full pipeline is now bounded
by roughly `159-160s` CUDA generation and `90s` compositor ingestion, with the
compositor mostly hidden behind the CUDA batch stream.

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
parallelization, and profile-only CUDA stage timing attribution.

Next high-leverage targets:

1. Attack MR first; it accounts for about `59.7s` of the full-run CUDA stage
   time.
2. Revisit dispatcher resource persistence only after isolating why one run
   improved CUDA time but worsened compositor time.
3. CUDA face extraction / encoding follow-ups: reduce packed-position divides
   and make sort/pack count-aware, gated by K5 byte parity.
4. Decide whether `--overlap-compositor` should become default after another
   verification repeat.
