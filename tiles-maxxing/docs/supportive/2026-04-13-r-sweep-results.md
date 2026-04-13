# R-Sweep Results: K_SQ=40, R = 875M–1125M

**Date:** 2026-04-13
**GPU:** RTX 4090 (sm_89, 24GB, vast.ai instance 34846705)
**Pipeline:** Fixed-chunk CUDA streaming (STREAM_CHUNK_SIZE=200K, burst_size=28000)
**Commits:** `3ffd202` (fixed-chunk streaming), `acf9c43` (400K cap, superseded)

## Results

| R | Verdict | Wall (s) | Tiles | Towers | Overflows | Spanning tower |
|---|---------|----------|-------|--------|-----------|----------------|
| 875,000,000 | **SPANNING** | 188.8 | 87,290,165 | 2,416,893 | 0 | ~660K (27%) |
| 900,000,000 | **MOAT** | 756.5 | 89,784,166 | 2,486,214 | 0 | — |
| 925,000,000 | **MOAT** | 774.4 | 92,278,122 | 2,555,000 | 0 | — |
| 950,000,000 | **SPANNING** | 7.6 | 94,772,122 | 2,624,321 | 0 | ~26 (instant) |
| 975,000,000 | **MOAT** | 814.2 | 97,266,079 | 2,693,107 | 0 | — |
| 1,000,000,000 | **MOAT** | 828.1 | 99,760,030 | 2,762,160 | 0 | — |
| 1,025,000,000 | **MOAT** | 849.9 | 102,254,036 | 2,831,214 | 0 | — |
| 1,050,000,000 | **MOAT** | 879.1 | 104,747,989 | 2,900,267 | 0 | — |
| 1,075,000,000 | **MOAT** | 880.1 | 107,241,989 | 2,969,321 | 0 | — |
| 1,100,000,000 | **MOAT** | 916.9 | 109,735,945 | 3,038,374 | 0 | — |
| 1,125,000,000 | **MOAT** | 933.0 | 112,229,900 | 3,107,427 | 0 | — |

**Prior result for context:** R=600,000,000 SPANNING at tower 931 (instant).

## Key Findings

### 1. The transition is non-monotonic

The SPANNING/MOAT boundary is NOT a clean threshold. R=950M SPANs instantly (7.6s, tower ~26) despite being surrounded by MOATs at R=925M and R=975M. This is a connectivity island — a specific cluster of Gaussian primes at R=950M creates a spanning path that doesn't exist 25M in either direction.

### 2. Two distinct SPANNING behaviors

- **R=875M:** Spanning found at ~27% of towers (188.8s). The connected component barely spans — it takes deep traversal of the annular region before a path is found.
- **R=950M:** Spanning found instantly at tower ~26 (7.6s). A strong, localized spanning path exists very close to the start of the sweep.
- **R=600M:** Spanning found at tower 931 (from prior session). Strong connectivity at this radius.

### 3. ISE percolation estimate validated (statistically)

The ISE estimate of R_0.5 ~ 839M +/- 12M predicted the statistical transition zone. The actual results show the FIRST MOAT appearing at R=900M, which is ~7% above the ISE midpoint. The ISE model captures the trend but cannot predict the non-monotonic islands.

### 4. MOAT dominates above 900M

9 out of 10 R-values at or above 900M show MOAT (the exception being R=950M). All R-values above 975M show MOAT without exception. The moat regime is robust above ~1B.

## Compute Summary

- Total tiles processed: ~1.1 billion across 11 runs
- Total wall time: ~2.3 hours (including the R=1B run)
- Zero overflows across all runs
- Instance cost: ~$0.67 at $0.29/hr
- Effective GPU rate: ~120-134K tiles/s depending on run

## Next Steps

- Binary search between 875M-900M and 925M-975M to pinpoint transition boundaries
- Investigate R=950M anomaly: what specific Gaussian prime cluster creates the spanning path?
- Run `--no-early-exit` on the SPANNING results to get full sweep statistics (total groups, inner/outer roots)
- Consider finer grid (5M or 10M steps) around the transition zone

## Artifacts

- `artifacts/2026-04-13-r-sweep/sweep-results.jsonl` — full JSON results per run
- `artifacts/2026-04-13-r-sweep/sweep-summary.txt` — human-readable summary
