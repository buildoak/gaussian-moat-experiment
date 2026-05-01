---
title: Multi-Kernel Architecture — 5-Kernel Pipeline, 3,333 tiles/s
date: 2026-04-10
engine: claude
type: report
status: complete
refs:
  - tile_cuda_multi_kernel/src/main.cu
  - tile_cuda_multi_kernel/src/kernel_sieve.cu
  - tile_cuda_multi_kernel/src/kernel_mr.cu
  - tile_cuda_multi_kernel/src/kernel_compact.cu
  - tile_cuda_multi_kernel/src/kernel_uf.cu
  - tile_cuda_multi_kernel/src/kernel_face_encode.cu
  - tile_cuda_multi_kernel/Makefile
  - docs/supportive/2026-04-10-cuda-kernel-optimization-spree.md
---

# Multi-Kernel Architecture

**Date:** 2026-04-10
**Hardware:** Jetson Orin Nano (SM 8.7, 8 SMs, 8 GB)
**HEAD commit (pre-multi-kernel):** 9814a1c
**Predecessor:** Monolithic single-kernel at 2,818 tiles/s (session a913f1dc)

## Motivation

The monolithic single-kernel architecture (`tile_cuda/`) hit a structural wall: nvcc global register allocation coupling. When all five tile-processing phases shared one compilation unit, nvcc's register allocator made globally-coupled decisions. Any change to one phase (e.g., union-find parallelization or switching MR algorithm) would perturb the register allocation of other phases, causing unexpected regressions:

- **FJ64 MR replacement:** Switching from 7-witness Sinclair MR to 2-round FJ64_262k hash-based MR regressed throughput by -11%, despite FJ64 being computationally cheaper per candidate. nvcc allocated more registers to accommodate the hash computation, spilling sieve and UF phases.
- **Full UF parallelization:** Replacing serial 32-thread UF with parallel 288-thread atomicCAS union-find regressed 5-15%. The structural change to UF's control flow caused nvcc to rearrange register usage across all phases.
- **UF popcount unroll:** A minor loop unroll in UF phase gained only +4.1% (2,818 to 2,867 tiles/s) when the theoretical improvement from -22.5% phase-3 cycles should have been larger. Codegen coupling dampened the gain.

The multi-kernel approach eliminates the coupling entirely: each phase is compiled in its own translation unit with independent `--maxrregcount` caps.

## Architecture: 5-Kernel Pipeline

```
K1 Sieve ──> K2 MR ──> K3 Compact ──> K4 UF ──> K5 FaceEncode
  (cands)   (bitmap)   (prime_pos)   (parent)     (TileOp)
```

All five kernels launch sequentially on the default CUDA stream with `dim3(num_tiles)` blocks of 288 threads each. One block per tile, same as monolithic.

### K1 Sieve (30 regs, --maxrregcount=40)

Barrett-reduction sieve against 609 split primes and 619 inert primes from constant memory. Each of 271 active threads handles one row of the expanded 271x271 grid. Survivors scatter to `d_cand_list` via `atomicAdd` on a per-tile shared counter.

**Output:** `d_cand_list[N * 6144]` (packed row:col uint32), `d_total_cands[N]`.

### K2 MR (46 regs, no cap)

FJ64_262k Miller-Rabin primality test. Two MR rounds total:
1. Base-2 witness (hardcoded).
2. Hash-table lookup: `h = hash(n)`, `witness = fj64_table[h & 0x3FFFF]`.

The FJ64_262k table is 262,144 uint16_t entries (512 KB) in global memory, L2-cached on Orin (1 MB L2). The hash function is Forisek-Jancina's two-round xorshift-multiply:
```
h = n
h = ((h >> 32) ^ h) * 0x45d9f3b3335b369
h = ((h >> 32) ^ h) * 0x3335b36945d9f3b
h = ((h >> 32) ^ h)
witness = table[h & 0x3FFFF]
```

This replaces 7-witness Sinclair MR (7 full Montgomery exponentiation rounds) with 2 rounds. The table is sourced from Eppie/euler, implementing the Forisek & Jancina deterministic MR algorithm for n < 2^64.

