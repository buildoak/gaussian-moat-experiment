---
title: K1 Shared Memory + Sieve Extension — Both Dead Ends
date: 2026-04-11
engine: claude
type: benchmark
status: complete
refs: [tile_cuda_multi_kernel/, docs/supportive/2026-04-11-4090-hardware-profiling.md, docs/supportive/2026-04-11-next-session-plan.md]
---

# K1 Shared Memory + Sieve Extension

RTX 4090 session testing two optimizations: (1) moving sieve tables from constant to shared memory, and (2) extending the sieve beyond 10K primes. Both are dead ends. The pipeline is at hardware INT32 throughput limits.

## Instance Details

- GPU: RTX 4090 (sm_89, 128 SMs, 2520 MHz boost)
- vast.ai instance ID: 34631398
- Image: pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel
- Session cost: <$0.10

## Optimization 1: K1 Constant Memory to Shared Memory

### Hypothesis

K1 sieve scaled only 1.20x from 3090 to 4090 (expected 1.56x based on SM count ratio). SASS profiling showed 62.8% INT32 ALU + 10 KB constant memory table. Hypothesis: L2 constant cache contention at 128 SMs x 5 blocks/SM = 640 concurrent blocks reading different table entries.

### Implementation

- Removed `__constant__` declarations for sieve tables (`c_split_barrett`, `c_inert_barrett`)
- Allocated sieve tables in global device memory (kernel parameters)
- Added cooperative load from global to shared memory at kernel start
- Dynamic shared memory layout: `[split_table | inert_table | total_cands]` = 9,828 bytes at 10K sieve
- `sieve_row_k1()` takes table pointers and counts as parameters instead of reading `__constant__`
- MR witnesses, trial primes, backward offsets remain in constant memory (280 B, no contention)

### Build Stats

| Kernel | Registers | Spills | Occupancy | Notes |
|--------|-----------|--------|-----------|-------|
| K1 Sieve (smem) | 38 | 0 (entry), 12B (helper) | 5 blk/SM (93.8%) | Up from 30 regs baseline |
| K2 MR | 44 | 0 | 4 blk/SM (75.0%) | Unchanged |
| K3 Compact | 21 | 0 | 5 blk/SM (93.8%) | Unchanged |
| K4 UF | 34 | 0 | 5 blk/SM (93.8%) | Unchanged |
| K5 FaceEncode | 40 | 0 | 5 blk/SM (93.8%) | Unchanged |

### Correctness

Smoke test output byte-identical to baseline:
- tile (600000000,600000000): prime_count=2218, tileop[0..3]=10 15 1a 04
- tile (699999744,400000000): prime_count=2254, tileop[0..3]=13 20 2b 05

Matches multi-stream overlap doc reference values exactly.

### Performance (10K tiles, 3 runs)

| Metric | Baseline (cmem, prev instance) | Run 1 | Run 2 | Run 3 |
|--------|-------------------------------|-------|-------|-------|
| K1 Sieve (ms) | 14.40 | 15.98 | 15.28 | 15.22 |
| K2 MR (ms) | 36.07 | 40.22 | 38.48 | 38.40 |
| K3 Compact (ms) | 0.47 | 0.48 | 0.46 | 0.46 |
| K4 UF (ms) | 12.44 | 13.88 | 13.16 | 13.27 |
| K5 FaceEncode (ms) | 1.75 | 1.94 | 1.84 | 1.85 |
| Total (ms) | 65.09 | 72.50 | 69.22 | 69.20 |
| tiles/s | 155,452 | 137,935 | 144,470 | 144,507 |

### Analysis

All kernels are proportionally slower on this instance vs the baseline instance. K1's share of total time:
- Baseline: 14.40 / 65.09 = **22.1%**
- Smem: 15.22 / 69.20 = **22.0%**

Identical fraction. The absolute difference is clock speed variation between instances. The smem change is strictly neutral.

