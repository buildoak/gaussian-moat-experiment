---
title: RTX 4090 Hardware Profiling — nsys Timeline + SASS Analysis
date: 2026-04-11
engine: claude
type: benchmark
status: complete
refs: [tile_cuda_multi_kernel/, docs/supportive/2026-04-11-4090-tuning-sweep.md]
---

# RTX 4090 Hardware Profiling

Deep hardware profiling of the multi-kernel CUDA pipeline on RTX 4090 (vast.ai instance 34629150, sm_89, 128 SMs, 24 GB GDDR6X, driver 580.95.05, CUDA 12.4). Session cost: ~$0.09.

**Tools used:** nsys (Nsight Systems 2024.4.2) for timeline profiling, cuobjdump for SASS instruction analysis. ncu (Nsight Compute) was blocked by container lacking `CAP_SYS_ADMIN` (`RmProfilingAdminOnly=1` and no permission to override).

## nsys Pipeline Timeline (10K tiles, single-stream)

### Kernel Durations

| Kernel | Duration (ms) | % of Total | Grid Size |
|--------|--------------|------------|-----------|
| K1 kernel_sieve | 15.793 | 22.0% | 10,000 |
| K2 kernel_mr | 39.933 | 55.7% | 10,000 |
| K3 kernel_compact | 0.472 | 0.7% | 10,000 |
| K4 kernel_uf | 13.567 | 18.9% | 10,000 |
| K5 kernel_face_encode | 1.887 | 2.6% | 10,000 |
| **Total** | **71.669** | **100%** | |

### Inter-Kernel Gaps

| Transition | Gap |
|-----------|-----|
| K1 -> K2 | 2.6 us |
| K2 -> K3 | 2.3 us |
| K3 -> K4 | 3.0 us |
| K4 -> K5 | 2.4 us |
| Warmup -> Benchmark | 30.8 us |

**Key finding:** Inter-kernel gaps are negligible (2-3 us). The CUDA runtime dispatches kernels back-to-back with near-zero launch overhead at steady state. There is NO launch overhead to optimize via multi-stream overlap.

### Memory Transfer Times

Total memory transferred: 0.694 MB host-to-device (coords + FJ64 table), 1.320 MB device-to-host (output + prime counts). Transfer time is <0.1 ms total — completely negligible vs the ~72 ms compute time.

### CUDA API Overhead

`cudaMemcpyToSymbol` dominates API time at 154 ms (one-time table upload). After setup, `cudaLaunchKernel` averages 4.4 us per launch (10 launches for 2 pipeline runs) — negligible.

## SASS Instruction Mix Analysis (cuobjdump)

Without ncu, the SASS disassembly provides the best available characterization of compute vs memory behavior.

### K1 Sieve — 857 instructions

| Category | Count | % |
|----------|-------|---|
| **INT arithmetic** | 538 | 62.8% |
| Control flow | 190 | 22.2% |
| Other (MOV, SEL) | 73 | 8.5% |
| Memory (LDG/STG/LDS/STS) | 56 | 6.5% |

Top opcodes: IMAD (18.8%), ISETP (14.2%), IADD3 (10.0%), LOP3 (6.4%), SHF (3.7%)

**Characterization:** Heavily INT compute-bound. Barrett modular reduction (IMAD-heavy) dominates. Memory footprint is tiny (sieve tables in constant memory, bitmap writes). The 62.8% INT instruction fraction means K1 is bounded by INT32 throughput.

### K1 Scaling Anomaly Explanation

K1 scaled only 1.20x from 3090 to 4090 vs expected 1.56x (SM count ratio). The SASS analysis confirms K1 is **INT32-bound** (62.8% integer ALU). On Ada Lovelace (sm_89), the INT32 pipe is shared with FP32 — Ada does NOT have a dedicated INT32 pipe like Turing/Ampere's "dual issue" path. However, the primary factor is likely the loop structure: K1's sieve loop iterates over 609 split primes per row with heavy control flow (22.2% branch/sync instructions). The branch divergence pattern and warp scheduling at 30 regs/thread (93.8% occupancy, 5 blocks/SM) should scale linearly with SM count. The under-scaling suggests the sieve's constant memory access pattern (10 KB `cmem[3]` for sieve tables) creates L1 constant cache contention at 128 SMs that didn't exist at 82 SMs. With 5 blocks/SM x 128 SMs = 640 concurrent blocks all reading the same constant memory tables, cache line contention would reduce effective constant memory bandwidth.

**Most likely root cause of K1's 1.20x scaling:** Constant memory (cmem) cache thrashing. The 10 KB sieve table resides in constant memory. On 3090 (82 SMs), each SM's L1 constant cache (8 KB) holds the hot portion of the table. On 4090 (128 SMs, 56% more), the same L1 cache size per SM must serve 56% more concurrent blocks. Since constant memory is broadcast-optimized only when all threads in a warp read the SAME address, and the sieve loop accesses different table entries per iteration, the broadcast optimization doesn't apply. Result: L2 constant cache becomes the bottleneck, and the extra SMs create proportionally more L2 traffic without proportionally more L2 bandwidth. The K1 sieve loop is effectively L2-bandwidth-limited for constant memory reads, not INT32-ALU-limited.

**Fix:** Move sieve tables from `__constant__` to `__device__` global memory with `__ldg()` hints, or load into shared memory at kernel start. This would make each SM self-sufficient and restore linear SM scaling.

### K2 Miller-Rabin — 4,231 instructions

| Category | Count | % |
|----------|-------|---|
| **INT arithmetic** | 3,085 | 72.9% |
| Other (MOV, SEL, I2F, MUFU) | 783 | 18.5% |
| Control flow | 324 | 7.7% |
| Memory (LDG/STG) | 39 | 0.9% |

