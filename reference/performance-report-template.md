# Performance Report Template

Use this template for optimization branches.

## Context

- Branch:
- Commit:
- Baseline commit:
- Hardware:
- CUDA driver / compiler:
- Build type:
- CMake CUDA arch:

## Commands

```bash
# build command

# benchmark command
```

## Correctness Gates

| Gate | Status | Evidence |
|---|---|---|
| CPU CTest |  |  |
| CUDA CTest |  |  |
| CPU/CUDA diff probe |  |  |
| Snapshot SHA smoke |  |  |
| Tsuchimura gate |  |  |

## Timing

| Metric | Baseline | Candidate | Delta |
|---|---:|---:|---:|
| total seconds |  |  |  |
| grid seconds |  |  |  |
| CUDA K1-K5 seconds |  |  |  |
| compositor seconds |  |  |  |
| produced tiles |  |  |  |
| ingested tiles |  |  |  |
| full pipeline tiles/s |  |  |  |
| CUDA TileOps/s |  |  |  |
| compositor tiles/s |  |  |  |

## Safety

- Overflow counters:
- Verdict:
- Snapshot mode affected:
- Byte layout affected:
- Methodology/canon affected:

## Decision

- Accept / reject:
- Reason:
- Follow-up:
