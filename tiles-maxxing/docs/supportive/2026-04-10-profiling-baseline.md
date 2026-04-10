---
title: Per-Phase Profiling Baseline (1,131 tiles/sec kernel)
date: 2026-04-10
engine: coordinator
type: benchmark
status: complete
refs:
  - tile-cuda/src/tile_kernel.cu
  - docs/tile_internals_cuda.md
---

# Per-Phase Profiling Baseline

## Hardware

- Jetson Orin Nano, sm_87, 8 SMs, CUDA 12.6, driver 540.4.0
- Clock: 1,020 MHz (core), 1,020 MHz (memory)
- Shared memory: 167,936 bytes/SM, 49,152 bytes/block default, 166,912 optin

## Kernel Configuration

- 288 threads/block, 4 blocks/SM, 75% occupancy (1,152/1,536 threads)
- 46 registers/thread, 56 bytes stack, 36,640 bytes dynamic shared, 1,040 bytes static shared
- MAX_PRIMES_GPU=2560, MAX_CANDIDATES_GPU=6144, FACES_PER_PASS=2

## Benchmark Conditions

- 1,000 tiles at origin (608000000, 608000000)
- Throughput: 1,095 tiles/sec (profiling build, ~3% overhead vs 1,131 production)
- Instrumented with clock64() on thread 0 between phases

## Per-Phase Cycle Breakdown

| Phase | Description | Mean cycles | Median cycles | Min | Max | % of total |
|-------|-------------|------------|---------------|-----|-----|-----------|
| 1a | Sieve count pass | 1,672,470 | 1,675,337 | 833,425 | 2,861,025 | **5.80%** |
| 1b | Prefix scan + scatter (2nd sieve_row) | 3,478,515 | 3,536,202 | 908,337 | 11,752,538 | **12.06%** |
| 1c | Miller-Rabin (12-witness, Montgomery) | 7,453,446 | 7,362,255 | 6,700,645 | 11,391,071 | **25.85%** |
| 2 | Compact (row prefix → dense primes) | 97,692 | 7,876 | 6,210 | 1,551,764 | 0.34% |
| 3 | Union-Find component detection | 3,390,637 | 3,397,467 | 2,442,763 | 3,997,921 | **11.76%** |
| 4+5 | Face extraction + port encoding | 12,744,683 | 12,841,785 | 8,703,025 | 13,794,133 | **44.19%** |
| **Total** | | **28,837,442** | **28,791,675** | 23,089,188 | 38,745,453 | 100% |

## Theoretical vs Measured

| Phase | Theoretical % | Measured % | Delta |
|-------|--------------|------------|-------|
| 1a Sieve | 34% | 5.80% | -28.2 pp |
| 1b Scatter | 2% | 12.06% | +10.1 pp |
| 1c MR | 45% | 25.85% | -19.2 pp |
| 2 Compact | 1% | 0.34% | -0.7 pp |
| 3 UF | 17% | 11.76% | -5.2 pp |
| 4+5 Face+Enc | 1% | 44.19% | **+43.2 pp** |

The theoretical cycle budget was dramatically wrong about Phase 4+5 (estimated 1%, measured 44%) and Phase 1a (estimated 34%, measured 6%).

## Key Observations

1. **Phase 4+5 is the dominant bottleneck (44%).** Face extraction + port encoding consumes nearly half of all cycles. The 2-face-per-pass rewrite addressed shared memory sizing but did not reduce the computational cost. The serial port scan (lane-0 only) and TileOp encoding are the likely culprits.

2. **Phase 1c MR is second (26%).** Montgomery multiplication is already in place. The cost is the modular multiply chain (12 witnesses x modexp), not software division. Barrett would not help here.

3. **Phase 1b scatter is third (12%).** This includes the second sieve_row() call. A single-pass sieve would eliminate this entirely.

4. **Phase 3 UF is fourth (12%).** Union-Find with CAS atomics. Moderate but worth investigating for thread utilization.

5. **Phase 1a sieve count is small (6%).** Barrett reduction would speed this up but it's only 6% of total — not the high-leverage target we assumed.

6. **Phase 1b has extreme variance** (908K to 11.7M, 13x range). Tiles with more candidates cause much longer scatter phases. The single-pass atomic reservation would eliminate this variance.

7. **Phase 2 has extreme variance** (6K to 1.5M, 250x range). The median is 7,876 but the max is 1.5M — occasional tiles hit slow paths in the compact phase.

## SASS Instruction Mix (top 20)

Total: 10,133 SASS instructions across all functions.

| Count | Instruction | Notes |
|-------|------------|-------|
| 896 | IADD3 | Integer add (3-input) |
| 587 | IMAD.MOV.U32 | Integer multiply-add (move variant) |
| 535 | SEL | Conditional select |
| 525 | STG.E.U8 | Global store byte (TileOp writes) |
| 443 | IMAD | Integer multiply-add |
| 393 | ISETP.NE.AND | Integer set-predicate |
| 379 | MOV | Move |
| 363 | STS | Shared store |
| 348 | ISETP.NE.U32.AND | Unsigned integer compare |
| 300 | ISETP.GT.U32.AND | Unsigned integer greater-than |
| 265 | LOP3.LUT | 3-input logic (LUT-based) |
| 262 | BSYNC / BSSY | Barrier sync pairs |
| 245 | ISETP.GE.U32.AND | Unsigned greater-or-equal |
| 241 | ISETP.NE.AND.EX | Extended compare |
| 226 | IMAD.X | Extended multiply-add |
| 218 | PRMT | Byte permute |
| 216 | IMAD.WIDE.U32 | Wide multiply-add (32→64 bit) |
| 213 | BRA | Branch |
| 204 | IMAD.HI.U32 | High-word multiply-add |
| 91 | MUFU.RCP | Floating-point reciprocal (software division path) |

Notable: 83 CALL.ABS.NOINC to `__cuda_sm20_rem_u64`/`__cuda_sm20_rem_s64` library functions. 525 global byte stores (TileOp encoding). 363 shared stores.

## Resource Usage (cuobjdump)

```
Function _Z20process_tiles_kernelPK9TileCoordP6TileOpPji:
  REG:46 STACK:56 SHARED:1040 LOCAL:0 CONSTANT[2]:336 CONSTANT[0]:380
Function __cuda_sm20_rem_u64:
  REG:24 STACK:0
Function __cuda_sm20_rem_s64:
  REG:28 STACK:0
```

## Throughput Context

- Mean cycles per tile: 28.8M
- At 1,020 MHz: 28.3 ms wall time per tile
- Effective parallelism: 4 blocks/SM x 8 SMs = 32 tiles in flight
- Expected throughput: 1000 / (28.3 / 32) = 1,130 tiles/sec (matches measured 1,095-1,131)