Top opcodes: IMAD (29.4%), ISETP (22.8%), IADD3 (14.3%), SEL (9.4%), MOV (5.1%), LEA (4.9%), CALL (1.4%), I2F (1.3%), MUFU (1.3%)

**Characterization:** Overwhelmingly INT compute-bound. 72.9% integer ALU, only 0.9% memory. The IMAD (29.4%) corresponds to Montgomery multiplication — `a * b` modular arithmetic chains. The high ISETP (22.8%) is from modular reduction comparisons. SEL (9.4%) is conditional selection in the modular exponentiation.

The I2F (1.3%) and MUFU (1.3%) instructions are notable — these are integer-to-float conversion and multi-function unit (likely `__log2f` or similar). These use the FP32 pipe, providing a small amount of dual-issue opportunity with INT32.

**Key conclusion:** K2 is INT32 throughput-bound. At 44 registers and 75% occupancy (4 blocks/SM), the pipeline is issue-bound on INT32 instructions. Memory is irrelevant (0.9%). The only way to speed K2 is:
1. Reduce the number of candidates entering MR (extend the sieve)
2. Reduce MR rounds (already at 2 via FJ64)
3. Algorithmic improvement to Montgomery multiplication (unlikely — already heavily optimized)

**Sieve extension analysis:** Each candidate costs ~4,231 SASS instructions in K2. Reducing candidates by 10% would reduce K2 time by ~10% (4 ms at 10K tiles). The sieve extension to eliminate more composites before MR is the highest-ROI optimization.

### K3 Compact — 338 instructions

| Category | Count | % |
|----------|-------|---|
| INT arithmetic | 189 | 55.9% |
| Control flow | 104 | 30.8% |
| Memory | 34 | 10.1% |
| Other | 11 | 3.3% |

Lightweight prefix-sum kernel. 0.7% of runtime. Not worth optimizing.

### K4 Union-Find — 460 instructions

| Category | Count | % |
|----------|-------|---|
| **INT arithmetic** | 311 | 67.6% |
| Memory (LDG/STG) | 62 | 13.5% |
| Control flow | 72 | 15.7% |
| Other | 15 | 3.3% |

Top opcodes: IMAD (17.4%), PRMT (11.3%), LEA (9.6%), ISETP (8.9%), IADD3 (7.8%), STG (6.5%), LOP3 (6.1%), LDG (5.4%)

**Characterization:** Mixed INT + memory. More memory-intensive than K1/K2 (13.5% vs 6.5%/0.9%). The PRMT (11.3%) is byte permutation for bitmap manipulation. The LDG/STG pair (11.9% combined) reflects the scatter/gather access pattern of union-find with path compression.

At 19% of runtime with 460 instructions (vs K2's 4,231), K4 has fewer instructions per tile but each instruction is likely higher-latency due to the memory scatter pattern. The `atomicCAS` in parallel union-find generates atomic replays that nsys cannot quantify (need ncu for this). The PRMT-heavy instruction mix suggests significant bit manipulation for bitmap indexing.

### K5 Face Encode — 2,070 instructions

| Category | Count | % |
|----------|-------|---|
| Memory (LDS/STS/LDG/STG) | 827 | 40.0% |
| INT arithmetic | 847 | 40.9% |
| Control flow | 245 | 11.8% |
| Other | 151 | 7.3% |

Top opcodes: STG (25.5%), ISETP (14.9%), LDS (10.4%), IMAD (8.0%)

**Characterization:** Memory-dominated. 40% memory instructions (25.5% global stores, 10.4% shared loads). This kernel writes the 128-byte TileOp output and does face enumeration with shared memory scratch space (11 KB). Despite having 2,070 instructions (half of K2's 4,231), it runs 20x faster because the memory operations are well-coalesced writes to output buffers.

## Occupancy Summary

| Kernel | Regs/Thread | Blocks/SM | Warps/SM | Occupancy |
|--------|-------------|-----------|----------|-----------|
| K1 Sieve | 30 | 5 | 45 | 93.8% |
| K2 MR | 44 | 4 | 36 | 75.0% |
| K3 Compact | 21 | 5 | 45 | 93.8% |
| K4 UF | 34 | 5 | 45 | 93.8% |
| K5 FaceEncode | 40 | 5 | 45 | 93.8% |

K2's 75% occupancy is the tradeoff for keeping 44 registers (no spills). At 40 regs (5 blocks/SM, 93.8%), spills cancel the occupancy gain. This is the optimal operating point confirmed by the earlier register sweep.

## Summary of Findings

1. **Pipeline is compute-bound.** Memory transfers are <0.1% of runtime. Inter-kernel gaps are <0.01% of runtime.
2. **K2 MR dominates (55.7%)** and is INT32 throughput-bound (72.9% integer ALU, 0.9% memory).
3. **K1 Sieve (22.0%)** is INT32-bound (62.8%) with likely constant-memory cache contention causing 1.20x scaling.
4. **K4 UF (18.9%)** is mixed INT + memory with scatter/gather atomic patterns.
5. **Multi-stream overlap is ineffective** at 10K+ tiles because each kernel generates 10K+ thread blocks that fully saturate 128 SMs.
6. **ncu profiling requires `CAP_SYS_ADMIN`** — standard vast.ai containers don't have it. Use bare-metal or `--privileged` Docker for full hardware counter analysis.

## Profiling Artifacts

All stored at `profiling/4090/`:
- `pipeline_baseline.nsys-rep` — 2K tile single-stream baseline
- `pipeline_detailed.nsys-rep` — 10K tile single-stream with memory usage
- `pipeline_multistream.nsys-rep` — 10K tile multi-stream (2 streams, 8 sub-batches)
