# Current Gate Board

Updated: 2026-05-03 after zero-offset axis-alignment verification.

## Baseline

- Current branch: `fix/zero-offset-axis-alignment`.
- Verified implementation commit: `102b367 Align grid offset with canonical axis ownership`.
- Current documentation commit containing this board update: the commit containing this edit.
- Verified hardware: Vast RTX 4090 at `/workspace/gaussian-moat-cuda-timing`, CUDA architecture `89`.

The code verified on Vast was `102b367`. The later `b256987` commit only
documents the chunk-size benchmark contract in `AGENTS.md`; no compiled source
changed between the verified implementation and that documentation commit.

## Required Gates

Use this stack when accepting optimization work:

1. Local CPU gate:

```bash
cd tiles-maxxing/cpp-campaign-v2
cmake -S . -B build -DK_SQ=36 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
ctest --test-dir build --output-on-failure
```

2. Remote CUDA build and CTest on a CUDA host:

```bash
cd /workspace/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36
cmake -S . -B build-k36 -DK_SQ=36 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-k36 -j"$(nproc)"
ctest --test-dir build-k36 --output-on-failure
```

3. CPU/CUDA TileOp parity probes:

```bash
./build-k36/cuda_vs_cpu_diff --r-inner 80000000 --r-outer 80015782 --m4 --k5 --verbose --limit 16
./build-k36/cuda_vs_cpu_diff --r-inner 80000000 --r-outer 80015790 --m4 --k5 --verbose --limit 16
```

4. Snapshot smoke:

```bash
scripts/run_snapshot_sha_gate.sh \
  --smoke \
  --chunk-size 64 \
  --cpu-bin ../cpp-campaign-v2/build/campaign_main \
  --cuda-bin ./build-k36/campaign_main_cuda
```

5. Tsuchimura external truth gate:

```bash
scripts/run_tsuchimura_gate.sh \
  --cuda-bin ./build-k36/campaign_main_cuda \
  --chunk-size 200000 \
  --timing \
  --profile-dir /workspace/profiles
```

Expected:

| Case | Mode | Verdict | Overflow counters |
|---|---|---|---|
| `R_inner=80000000`, `R_outer=80015782` | early exit | `SPANNING` | zero required |
| `R_inner=80000000`, `R_outer=80015782` | `--no-early-exit` | `SPANNING` | zero required |
| `R_inner=80000000`, `R_outer=80015790` | `--no-early-exit` | `MOAT` | zero required |

6. Tsuchimura K34 upper-bound cross-K gate:

```bash
cd /workspace/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36
cmake -S . -B build-k34 -DK_SQ=34 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-k34 -j"$(nproc)"
scripts/run_tsuchimura_k34_gate.sh \
  --cuda-bin ./build-k34/campaign_main_cuda \
  --chunk-size 200000 \
  --timing \
  --profile-dir /workspace/profiles-k34
```

Expected:

| Case | Mode | Verdict | Overflow counters |
|---|---|---|---|
| `K_SQ=34`, `R_inner=24289452`, `R_outer=24297644` | `--no-early-exit` | `MOAT` | zero required |

This is weaker than the K36 two-case boundary because Tsuchimura reports
`sqrt(34)` finite with farthest distance `< 24,289,452`, not a nearby exact
SPANNING/MOAT bracket. Its value is cross-K coverage and overflow pressure.

## Baseline Performance

Vast RTX 4090 zero-offset correctness gate, chunk `200000`, snapshot disabled:

| Case | Produced / ingested tiles | App batches | Total | CUDA K1-K5 | Compositor | Full pipeline |
|---|---:|---:|---:|---:|---:|---:|
| early `80015782` SPANNING | `1,799,777 / 1,644,732` | `9` | `19.859s` | `16.332s` | `2.590s` | `82.8k ingested tiles/s` |
| full `80015782` SPANNING | `15,444,897 / 15,444,897` | `78` | `169.567s` | `141.977s` | `23.771s` | `91.1k tiles/s` |
| full `80015790` MOAT | `15,452,696 / 15,452,696` | `78` | `170.945s` | `143.251s` | `23.891s` | `90.4k tiles/s` |

Stage throughput for full cases:

| Case | CUDA K1-K5 | Compositor |
|---|---:|---:|
| full `80015782` SPANNING | `108.8k tiles/s` | `649.7k tiles/s` |
| full `80015790` MOAT | `107.9k tiles/s` | `646.8k tiles/s` |

Optimized full-pipeline MOAT spot check with `--overlap-compositor`, same
zero-offset build and hardware:

| Chunk size | App batches | Total | CUDA K1-K5 | Compositor | Full pipeline |
|---:|---:|---:|---:|---:|---:|
| `400000` | `39` | `142.827s` | `138.556s` | `24.916s` | `108.2k tiles/s` |
| `500000` | `31` | `144.160s` | `140.062s` | `24.560s` | `107.2k tiles/s` |

Both runs returned `MOAT` for `R_outer=80015790` with all overflow counters
zero. `400000` did not OOM and was slightly faster in this single current-build
spot check; treat it as the provisional optimized chunk size until a repeated
sweep confirms variance.

Small chunks such as `8192` are useful streaming stress tests but are not the
performance baseline. The 2026-05-03 zero-offset stress run with chunk `8192`
passed all Tsuchimura cases with zero overflows, but full-pipeline throughput
was about `54k tiles/s` because it used about `1,895` app batches.

Optimization branches must report before/after profile JSON, command lines,
hardware, commit, and whether each gate above passed.
