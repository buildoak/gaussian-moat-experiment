# CUDA Campaign Experiment Contract

This is the operational contract for experiments on the current
`tiles-maxxing/` C++/CUDA campaign surface. It is not a math specification; the
math specification is `methodology/tile-operator-definition-v-claude.md`.

## Default Doctrine

Use the compact verification spine in
`reference/current-verification-spine.md`. In short:

1. methodology alignment;
2. exact BZ/profile enforcement;
3. local C++ CTest;
4. CUDA CTest on a 4090/CUDA host;
5. zero-overflow, full-ingest run contract;
6. post-flight tile-sample audit, normally `512` samples;
7. SPANNING coordinate certificate when the verdict is `SPANNING`;
8. `stats_v2` telemetry for sweeps and anomaly analysis.

No MOAT replay, K34 shell probe, golden JSON, snapshot SHA, bounded UF, or
CPU/CUDA diff probe is an official claim-acceptance gate by itself. Those tools
are still useful for development, regression, and fault localization.

## Experiment Types

Use the smallest experiment that answers the question.

| Type | Purpose | Required evidence |
|---|---|---|
| Logic audit | Check code against methodology | File/line evidence plus stated assumption. |
| C++ implementation change | Change local reference behavior | C++ CTest; methodology check if semantics moved. |
| CUDA TileOp/kernel change | Change CUDA production | CUDA CTest on real CUDA hardware; targeted diff only if needed to localize. |
| Host/campaign orchestration | Change CLI, batching, streams, profiles, samples, certs | CUDA CTest plus post-flight run contract on a representative row. |
| Accepted/profile row | Produce campaign evidence | BZ clean, zero overflow, full-ingest when needed, 512-sample audit, SPANNING cert if applicable, `stats_v2`. |
| Performance experiment | Measure speed/profile bottlenecks | Correctness gate first, then isolated benchmark with profile JSON. |
| Golden refresh | Update regression fixtures | Explicit reason; never to bless a stronger-gate failure. |

## Run Contract

Every campaign report should state:

- branch and commit;
- machine/GPU/driver/CUDA version;
- build flags and `K_SQ`;
- exact command line;
- `R_inner`, `R_outer`, width, region, chunk size;
- early-exit enabled/taken;
- produced and ingested tile counts;
- overflow counters;
- BZ status;
- profile path and artifact paths;
- post-flight status and proof status;
- timing fields from the profile.

`MOAT` requires full ingest. `SPANNING` may early-exit only after a sound
column-complete witness is latched, but accepted proof rows should still carry a
coordinate certificate.

## Telemetry Levels

Telemetry levels are `none`, `profile`, `audit`, and `full`.

- Broad exploratory sweeps may use `profile` when samples are too expensive.
- Accepted/profile rows should use `audit` or `full`.
- The default audit sample budget is `512`.
- Larger sample counts are stress/checker benchmarks and must be named as such.

`stats_v2` is the first-class telemetry surface. Current fields include
timings, CUDA stage timings, tile counts, BZ/overflow state, geo tile and port
populations, group/port pressure distributions, high-pressure tiles, sample
paths, and live-frontier component census.

Known nullable fields are acceptable when documented: current profiles may leave
candidate-count and Gaussian-prime-count distributions null because the emitted
TileOp stream does not expose exact per-tile candidate/prime counts.

## Report Shape

Keep experiment reports compact:

```text
Question:
Commit/branch:
Machine/GPU:
Build:
Command:
Gate:
Result:
Evidence:
Decision:
Next:
```

For performance work, include before/after total wall time, CUDA K1-K5 time,
compositor time, snapshot time if any, produced/ingested tiles, and throughput.

## Development Tools

Use these when they answer a concrete debugging question:

- `cuda_vs_cpu_diff --m4/--k5` for first divergent CUDA/CPU surface.
- Snapshot SHA gates for host/snapshot parity regressions.
- CUDA goldens for cheap drift detection.
- K34 regression scripts for cross-K implementation drift.
- `exact_global_uf` for bounded small/medium oracle checks.
- Compositor replay for emitted-TileOp debugging only.

Do not cite any of these as claim-proof acceptance unless the compact spine also
passes.
