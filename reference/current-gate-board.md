# Current Gate Board

Updated: 2026-05-02.

## Baseline

- Current local main: `3eda964 Record Vast 4090 verification results`.
- Verified implementation commit: `51b0975 Fix dispatcher reuse test fixture size`.
- Verification record: `tiles-maxxing/cuda-campaign-v2-sqrt-36/planning/2026-05-02-vast-4090-verification.md`.
- Verified hardware: Vast contract `36017558`, RTX 4090, driver `580.126.09`, CUDA `12.4.131`.

`3eda964` only records the verification result. The code verified on Vast was
`51b0975`; later branches should treat the verification note as provenance, not
as a code change.

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
| `R_inner=80000000`, `R_outer=80015782` | early exit | `SPANNING` | zero expected |
| `R_inner=80000000`, `R_outer=80015782` | `--no-early-exit` | `SPANNING` | zero required |
| `R_inner=80000000`, `R_outer=80015790` | `--no-early-exit` | `MOAT` | zero required |

## Baseline Performance

Vast RTX 4090 baseline, chunk `200000`:

| Case | Total | CUDA K1-K5 | Compositor | Full pipeline |
|---|---:|---:|---:|---:|
| early `80015782` SPANNING | `42.183s` | `25.936s` | `14.425s` | `39.0k tiles/s` |
| full `80015782` SPANNING | `353.998s` | `221.996s` | `128.225s` | `43.6k tiles/s` |
| full `80015790` MOAT | `353.263s` | `221.979s` | `127.488s` | `43.7k tiles/s` |

Optimization branches must report before/after profile JSON, command lines,
hardware, commit, and whether each gate above passed.
