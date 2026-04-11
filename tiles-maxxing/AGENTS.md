# AGENTS.md — tiles-maxxing

## Canonical Status

`tiles-maxxing/` is the **new canonical state** of all progress for the gaussian-moat-cuda project. The old CUDA implementations (`src/gpu_uf.cu`, `src/fat_stripe_cuda.cu`, `tile-probe/` crates) are **inefficient legacy**. They were first-generation proof-of-concept code. Do not treat them as reference implementations or optimization targets. New work starts from the specs and tile-cpp reference in this directory.

When building CUDA kernels, design from the specs — not by porting legacy `.cu` files. The old kernels encode assumptions (buffer sizes, phase boundaries, memory layout) that may not survive the spec.

## Authoritative Specs

Three documents in `docs/` are the **single most authoritative source of truth** for this project:

| Spec | File | Governs |
|------|------|---------|
| Tile Spec | `docs/tile_spec.md` | Tile geometry, coordinate system, collar, sieve domain |
| Grid Spec | `docs/grid_spec.md` | Grid layout, tiling scheme, octant structure, radial shells |
| Tile Internals Spec | `docs/tile_operations.md` | 5-phase pipeline, TileOp encoding, port structure, group semantics |
| Compositor Spec | `docs/compositor_spec.md` | Tile composition, seam merging, multi-tile operations |

**Authority chain:** User input > Specs > Code. Code must align with specs. If code contradicts a spec, the code is wrong unless there is strong enough evidence to challenge the spec — in which case the agent **must explicitly surface the conflict to the user** before proceeding. Agents do not silently override specs.

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
4. **K1 scaling anomaly: constant memory contention.** K1 sieve scaled only 1.20x from 3090→4090 (expected 1.56x). SASS shows 62.8% INT32 ALU + 10 KB constant memory table. The L1 constant cache thrashes at 128 SMs. Fix: move sieve tables to shared memory or global with `__ldg`.
5. **K2 is purely INT32-bound.** 72.9% INT ALU, 0.9% memory. Sieve extension to reduce candidates entering MR is the highest-ROI optimization path.
6. **ncu requires CAP_SYS_ADMIN on vast.ai.** Standard containers block GPU performance counters. Use bare-metal or request privileged containers for detailed warp stall / throughput analysis.
7. **256 threads/block breaks correctness** — kernels assume 288 for 257-column coverage. 320 threads works but performs identically. 288 is the only valid choice.
8. **Remaining optimization is algorithmic:** sieve extension (reduce K2 candidates), persistent kernel K2 (eliminate launch overhead at small batches), or K1 constant memory fix (restore linear SM scaling).

## Working Documents

All documents produced during tiles-maxxing work go to `docs/supportive/`. No exceptions — audits, analyses, benchmarks, investigations, design notes, all land there.

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

**All tests must run at R ≥ 800M.** No tests at low radii. The system was not designed for the near-origin regime — prime density, buffer sizing, and port structure behave differently there. Tests at (0,0), (100,100), or (10000,10000) are not meaningful validation.

Concrete minimums for test tiles:
- Use off-axis coordinates (not on real or imaginary axis)
- All `(a_lo, b_lo)` must satisfy `sqrt(a_lo² + b_lo²) ≥ 800,000,000`
- Coordinates must be multiples of `TILE_SIDE` (256)
- Include at least two angular positions (e.g., 30° and 45°) to catch anisotropy

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
rsync -avz --delete tile-cuda/ jetson:~/tiles-maxxing/tile-cuda/

# Push C++ reference (for cross-validation)
rsync -avz --delete tile-cpp/ jetson:~/tiles-maxxing/tile-cpp/
```

Run from `tiles-maxxing/` on the Mac.

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

Operational docs and deploy scripts live in `vast-ai/`. Read `vast-ai/README.md` for the full workflow.

### Hard Rules

1. **tmux for everything.** SSH drops are common. Every command > 5 seconds runs in tmux.
2. **DESTROY instances when done.** `vastai destroy instance $ID`. Verify with `vastai show instances`. Never leave instances running.
3. **Pin SSH endpoint once.** Cache port and host at session start. Never re-resolve mid-run — the API can return changed bindings.
4. **Patch `-arch` flag.** Makefile defaults to `sm_87` (Jetson). Change to `sm_86` (3090), `sm_89` (4090), or `sm_80` (A100) before building on cloud instances.
5. **Do not modify local Makefile.** Arch patching happens on the remote instance only. The local copy stays at `sm_87` for Jetson.
6. **Budget awareness.** Track cost via `vastai show instances`. 3090: ~$0.13/hr, 4090: ~$0.27/hr. Total spend across both sessions: ~$0.23. Destroy immediately after work.