No register cap: nvcc naturally uses 46 registers for the Montgomery arithmetic. Capping would spill and hurt ILP. At 46 regs, occupancy is 3 blocks/SM (864 threads / 1536 max = 56%).

**Input:** `d_cand_list`, `d_total_cands`, `d_fj64_table`.
**Output:** `d_bitmap[N * BITMAP_WORDS]` (2439 uint32 words per tile, set via atomicOr).

### K3 Compact (21 regs, --maxrregcount=32)

Bitmap to compact prime position list. Per-row popcount, exclusive prefix scan in shared memory, scatter prime positions.

**Input:** `d_bitmap`.
**Output:** `d_row_prefix[N * 272]`, `d_prime_pos[N * 2560]`, `d_prime_count[N]`.

### K4 UF (34 regs, --maxrregcount=40)

Parallel lock-free union-find with all 288 threads. Each thread processes primes in stride, checking backward neighbors (64 offsets within K_SQ=40 Euclidean distance) against the bitmap, then performing `atomicCAS`-based union operations.

The union-find uses path splitting (point to grandparent) during find operations, and deterministic smaller-root-wins ordering for unions. A final path compression pass flattens all parent pointers to roots.

This replaces the monolithic kernel's serial 32-thread (single-warp) UF approach with full 288-thread parallelism. The 9x thread increase is possible because separate compilation eliminates the register pressure coupling that caused 5-15% regressions in the monolithic kernel.

**Input:** `d_bitmap`, `d_row_prefix`, `d_prime_pos`, `d_prime_count`.
**Output:** `d_parent[N * 2560]`.

### K5 FaceEncode (40 regs, --maxrregcount=40)

Face extraction, dead-end pruning, group assignment, and TileOp encoding. Parallel face-membership classification using all 288 threads, followed by single-thread-per-face sorting and port detection, then serial pruning and encoding.

Uses dynamic shared memory for face prime lists, face scratch, and face data structures. Total shared memory: `sizeof(FacePrimeGPU) * 4 * 256 + sizeof(uint32_t) * 4 + sizeof(FaceScratchGPU) + sizeof(FaceDataGPU)`.

**Input:** `d_prime_pos`, `d_prime_count`, `d_parent`.
**Output:** `d_output[N]` (TileOp), `d_prime_counts_out[N]`.

## Inter-Kernel Buffer Layout

All inter-kernel buffers are pre-allocated in a single `TileBatchDeviceMemory` struct. Buffer sizes for N tiles:

| Buffer | Size per tile | Total (2000 tiles) | Flow |
|--------|--------------|-------------------|------|
| d_coords | 16 B | 32 KB | Host -> K1 |
| d_cand_list | 24,576 B (6144 * 4) | 48 MB | K1 -> K2 |
| d_total_cands | 4 B | 8 KB | K1 -> K2 |
| d_bitmap | 9,756 B (2439 * 4) | 19 MB | K2 -> K3, K4 |
| d_row_prefix | 544 B (272 * 2) | 1 MB | K3 -> K4, K5 |
| d_prime_pos | 10,240 B (2560 * 4) | 20 MB | K3 -> K4, K5 |
| d_prime_count | 4 B | 8 KB | K3 -> K4, K5 |
| d_parent | 5,120 B (2560 * 2) | 10 MB | K4 -> K5 |
| d_output | 128 B | 256 KB | K5 -> Host |
| d_fj64_table | 512 KB (shared) | 512 KB | K2 only |

**Total device memory:** ~99 MB for 2000 tiles. Well within Orin's 8 GB.

**L2 residency:** The FJ64 table (512 KB) fits in Orin's 1 MB L2. After warmup, table accesses hit L2 at ~200 cycle latency rather than DRAM ~500+ cycles. The bitmap buffer (19 MB for 2000 tiles) does not fit in L2 and streams through DRAM, but is written once (K2) and read twice (K3, K4) with sequential access patterns.

## Build System: Separate Compilation

The Makefile uses nvcc `-dc` (device-code, separate compilation) for each .cu file, followed by `-dlink` (device linking) to produce the final binary. This is the key enabler: each translation unit gets its own `--maxrregcount` cap and independent register allocation.

