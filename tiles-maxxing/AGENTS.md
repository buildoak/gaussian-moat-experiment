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
