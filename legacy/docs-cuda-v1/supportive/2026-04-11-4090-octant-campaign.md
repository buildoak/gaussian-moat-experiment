---
title: Full First-Octant Dump Campaign — R = 850M, RTX 4090
date: 2026-04-11
engine: claude
type: dataset
status: complete
refs: [docs/supportive/2026-04-11-300k-tile-census.md, docs/supportive/2026-04-11-4090-tuning-sweep.md, docs/supportive/2026-04-11-4090-hardware-profiling.md, vast-ai/README.md]
session: 09d60151
---

# Full First-Octant Dump — R = 850M

Captured the complete first octant of Gaussian integer tiles at radius 850,000,000 on a rented RTX 4090. 75.1M tiles, 10.4 GB, under 9 minutes of GPU time, $0.17 total cost. The dataset now lives on the Mac Mini for local compositor/seam-merging development.

**Date:** 2026-04-11
**Status:** Complete

---

## Objective

Produce a complete TileOp dump covering the entire first octant (90 degrees to 45 degrees, from the y-axis down to the y=x diagonal) at R=850M. This gives the compositor development pipeline a full-scale dataset to iterate on locally without needing further GPU rentals. Every tile in the search domain is represented -- all seam types, all angular regions, all edge cases near the diagonal.

---

## Prerequisites

This run was the final phase of a session that first validated correctness and performance at smaller scale:

1. **300K Tile Census** (`2026-04-11-300k-tile-census.md`) — 3 angular bands of 100K tiles each (85 degrees, 67.5 degrees, 45 degrees). CUDA vs C++ cross-validation: 300,000/300,000 exact byte-for-byte match. Zero poisoned tiles. Zero dead tiles in output. Statistical properties confirmed angle-invariant.

2. **4090 Tuning Sweep** (`2026-04-11-4090-tuning-sweep.md`) — systematic tuning on RTX 4090. Best config: sm_89, maxreg=44 for K2, 288 threads/block, FJ64_262k 2-round MR. Peak throughput: 155K tiles/s at batch sizes >= 10K.

3. **4090 Hardware Profiling** (`2026-04-11-4090-hardware-profiling.md`) — nsys timeline + SASS analysis. K2 Miller-Rabin dominates at 55.7% of runtime (INT32-bound). Pipeline is compute-bound; memory transfers are negligible.

4. **Allocation overhead diagnosis** — the 300K dump measured 130K tiles/s vs the 155K benchmark rate. Root cause: `run_dump()` called `cudaMalloc`/`cudaFree` inside the chunk loop, paying ~25ms per chunk. At 5 chunks for 300K tiles the overhead was tolerable (~125ms). At 3,757 chunks for 75M tiles it would have cost ~87 seconds of pure allocation overhead.

---

## Infrastructure

| Parameter | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 4090 (sm_89, 128 SMs, 24 GB GDDR6X) |
| Platform | vast.ai |
| Offer ID | 34354977 |
| Instance ID | [unconfirmed — not captured in session transcript] |
| Rate | $0.281/hr |
| Image | pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel |
| Disk requested | 30 GB |
| Session wall time | ~37 minutes (18:42 to 19:19) |
| Total cost | ~$0.17 |

The instance was rented specifically for the octant run (separate from the earlier 300K instance 34633595). Destroyed immediately after data retrieval and verified empty.

---

## Execution

### Phase 0: Code Fixes (Local, before renting)

Two changes were made to the codebase before deploying to the GPU instance:

**Fix 0a: Eliminate per-chunk cudaMalloc overhead (`main.cu`)**

Moved GPU buffer allocation (`TileBatchDeviceMemory` and host-side result buffers) from inside the chunk loop to before it. Buffers are allocated once for the max chunk size (20,000 tiles), reused across all chunks, and freed after the loop ends. The last chunk (10,112 tiles) uses the same buffers with a smaller launch count.

This eliminated ~87 seconds of `cudaMalloc`/`cudaFree` overhead at 3,757 chunks.

**Fix 0b: Full-octant coordinate generation (`gen_coords.py`)**

Added `--full-octant` CLI flag to generate all first-octant tiles:

```
python3 gen_coords.py --full-octant
```

Math:
- R = 850,000,000
- j_max = floor(R / (256 * sqrt(2))) = tower index where y approaches x
- For each tower j from 0 to j_max:
  - a_lo = j * 256
  - base_y = isqrt(R^2 - a_lo^2) (integer sqrt, no floating point)
  - For r from 0 to 31: b_lo = base_y + r * 256
  - Dead-tile filter: skip if b_lo + 256 <= a_lo (tile falls entirely below y=x diagonal)
- Buffered I/O with progress reporting every 100K towers

Output: `coords_octant_850M.bin` — same binary format as band coord files (uint32 num_tiles header, then int64 a_lo + int64 b_lo per tile).

### Phase 1: vast.ai Cycle

1. **Rent** — 4090 instance via vast.ai CLI, 30 GB disk, with tmux and pigz in onstart command
2. **Deploy** — rsync tile_cuda_multi_kernel/ and gen_coords.py to instance, patch Makefile to sm_89, build
3. **Generate coords on remote** — ran `gen_coords.py --full-octant` on the instance to avoid uploading the 1.1 GB coord file
4. **CUDA dump** — `./tile_kernel_multi dump /root/coords_octant_850M.bin /root/cuda_octant_850M.bin`, run inside tmux, 3,757 chunks of 20,000 tiles
5. **Compress** — `pigz -1` (fast parallel gzip) reduced 10.4 GB to 4.5 GB (43.3% ratio)
6. **Download** — scp compressed file to Mac Mini: 4.5 GB in 23 minutes (3.3 MB/s effective)
7. **Decompress locally** — `gunzip` to recover full dump
8. **Destroy instance** — destroyed and verified zero instances running