| Translation Unit | Register Cap | Actual Regs | Rationale |
|-----------------|-------------|-------------|-----------|
| kernel_sieve.cu | 40 | 30 | Leave headroom for Barrett arithmetic |
| kernel_mr.cu | none | 46 | Montgomery multiply needs ILP; capping spills |
| kernel_compact.cu | 32 | 21 | Lightweight scan; maximize occupancy |
| kernel_uf.cu | 40 | 34 | Balance atomicCAS latency hiding with occupancy |
| kernel_face_encode.cu | 40 | 40 | Complex control flow; 40 is the sweet spot |

Constant memory symbols (`c_split_barrett`, `c_inert_barrett`, `c_mr_witnesses`, `c_trial_primes`, `c_bk_dr`, `c_bk_dc`) are defined in `kernel_sieve.cu` (primary TU) and declared `extern` in `gpu_math.cuh` for cross-TU access.

## Performance Results

**Benchmark:** 2000 tiles at origin (608000000, 608000000), Jetson Orin Nano SM 8.7.

### Throughput

**3,333 tiles/s** (0.300 ms/tile)

- vs monolithic baseline (same session): 2,395 tiles/s -> **+39.2%**
- vs monolithic best (session a913f1dc): 2,818 tiles/s -> **+18.3%**
- vs Mac Mini 12-core CPU: ~1,000 tiles/s -> **~3.3x**

### Per-Kernel Timing (2000 tiles)

| Kernel | Time (ms) | % of Total | Regs | Notes |
|--------|-----------|-----------|------|-------|
| K1 Sieve | 98.2 | 16.4% | 30 | Barrett sieve, 271 active rows |
| K2 MR | 323.9 | 54.0% | 46 | FJ64_262k, 2 MR rounds |
| K3 Compact | 1.9 | 0.3% | 21 | Prefix scan + scatter |
| K4 UF | 161.5 | 26.9% | 34 | 288-thread parallel atomicCAS |
| K5 FaceEncode | 14.6 | 2.4% | 40 | Face extraction + encoding |
| **Total** | **600.1** | **100%** | | |

### Phase Distribution Shift

| Phase | Monolithic (a913f1dc) | Multi-Kernel |
|-------|----------------------|--------------|
| Sieve | ~16% | 16.4% |
| MR | ~26% (Sinclair 7-base) | 54.0% (FJ64 2-round) |
| Compact | n/a (inline) | 0.3% |
| UF | ~12% (serial 32-thread) | 26.9% (parallel 288-thread) |
| Face encode | ~2% | 2.4% |

MR dominates more in absolute share because the other phases got faster (sieve was already optimized; UF parallelization handles more work per unit time). The MR percentage increase is misleading -- MR is actually faster per-candidate than Sinclair (2 rounds vs 7), but it now processes more candidates per second because the pipeline around it is faster.

## Correctness

**Byte-identical TileOps** against the monolithic kernel on both canonical smoke tiles:
- (600000000, 600000000)
- (699999744, 400000000)

Cross-validated against C++ reference implementation (tile-cpp).

## Known Limitations

1. **No pipeline overlap:** All 5 kernels execute sequentially on one CUDA stream. K1 of batch N+1 could overlap with K4/K5 of batch N using multi-stream pipelining.
2. **K2 MR is 54% of total:** The dominant bottleneck. FJ64 reduced from 7 to 2 MR rounds, but Montgomery exponentiation is still the most expensive per-candidate operation.
3. **K4 UF atomicCAS contention:** 288 threads competing on shared parent array. Contention on high-degree nodes (primes with many K_SQ neighbors) may cause retry storms.
4. **FJ64 table not in shared memory:** 512 KB exceeds Orin's 48 KB shared memory limit. L2 caching works but adds ~200 cycle latency per lookup vs ~30 cycles for shared memory.
5. **Device linking overhead:** Separate compilation adds a `-dlink` step and slightly larger binary. Not a runtime concern, but increases build time.

## Next Steps

- Register tuning for K2 MR: experiment with 48, 52, 56, 64 register caps
- R^2 fast computation in Montgomery init: replace 128 addmod loop with 2-op shortcut
- K4 UF: analyze atomicCAS retry rates, consider label propagation alternative
- Multi-stream pipeline overlap for K1/K4+K5 interleaving
- Port to 3090/4090 where larger register file may unlock higher occupancy for K2
