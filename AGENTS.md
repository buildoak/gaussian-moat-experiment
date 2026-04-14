# AGENTS.md — gaussian-moat-cuda

## Canonical Status

`tiles-maxxing/` contains all active code for the gaussian-moat-cuda project. Specs, results, artifacts, and documentation live at the repo root. Legacy code (first-generation CUDA implementations, old crates) is in `legacy/`. Do not treat legacy as reference — new work starts from the specs and tile-cpp reference.

When building CUDA kernels, design from the specs — not by porting legacy `.cu` files. The old kernels encode assumptions (buffer sizes, phase boundaries, memory layout) that may not survive the spec.

## Directory Layout

```
gaussian-moat-cuda/
├── tiles-maxxing/                 — All active code
│   ├── tile-cpp/                  — C++ reference implementation (libtile.a)
│   ├── tile_cuda_multi_kernel/    — Current GPU path, 5-kernel pipeline
│   │                                (K1 Sieve → K2 MR → K3 Compact → K4 UF → K5 FaceEncode)
│   ├── tiles-compositor/          — Campaign runner + compositor library + grid
│   ├── tile-compare/              — Python comparison tools (compare.py, analyze.py, dump_io.py)
│   ├── tile-validator/            — Python tile validation
│   └── vast-ai/                   — Deploy script for vast.ai instances
├── docs/                          — 6 canonical specs + docs/supportive/ (analysis/audit reports)
├── results/                       — Benchmark and campaign result dumps
│   ├── 4090-300k/                 — 300K tile benchmark (42MB)
│   └── 4090-octant/               — Full octant dump R=850M K_SQ=40 (10GB)
├── artifacts/                     — Data artifacts, coordinate files, profiling, sweep data
│   ├── 2026-04-14-sweep4/         — Canonical 4090 R-sweep dataset (68 runs, 55 R values)
│   ├── 2026-04-13-r-sweep/        — Earlier R-sweep (historical subset)
│   ├── *.bin                      — Coordinate and C++ dump files
│   └── profiling/                 — 4090 profiling data
├── legacy/                        — First-generation code (src/, tile-probe/, old crates)
│   └── tiles-maxxing-archive/     — Superseded tile-cuda variants, old dispatch prompts
├── AGENTS.md                      — This file (project status and structure)
└── CLAUDE.md                      — Agent instructions
```

## Authoritative Specs

Six documents in `docs/` are the **single most authoritative source of truth** for this project:

| Spec | File | Governs |
|------|------|---------|
| Tile Spec | `docs/tile_spec.md` | Tile geometry, coordinate system, collar, sieve domain |
| Grid Spec | `docs/grid_spec.md` | Grid layout, tiling scheme, octant structure, radial shells |
| Tile Internals Spec | `docs/tile_operations.md` | 5-phase pipeline, TileOp encoding, port structure, group semantics |
| CUDA Internals | `docs/tile_internals_cuda.md` | GPU kernel implementation details |
| Compositor Spec | `docs/compositor_spec.md` | Tile composition, seam merging, multi-tile operations |
| Campaign Spec | `docs/campaign_spec.md` | Campaign runner pipeline, burst mode, radial sweep |

**Authority chain:** User input > Specs > Code. Code must align with specs. If code contradicts a spec, the code is wrong unless there is strong enough evidence to challenge the spec — in which case the agent **must explicitly surface the conflict to the user** before proceeding. Agents do not silently override specs.

## Build Parameterization

K_SQ is now a build parameter, not hardcoded:
- `make K_SQ=36` or `cmake -DK_SQ=36` — all derived constants auto-computed via constexpr
- Separate build directories: `build-k36/`, `build-k40/`
- Default is **K_SQ=40**

## Tile Boundary Convention (HARD RULE)

A tile of side S = 256 lattice units contains **257 x 257 lattice points** (fencepost: 256 segments = 257 endpoints per axis). Tile proper covers closed interval [0, S] per axis. (updated 2026-04-09: 257x257 shared boundary convention)

Adjacent tiles SHARE boundary lattice points. Tile A at origin a_lo owns [a_lo, a_lo + S]. Tile B at origin a_lo + S owns [a_lo + S, a_lo + 2S]. The column/row at a_lo + S belongs to BOTH tiles. (updated 2026-04-09: 257x257 shared boundary convention)