All phases succeeded on first attempt. No retries, no SSH drops, no errors.

---

## Results

| Metric | Value |
|---|---|
| Total towers | 2,347,816 |
| Rows per tower | 32 |
| Raw tile slots | 75,130,112 (after dead-tile filtering) |
| Dead tiles in output | 0 |
| GPU dump time | 532,074.4 ms (8 min 52 sec) |
| Per-tile time | 0.007 ms/tile |
| Effective throughput | ~141K tiles/s |
| Chunk count | 3,757 chunks of 20,000 (last chunk: 10,112) |

### File Sizes

| File | Size |
|---|---|
| Raw dump | 11,119,256,580 bytes (10.4 GiB) |
| Compressed (pigz -1) | 4,808,948,132 bytes (4.5 GiB) |
| Coords file | 1,202,081,796 bytes (1.12 GiB) |

Raw dump size matches expected: 4 + 75,130,112 * 148 = 11,119,256,580 bytes.

### Throughput

The allocation fix brought per-tile time from 0.008 ms (300K run with per-chunk allocation) to 0.007 ms (octant run with hoisted allocation). At 3,757 chunks the fix saved approximately 87 seconds of pure `cudaMalloc`/`cudaFree` overhead.

The effective throughput of ~141K tiles/s (vs the 155K benchmark ceiling) reflects remaining overhead from disk I/O (writing 10.4 GB to the instance's SSD) and device-to-host memcpy between chunks. These are unavoidable in dump mode.

### Coverage Geometry

The dump covers the full first octant of the arc at R=850M:

| Boundary | Coordinate | Tower index |
|---|---|---|
| First tile (y-axis, 90 degrees) | (0, 850000000) | j=0, r=0 |
| Last tile (diagonal, ~45 degrees) | (601040640, 601048824) | j=2,347,816, near y=x |

Sweep direction: towers advance from j=0 (x=0, pure y-axis) rightward to j_max (x approximately equal to y, the y=x diagonal). Within each tower, 32 rows stack radially outward from the arc. Dead tiles near the diagonal (where the tile falls entirely below y=x) are filtered out during coordinate generation and never reach the GPU.

### Spot-Check

| Tile | Position | Primes |
|---|---|---|
| First | (0, 850000000) | 2,311 |
| Last | (601040640, 601048824) | 2,303 |

Prime counts are consistent with the 300K census mean of ~2,274 per tile.

---

## Artifacts

### Output File

`results/4090-octant/cuda_octant_850M.bin` — 10.4 GiB

### Record Format

148-byte fixed-width records, prefixed by a 4-byte little-endian header:

| Offset | Size | Field |
|---|---|---|
| 0 (header) | 4 bytes | uint32 LE tile count (75,130,112) |
| Per record: | | |
| +0 | 8 bytes | int64 LE a_lo (tile x-corner) |
| +8 | 8 bytes | int64 LE b_lo (tile y-corner) |
| +16 | 4 bytes | uint32 LE prime_count (CUDA domain, including collar) |
| +20 | 128 bytes | TileOp payload |

Total file size: 4 + 75,130,112 * 148 = 11,119,256,580 bytes.

The TileOp payload (128 bytes) encodes: 3-byte header (off_I, off_L, off_R face offsets), followed by up to 125 bytes of packed port data (group-ID + face-local port index per entry). The 300K census showed mean payload usage of 76.8/125 bytes with worst-case 113/125 bytes.

### Code Changes

| File | Change |
|---|---|
| `tile_cuda_multi_kernel/src/main.cu` | GPU buffer allocation hoisted before chunk loop; host-side result buffers also hoisted |
| `gen_coords.py` | Added `--full-octant` mode with integer sqrt, dead-tile filtering, buffered I/O |

### Related Files

| File | Description |
|---|---|
| `results/4090-300k/cuda_{85,67,45}deg.bin` | 300K calibration dumps (predecessor) |
| `tile-compare/analyze.py` | Statistical analysis tool (ready for octant use) |
| `tile-compare/dump_io.py` | Binary dump reader/writer |

---

## Validation Status

**Fully validated (CUDA correctness).** The 300K sample (3 bands of 100K tiles) showed:
- 300,000/300,000 exact byte-for-byte match between CUDA and C++ reference
- 0 overflow/poisoned tiles
- 0 dead tiles in output
- Angle-invariant statistics across 85 degrees, 67.5 degrees, and 45 degrees

`analyze.py` was run on the full octant binary on 2026-04-12. Results across all 75,130,112 tiles:
- **0 poisoned (overflow) tiles**
- **0 dead tiles**
- **All 75,130,112 tiles normal**
- Max payload usage: **123 of 125 bytes** (2-byte minimum slack; no overflow path needed)
- Groups/tile: median 10, max 24, p99 = 16
- Stats JSON: `results/4090-octant/cuda_octant_850M.bin.stats.json`

The full-octant dataset is cleared for compositor development. No overflow fallback path is required.

---

## Open Items

1. **Diagonal seam analysis** — tiles near the y=x boundary (the last towers in the sweep) are geometrically unusual. The 45-degree band in the 300K census found at least one tile with zero inner-face ports (Face I min = 0). The full octant contains many more such tiles and may surface additional edge cases.

2. **C++ cross-validation at scale** — the 300K run proved bitwise correctness for representative tiles. A full-octant C++ dump would take ~20 hours on the Mac Mini (75M tiles at ~1K tiles/s with 12-core parallelism). May not be worth the cost unless the analyze.py pass surfaces anomalies.
