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

## Rejected K34 Candidate Gate

Tsuchimura reports `sqrt(34)` finite with farthest distance `< 24,289,452`.
That is an upper bound on the origin-connected component, not a directly
specified annular boundary for this compositor.

The naive shell probe was tested and rejected as a gate:

```bash
cd /workspace/gaussian-moat-cuda-timing/tiles-maxxing/cuda-campaign-v2-sqrt-36
cmake -S . -B build-k34 -DK_SQ=34 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-k34 -j"$(nproc)"
./build-k34/campaign_main_cuda \
  --k-sq=34 \
  --r-inner=24289452 \
  --r-outer=24297644 \
  --region full-octant \
  --chunk-size=200000 \
  --no-early-exit \
  --timing \
  --profile /workspace/profiles-k34-20260503-165257/R24289452_moat.profile.json
```

Observed at commit `9e69542` on the Vast RTX 4090:

| Case | Tiles | Total | CUDA K1-K5 | Compositor | Verdict | Overflow counters |
|---|---:|---:|---:|---:|---|---|
| `K_SQ=34`, `R_inner=24289452`, `R_outer=24297644` | `2,479,915` | `27.2406s` | `22.1916s` | `4.10526s` | `SPANNING` | all zero |

This does not contradict Tsuchimura: his K34 result bounds the component of the
origin, while the shell compositor asks whether any prime component spans the
chosen annulus. K34 can become a strong gate only if we implement direct
origin-component verification or obtain an exact externally justified annular
boundary.

The corrected outer-bound placement was also tested:

| Case | Tiles | Total | CUDA K1-K5 | Compositor | Verdict | Overflow counters |
|---|---:|---:|---:|---:|---|---|
| `K_SQ=34`, `R_inner=24281260`, `R_outer=24289452` | `2,479,110` | `27.639s` | `22.5806s` | `4.08228s` | `SPANNING` | all zero |

This means the earlier inner/outer placement was wrong, but fixing that
placement still did not yield a K34 MOAT gate.

The centered placement was tested as well:

| Case | Tiles | Total | CUDA K1-K5 | Compositor | Verdict | Overflow counters |
|---|---:|---:|---:|---:|---|---|
| `K_SQ=34`, `R_inner=24285356`, `R_outer=24293548` | `2,479,579` | `27.7603s` | `22.8277s` | `4.01044s` | `SPANNING` | all zero |

## K34 Cross-K Regression Gate

This is accepted only as implementation regression coverage, not as external
truth:

```bash
cd /workspace/gaussian-moat-cuda-timing/tiles-maxxing/cuda-campaign-v2-sqrt-36
scripts/run_k34_regression_gate.sh \
  --cpu-bin ../cpp-campaign-v2/build-k34/campaign_main \
  --cuda-bin ./build-k34/campaign_main_cuda \
  --diff-bin ./build-k34/cuda_vs_cpu_diff \
  --chunk-size 200000 \
  --timing \
  --profile-dir /workspace/profiles-k34-regression
```

Expected:

| Step | Expected |
|---|---|
| K34 snapshot smoke | CPU/CUDA SHA match |
| K34 separate `cuda_vs_cpu_diff --m4 --limit 16` and `--k5 --limit 16` on `24289452..24297644` | pass |
| K34 shell sentinel `24289452..24297644` | `SPANNING`, zero overflow counters |

Accepted at commit `fc70d43` on the Vast RTX 4090:

| Step | Result |
|---|---|
| K34 snapshot smoke | PASS, CPU/CUDA SHA `1dc6c4dc031690a8849a59d94f6d2253c4c5b02a0c1b3a2db5d0c9935c2001e5` |
| K34 separate `cuda_vs_cpu_diff --m4 --limit 16` and `--k5 --limit 16` | PASS |
| K34 shell sentinel | `SPANNING`, zero overflow counters |

K34 shell sentinel timing at `fc70d43`, chunk `200000`:

| Tiles | App batches | Total | CUDA K1-K5 | Compositor |
|---:|---:|---:|---:|---:|
| `2,479,915` | `13` | `26.3411s` | `21.3462s` | `4.04465s` |

Verifier correction, 2026-05-04: the K34 gate now runs M4 and K5 diff as
separate commands. The M4 expected surface was corrected to match the actual
TileOp contract: only roots visible through ports or geo flags receive wire
labels. Post-fix K34 regression gate passed on the Vast RTX 4090 with
`SPANNING`, zero overflow counters, `2,479,915` tiles, `27.3446s` total.

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