Half-open [0, S) ownership is UNSOUND for moat proofs — it creates disjoint face-prime sets that break composition. See `docs/supportive/2026-04-09-convention-a-soundness-audit.md`. (updated 2026-04-09: 257x257 shared boundary convention)

Any code using `< S` or `< TILE_SIDE` for the in_tile check MUST be changed to `<= S` or `< S+1`. This is non-negotiable. (updated 2026-04-09: 257x257 shared boundary convention)

## Sieve Candidate Density

The grid sweeps from the Y axis (tower j=0, x≈0) rightward to y=x (45°). Split-prime residue collapse only occurs when **both** coordinates are small (< ~2,048). A single large coordinate (e.g., b≈830M at tower j=0) is sufficient for normal sieve coverage.

**Census results (2026-04-10, 490K+ tiles):**
- Operating range (R ≥ 800M), all towers including j=0: max **5,882 candidates**
- Near-origin (both a,b < ~2,048): up to 7,744 — never reached at operating radii
- Distribution is tight: P99.99 ≈ 5,835, mean ≈ 5,686

**Implication:** `MAX_CANDIDATES_GPU = 6144` is safe for the operating range (4.5% headroom over observed max). The near-origin elevation is irrelevant — operating radii guarantee at least one large coordinate per tile.

## Performance Record

- **2026-04-10:** 2,818 tiles/s on Jetson Orin (SM 8.7), ~2.8x Mac Mini 12-core CPU baseline. Sinclair 7-base MR, Barrett sieve, per-prime face extraction. 43 regs/thread, 4 blocks/SM. Commit `5997aa1`. Session `a913f1dc`. Full log: `docs/supportive/2026-04-10-cuda-kernel-optimization-spree.md`.
- **2026-04-10:** UF popcount unroll → 2,867 tiles/s (+4.1%). Phase3: 3.42M → 2.65M cycles (-22.5%). Full UF parallelization (4 strategies: atomic UF, edge buffer, multi-warp, warp-cooperative) all regressed 5-15% due to nvcc global register allocation coupling — structural changes to UF perturbed codegen for sieve/MR phases. **TODO:** Retest aggressive UF parallelization on 3090/4090 where larger register file may avoid the coupling effect.
- **2026-04-10:** Multi-kernel architecture → **3,333 tiles/s (+39.2%** vs monolithic 2,395 in same test, +18.3% vs best monolithic 2,818). 5 separate kernels (Sieve/MR/Compact/UF/FaceEncode) with per-kernel `--maxrregcount` via separate compilation (`-dc`/`-dlink`). FJ64_262k 2-round MR (512 KB hash table, L2-cached) replaces 7-witness Sinclair. Parallel 288-thread atomicCAS union-find replaces serial 32-thread. Byte-identical correctness vs monolithic. K2 MR 54%, K4 UF 27%, K1 Sieve 16%. Full log: `docs/supportive/2026-04-10-multi-kernel-architecture.md`.
- **2026-04-10:** K2 register sweep → **3,431 tiles/s (+2.2%).** Swept 8 caps (uncapped through 36). Winner: `--maxrregcount=44` — forces nvcc from natural 46→44 regs, better instruction schedule. Same 4 blocks/SM, zero spills. Spills at 40 regs cancel 5-block occupancy gain. Commit `dd56e2e`.
- **2026-04-11:** RTX 3090 baseline (vast.ai, sm_86, 82 SMs) → **66,800 tiles/s** at 2,000 tiles. 19.5x Jetson, consistent with SM count × clock scaling. K2 MR share rises to 62% (from 54% on Jetson). INT32 pipeline is the binding constraint — consumer GPUs dominate $/tiles-s. Session cost: ~$0.09. Full analysis: `docs/supportive/2026-04-11-3090-performance-analysis.md`.
- **2026-04-11:** RTX 4090 tuning sweep (vast.ai, sm_89, 128 SMs) → **155,452 tiles/s** at 10K tiles, **156,868 tiles/s** at 20K tiles. 2.01x over 3090 at 2K tiles (134,541). Jetson-tuned config (maxreg=44, 288 threads) already optimal — no micro-optimization moved the needle: mont_to_gpu fix negligible (ALU hides the modulo), `__ldg` zero impact (compiler already uses LDG via `__restrict__`), register sweep confirms 44 still best. **Batch size is the real variable** — throughput climbs from ~135K at 640 tiles to ~157K at 20K. Session cost: ~$0.14. Full sweep: `docs/supportive/2026-04-11-4090-tuning-sweep.md`.
- **2026-04-11:** Hardware profiling (nsys + SASS) on 4090. **K2 is 72.9% INT32 ALU, 0.9% memory** — purely INT throughput-bound. **K1 is 62.8% INT32 ALU** — constant memory cache contention likely explains 1.20x 3090→4090 scaling (expected 1.56x). K4 is mixed INT+memory (67.6% INT, 13.5% memory) with scatter/gather atomics. Inter-kernel gaps: 2-3 us (negligible). ncu blocked by container `CAP_SYS_ADMIN` restriction. Session cost: ~$0.09. Full analysis: `docs/supportive/2026-04-11-4090-hardware-profiling.md`.
- **2026-04-11:** Multi-stream overlap: **dead end at 10K+ batch sizes.** 2-stream pipeline with double-buffered intermediate arrays achieves +1.3% at 10K (141,625 vs 139,839) and -7.1% at 20K (142,948 vs 153,903). Each kernel already generates 10K+ thread blocks saturating all 128 SMs — no idle compute for stream overlap. Splitting into sub-batches reduces per-kernel amortization. Correctness verified. Patch: `vast-ai/multi_stream.patch`. Full results: `docs/supportive/2026-04-11-multi-stream-overlap.md`.
- **2026-04-11:** K1 smem + sieve extension: **both dead ends.** (1) Moving 10 KB sieve tables from constant memory to shared memory (cooperative load from global): K1 proportion unchanged at ~22%, zero improvement. Constant memory was NOT the bottleneck — K1 is purely INT32 ALU-bound. 38 regs (up from 30) with smem, 5 blocks/SM maintained, zero spills. (2) Sieve extension sweep (10K-50K sieve limit): candidates drop linearly (5,687 at 10K to 4,848 at 50K = -14.8%), K2 drops proportionally (40.3 to 34.6ms = -14.1%), but K1 cost grows faster (+8.1ms) than K2 saves (-5.7ms). Net negative at every sweep point. Best was 12K at 138,323 tiles/s vs 137,374 at 10K (+0.7%, within noise). Crossover at ~12K confirms 10K sieve is already optimal. Correctness verified: identical prime counts and tileop bytes across all sieve limits. Session cost: <$0.10. Full sweep data: `docs/supportive/2026-04-11-k1-smem-sieve-extension.md`.