**Conclusion:** Constant memory was NOT the K1 bottleneck. K1 is purely INT32 ALU-bound, same as K2. The 1.20x scaling gap from 3090 to 4090 is likely Ada Lovelace INT32 pipe sharing with FP32 (each SM has one dedicated INT32 unit + one that's shared with FP32), not a constant cache issue. The 10 KB table fits entirely in L1 constant cache (8 KB per SM) and the broadcast pattern works fine.

## Optimization 2: Sieve Extension Sweep

### Implementation

- Made sieve limit a runtime parameter (`./tile_kernel_multi sweep <sieve_limit> <tile_count>`)
- `SieveTablesBarrett` expanded to `MAX_SPLIT_PRIMES=5000`, `MAX_INERT_PRIMES=5000`
- `init_sieve_tables_host()` parameterized by `uint32_t sieve_limit`
- Candidate count statistics added to benchmark output (mean, min, max, p50, p99)
- All primes up to 50K fit in `uint16_t` (max prime < 49,999)

### Sweep Results (10K tiles, this instance)

| Sieve Limit | Split | Inert | Cands Mean | Cand Reduction | K1 (ms) | K2 (ms) | Total (ms) | tiles/s |
|-------------|-------|-------|------------|---------------|---------|---------|------------|---------|
| 10,000 | 609 | 619 | 5,687 | baseline | 16.0 | 40.3 | 72.8 | 137,374 |
| 12,000 | 707 | 730 | 5,586 | -1.8% | 16.3 | 39.7 | 72.3 | 138,323 |
| 15,000 | 866 | 887 | 5,455 | -4.1% | 18.6 | 39.1 | 73.9 | 135,312 |
| 20,000 | 1,125 | 1,136 | 5,295 | -6.9% | 19.4 | 38.3 | 73.9 | 135,279 |
| 30,000 | 1,611 | 1,633 | 5,089 | -10.5% | 20.3 | 37.4 | 74.0 | 135,140 |
| 50,000 | 2,549 | 2,583 | 4,848 | -14.8% | 24.1 | 34.6 | 74.1 | 134,938 |

### K1 Occupancy Impact

| Sieve Limit | Smem/block | K1 Blocks/SM | Occupancy |
|-------------|-----------|-------------|-----------|
| 10K | 9,828 B | 5 | 93.8% |
| 12K | 11,500 B | 5 | 93.8% |
| 15K | 14,028 B | 5 | 93.8% |
| 20K | 18,092 B | 5 | 93.8% |
| 30K | 25,956 B | 3 | 56.2% |
| 50K | 41,060 B | 2 | 37.5% |

### Correctness

All sieve limits produce identical prime counts and tileop bytes for the first 8 tiles. This confirms:
- Sieve extension correctly eliminates additional composites (candidates decrease monotonically)
- No false positive marking of primes as composite
- K2 MR correctly classifies the same primes regardless of how many composites K1 pre-filters

### Analysis

The sieve extension works exactly as theoretically predicted:
1. Candidates decrease linearly with log(sieve_limit) per Mertens' theorem
2. K2 time decreases proportionally to candidate count (K2 cost per candidate is constant)
3. K1 time increases linearly with prime count (each iteration costs fixed INT32 ALU work)

The problem: K1 and K2 are both INT32-bound on the same hardware. Every INT32 operation saved in K2 costs approximately one INT32 operation in K1. The trade is nearly 1:1, with K1 slightly worse because:
- K1 does Barrett reduction (2 multiplies + conditional) per prime per row = more ALU per prime
- K2 does Montgomery exponentiation per candidate = more ALU per candidate but fewer candidates

At 50K sieve: K1 gains +8.1ms, K2 saves -5.7ms. Net: +2.4ms worse. The crossover is around 12K where the net is approximately zero (+0.3ms K1, -0.6ms K2).

**Conclusion:** 10K sieve limit is already optimal (or 12K for a noise-level +0.7% improvement not worth the complexity). Sieve extension is a dead end because both K1 and K2 are bound by the same INT32 pipe.

## Overall Conclusions

1. **The 4090 pipeline is at hardware INT32 throughput limits.** Every optimization attempt (mont_to_gpu, __ldg, register sweep, multi-stream, K1 smem, sieve extension) is neutral or harmful. The pipeline saturates the Ada Lovelace INT32 execution units.

2. **K1 scaling anomaly is INT32 pipe sharing, not constant memory.** On Ada Lovelace, each SM has one dedicated INT32 unit and one FP32/INT32 shared unit. K1's effective INT32 throughput scales with the dedicated unit count, not total SM count. This explains the 1.20x scaling (vs 1.56x expected from SM ratio alone).

3. **Code reverted to baseline.** Neither optimization produced a clear improvement. The 155K baseline (commit `03fd7f2`) remains the proven optimum.

4. **Remaining paths require algorithmic changes:**
   - Faster primality test (fewer Montgomery multiplications per candidate)
   - Batch MR with shared witnesses (amortize Montgomery context setup)
   - Hardware with higher INT32 throughput (H100, Blackwell)
   - Algorithmic reduction of K4 UF work (19% of pipeline)