### Baseline Commit (Fallback Point)

**155K baseline commit:** `03fd7f2` (2026-04-11). Multi-kernel pipeline, maxreg=44, 288 threads, FJ64_262k 2-round MR. Validated across Jetson (3,431), 3090 (66,800), 4090 (155,452 tiles/s). All subsequent optimization attempts (mont_to_gpu, __ldg, register sweep, multi-stream) were neutral or harmful on 4090 — this config is the proven optimum before K1 smem fix and sieve extension.

**To revert:** `git checkout 03fd7f2 -- tiles-maxxing/tile_cuda_multi_kernel/`

### Cross-Platform Summary

| GPU | SMs | Arch | tiles/s (2K) | tiles/s (10K+) | vs Jetson |
|-----|-----|------|-------------|----------------|-----------|
| Jetson Orin | 8 | sm_87 | 3,431 | — | 1.0x |
| RTX 3090 | 82 | sm_86 | 66,800 | 78,984 | 19.5x |
| RTX 4090 | 128 | sm_89 | ~144,000 | 155,452 | 45.2x |

### Operational Learnings (4090 Sessions)

1. **Micro-optimizations are exhausted.** mont_to_gpu modulo removal, `__ldg` hints, register sweep beyond 44 — all within noise on 4090. The Jetson-tuned config transfers directly to sm_89. Don't waste cloud time retesting these.
2. **Batch size matters more than kernel tuning.** 640→20K tiles: +27% throughput from launch overhead amortization alone. Use 10K+ tiles for any production or benchmark run.
3. **Multi-stream overlap is a dead end.** At 10K+ tiles, each kernel fully saturates 128 SMs. Inter-kernel gaps are 2-3 us. No scheduling optimization can help — the pipeline is compute-bound.
4. **K1 scaling anomaly: NOT constant memory contention.** K1 sieve scaled only 1.20x from 3090→4090 (expected 1.56x). Hypothesis was L1 constant cache thrash at 128 SMs. **Tested:** moved 10 KB sieve tables to shared memory (cooperative load from global). Result: zero improvement — K1 proportion stayed at 22%. K1 is purely INT32 ALU-bound, same as K2. The 1.20x scaling gap is likely Ada Lovelace INT32 pipe sharing with FP32, not a cache issue.
5. **K2 is purely INT32-bound, but sieve extension cannot help.** 72.9% INT ALU, 0.9% memory. Tested sieve extension from 10K to 50K primes: K2 drops proportionally with candidates (-14.1% at 50K), but K1 cost grows faster (+50% at 50K) because the sieve loop is also INT32-bound. Net negative at every sweep point beyond 12K. The 10K sieve limit is already optimal.
6. **ncu requires CAP_SYS_ADMIN on vast.ai.** Standard containers block GPU performance counters. Use bare-metal or request privileged containers for detailed warp stall / throughput analysis.
7. **256 threads/block breaks correctness** — kernels assume 288 for 257-column coverage. 320 threads works but performs identically. 288 is the only valid choice.
8. **Pipeline is at hardware INT32 throughput limits.** All tested optimizations (mont_to_gpu fix, `__ldg`, register sweep, multi-stream, K1 smem, sieve extension) are neutral or harmful. The pipeline is bound by the Ada Lovelace INT32 pipe across K1, K2, and K4. Remaining paths: algorithmic changes to reduce total INT operations per candidate (e.g., faster primality test, reduced sieve iteration cost), or moving to hardware with higher INT32 throughput (H100's 128 SM x 4 INT32 units/SM).
9. **Fixed-chunk streaming decouples GPU memory from burst size.** As of `3ffd202`, `run_stream()` allocates GPU buffers at a fixed `STREAM_CHUNK_SIZE` (200K tiles, ~10GB) and processes arbitrarily large bursts in internal chunks. Coords are read into host memory, processed in GPU-sized bites, results accumulated and sent as one pipe response. Burst size is now purely a campaign concept (when to check spanning), not a GPU memory constraint. Default `--burst-size` raised to 28K towers (~1M tiles/burst). Effective rate ~134K tiles/s (86% of 155K GPU benchmark).
10. **Compositor CPU overhead is the remaining bottleneck.** The 14% gap between GPU benchmark (155K tiles/s) and campaign effective rate (134K tiles/s) is compositor work: parsing 1M TileOps and union-find merges take ~1s per burst, during which the GPU idles waiting for the next burst. Pipe I/O (~0.05s) and cudaMemcpy (~5ms) are noise. **Fix:** double-buffered async pipeline in `campaign.cpp` — send burst N+1 coords to GPU while compositor processes burst N results. Requires two threads or non-blocking pipe I/O. This would fully hide compositor cost behind GPU compute and close the gap to ~155K tiles/s.

## Warp Profiling Investigation TODO

**Observation (2026-04-14):** K_SQ=36 (256B TileOp) on RTX 4090 hit **151K tiles/s** — nearly identical to K_SQ=40 (128B TileOp) at ~155K tiles/s. K5 FaceEncode is only **2.2% of total kernel time**. Doubling the payload size had negligible throughput impact.

**Hypothesis:** K2 Miller-Rabin dominates at 58.1%. K5 is memory-bound but the 4090's bandwidth absorbs the 2x payload doubling. TileOp size is simply not on the critical path.

**Investigation needed (requires privileged container — ncu is blocked by CAP_SYS_ADMIN on standard vast.ai):**

1. **Warp-level profiling** — warp occupancy, warp stall reasons (memory dependency vs issue, execution, etc.), achieved vs theoretical occupancy per kernel. Use `ncu --set full` or `ncu --metrics` targeting stall reasons.
2. **Memory throughput on K5** — global load/store throughput, L2 hit rate, DRAM bandwidth utilization. Confirm K5 is bandwidth-bound and that 4090 has headroom at the 256B write size.
3. **Register pressure** — compile all 5 kernels with `--ptxas-options=-v` and log spills, registers/thread, shared memory usage for both K_SQ=36 and K_SQ=40 builds side-by-side.
4. **Shared memory vs L1 partition** — check whether K5's larger writes pressure the L1/shared partition and affect co-resident kernels.
5. **Cliff check at higher K_SQ** — does the invariant hold at K_SQ=44 (sqrt(44) ≈ 6.63) and K_SQ=48 (sqrt(48) ≈ 6.93)? TileOp payload grows further. Need at least one data point to confirm no bandwidth cliff before declaring the format universal.

**Implication if confirmed:** The 256B TileOp format is the universal format for all K_SQ campaigns. No per-campaign format negotiation, no payload-size tuning, no separate build variants for different K_SQ values — one wire format covers everything up to at least K_SQ=48.

**How to unblock ncu:** Request a bare-metal or privileged container on vast.ai (`--disk=50` and add `--privileged` in the offer filter). Estimated session cost: ~$0.10-0.15 on 4090.

## Compositor (tiles-compositor/)

The compositor is an in-tree C++ library at `tiles-compositor/` that takes TileOp binaries and produces a SPANNING/MOAT verdict via union-find over tile groups. Includes the campaign runner and grid logic.

**Reference document:** `tiles-compositor/docs/supportive/2026-04-12-compositor-logic.md` — single source of truth for all compositor logic. Read it before touching compositor code.

### Face Convention (HARD RULE)

| Face | Enum | Position | Direction | h1 stored? |
|------|------|----------|-----------|------------|
| FACE_I | 0 | Bottom of tile (low b) | Within-tower | No |
| FACE_O | 1 | Top of tile (high b) | Within-tower | No |
| FACE_L | 2 | Left side (low a) | Between-tower | Yes |
| FACE_R | 3 | Right side (high a) | Between-tower | Yes |

- `tile_row = b - b_lo` (vertical offset). `tile_col = a - a_lo` (horizontal offset).
- Tile row 0 = bottom (lowest b). Row 31 = top (highest b). Tiles stack UPWARD.
- h1 for L/R faces = tile_row = b-offset along the vertical edge. Needed because adjacent towers have different base_y.
- I/O faces have NO h1 — within-tower tiles share the same a-range, matching is exact by position.

### decode_group_id: L/R faces ONLY (HARD RULE)

`decode_group_id(group_byte)` masks `& 0x7F` to strip the h1 bit-steal from L/R group bytes. **Never apply it to I/O face groups** — I/O groups are plain 8-bit labels with no bit-steal. Applying decode_group_id to I/O groups silently truncates labels >= 128.

**Fixed 2026-04-13** (commit `7e28a44`): Removed decode_group_id() from I/O face paths in compositor boundary collectors. Was silently corrupting groups >= 128. Additionally, a defensive 128-group poison guard was added — if max_group_label >= 128, the tile is treated as dead (belt-and-suspenders with the existing encoder poison checks).

### Between-Tower Row Mapping (HARD RULE)

Current tower j's row r maps to previous tower j-1's row `r - q` (where `q = delta[j-1] / TILE_SIDE`). **NOT `r + q`.** The previous tower starts HIGHER (larger base_y), so the same absolute b corresponds to a LOWER row index in the previous tower. This was a bug that shipped and was caught by audit on 2026-04-12.

## Bug Fixes (2026-04-13)

1. **decode_group_id I/O face fix** (commit `7e28a44`) — Removed decode_group_id() from I/O face paths in compositor. I/O faces use raw 8-bit group labels, not the L/R h1-encoded format. Was silently corrupting groups >= 128.

2. **Defensive 128-group poison guard** (commit `7e28a44`) — Added compositor-side guard: if max_group_label >= 128, tile treated as dead. Belt-and-suspenders with existing encoder poison checks.

3. **O(N^2) spanning check fix** — Replaced has_spanning() recompute-from-scratch (O(T^2) total) with incremental reachability tracking. Maintains inner/outer reachability bitmasks per UF root, detects spanning on union. O(1) check, O(new_members) per tower. Debug assert in finalize() cross-checks against old algorithm.

## K_SQ=36 Campaign Post-Mortem (2026-04-13)

First campaign attempt at R=80,015,782 with K_SQ=36 on RTX 4090 failed. Three issues found:

1. **O(N^2) compositor scaling** — now fixed (see bug fix #3 above)
2. **22% overflow tile rate** — GPU caps (`MAX_FACE_PRIMES_PER_FACE=256`, `MAX_FACE_PORTS_GPU=32`, `MAX_TOTAL_PORTS_GPU=128`) too tight for K_SQ=36
3. **Latent parse failure** — suggesting CUDA memory corruption

Full post-mortem: `docs/supportive/2026-04-13-k36-campaign-postmortem.md`

## Strategy (2026-04-14)

_claim by claim
lemma by lemma
move towards the paper_

## Current State (2026-04-14)

- **K_SQ=40 pipeline:** Verified at scale. Full R-sweep complete — 68 runs, 55 unique R values, zero overflows. Deployed code pinned to `c73085f`.
- **K_SQ=36 pipeline:** Blocked on GPU cap adjustment and overflow investigation.
- **CUDA streaming:** Fixed-chunk architecture (`3ffd202`). GPU allocates for 200K tiles, processes any burst in internal chunks. Burst size decoupled from GPU memory.
- **4090 instance:** Destroyed 2026-04-14 after full data verification (MD5-verified all artifacts). No active cloud instances.

### Latest Results

Canonical sweep dataset: `artifacts/2026-04-14-sweep4/`
- `RUN-CATALOG.md` — provenance, exact parameters, full run list (68 runs)
- `sweep4-results.jsonl` — final 47-run overnight sweep
- `sweep-intermediate.jsonl` — earlier intermediate runs
- `sweep4-summary.txt`, `sweep4-console.log`, `logs/` — campaign logs

### K_SQ=40 R-Sweep Results

| R | Verdict | Wall (s) | Note |
|---|---------|----------|------|
| 600M | SPANNING | — | Tower 931 (instant) |
| 875M | SPANNING | 188.8 | Tower ~660K (27% — barely spans) |
| 900M | **MOAT** | 756.5 | First MOAT in sweep |
| 925M | **MOAT** | 774.4 | |
| 950M | SPANNING | 7.6 | Non-monotonic island — instant |
| 975M | **MOAT** | 814.2 | |
| 1000M | **MOAT** | 828.1 | |
| 1025M | **MOAT** | 849.9 | |
| 1050M | **MOAT** | 879.1 | |
| 1075M | **MOAT** | 880.1 | |
| 1100M | **MOAT** | 916.9 | |
| 1125M | **MOAT** | 933.0 | |

**Key finding: the transition is non-monotonic.** R=950M SPANs instantly despite MOATs at 925M and 975M. The ISE estimate (~839M) captures the statistical trend but not the local prime structure.

Full report: `docs/supportive/2026-04-13-r-sweep-results.md`

## Working Documents

All documents produced during work go to `docs/supportive/`. No exceptions — audits, analyses, benchmarks, investigations, design notes, all land there.

**Naming:** `YYYY-MM-DD-slug.md` (e.g., `2026-04-09-encoding-overflow-analysis.md`)

**Frontmatter:**
```yaml
---
title: <descriptive title>
date: YYYY-MM-DD
engine: <codex|claude|gemini|coordinator|human>
type: <audit|analysis|benchmark|investigation|design-note|report>
status: <complete|partial|superseded>
refs: [<spec or file this relates to>]
---
```

All fields required except `refs` (include when the document relates to a specific spec or code file).

## Testing Requirements

**All tests must run at R >= 800M.** No tests at low radii. The system was not designed for the near-origin regime — prime density, buffer sizing, and port structure behave differently there. Tests at (0,0), (100,100), or (10000,10000) are not meaningful validation.

Concrete minimums for test tiles:
- Use off-axis coordinates (not on real or imaginary axis)
- All `(a_lo, b_lo)` must satisfy `sqrt(a_lo^2 + b_lo^2) >= 800,000,000`
- Coordinates must be multiples of `TILE_SIDE` (256)
- Include at least two angular positions (e.g., 30 deg and 45 deg) to catch anisotropy

Legacy test coordinates in `test_e2e.cpp` and `test_sieve.cpp` are placeholders from initial development. They should be replaced with operating-point coordinates.

## Jetson Interaction Workflow

### Connection

| Detail | Value |
|--------|-------|
| SSH alias | `ssh jetson` |
| Host | `169.254.24.100` |
| User | `clifford` |
| GPU | Orin Nano (sm_87 Ampere) |

The Mac Mini has no CUDA compiler. All compilation and execution happens on Jetson. Code is written locally, pushed via rsync, built and run remotely.

### Remote Directory Layout

```
~/tiles-maxxing/
├── tile-cuda/       # CUDA kernel (primary target)
└── tile-cpp/        # C++ reference (cross-validation)
```

Both directories are synced from the local `tiles-maxxing/` tree. `tile-cpp/` must be present on Jetson so test harnesses can cross-validate CUDA output against the reference implementation.

### Push-Compile-Run Commands

**Push code to Jetson:**
```bash
# Push CUDA kernel
rsync -avz --delete tiles-maxxing/tile-cuda/ jetson:~/tiles-maxxing/tile-cuda/

# Push C++ reference (for cross-validation)
rsync -avz --delete tiles-maxxing/tile-cpp/ jetson:~/tiles-maxxing/tile-cpp/
```

Run from the repo root on the Mac.

**Compile on Jetson:**
```bash
ssh jetson "cd ~/tiles-maxxing/tile-cuda && nvcc -arch=sm_87 -O3 -std=c++17 --expt-relaxed-constexpr -lineinfo -o tile_kernel src/main.cu"
```

Adjust source path and output name to match actual file layout.

**Run on Jetson:**
```bash
ssh jetson "cd ~/tiles-maxxing/tile-cuda && ./tile_kernel"
```

**Pull results back:**
```bash
rsync -avz jetson:~/tiles-maxxing/tile-cuda/results/ tile-cuda/results/
```

### Build Flags

| Flag | Purpose |
|------|---------|
| `-arch=sm_87` | Orin Nano compute capability (Ampere) |
| `-O3` | Full optimization |
| `-std=c++17` | C++17 standard |
| `--expt-relaxed-constexpr` | Allow constexpr in device code |
| `-lineinfo` | Debug line info without disabling optimizations |
| `-Xptxas -dlcm=ca` | L1 cache all global loads (add when benchmarking memory) |
| `--maxrregcount=N` | Cap register usage to increase occupancy (tune per kernel) |

For shared memory configuration, set at launch site with `cudaFuncSetAttribute` — Orin Nano supports up to 48 KB shared memory per SM.

### Agent Rules for Jetson

1. **Codex workers CANNOT ssh.** They have no network access to Jetson. Workers write code locally in `tile-cuda/`. Only the coordinator or user terminal pushes, compiles, and runs on Jetson.
2. **Workflow boundary:** Workers produce code + local tests (syntax, logic checks). The coordinator (or user) handles the rsync-compile-run cycle and feeds results back.
3. **Never assume CUDA compilation succeeded.** Always capture and inspect nvcc output. Jetson nvcc version may differ from what workers expect — surface errors verbatim.
4. **Results stay in-tree.** Pull benchmark/test output into `tile-cuda/results/` so it is version-controlled alongside the kernel code.

## vast.ai Cloud GPU

Operational docs and deploy scripts live in `tiles-maxxing/vast-ai/`. Read `tiles-maxxing/vast-ai/README.md` for the full workflow.

### Hard Rules

1. **tmux for everything.** SSH drops are common. Every command > 5 seconds runs in tmux.
2. **DESTROY instances when done.** `vastai destroy instance $ID`. Verify with `vastai show instances`. Never leave instances running.
3. **Pin SSH endpoint once.** Cache port and host at session start. Never re-resolve mid-run — the API can return changed bindings.
4. **Patch `-arch` flag.** Makefile defaults to `sm_87` (Jetson). Change to `sm_86` (3090), `sm_89` (4090), or `sm_80` (A100) before building on cloud instances.
5. **Do not modify local Makefile.** Arch patching happens on the remote instance only. The local copy stays at `sm_87` for Jetson.
6. **Budget awareness.** Track cost via `vastai show instances`. 3090: ~$0.13/hr, 4090: ~$0.27/hr. Total spend across both sessions: ~$0.23. Destroy immediately after work.
