# AGENTS.md — gaussian-moat-cuda

Orientation for agents about to build or modify campaigns. Read top-to-bottom before touching code. Every section earns its space.

## Ground Truth — Tsuchimura's Moat (k²=36)

| Parameter | Value | Notes |
|-----------|-------|-------|
| **Paper** | METR 2004-13 | Tsuchimura, 2004 |
| **k²** | 36 | Step size k = 6 |
| **Moat boundary** | **80,015,782** | Distance from origin where connectivity terminates |
| **Interpretation** | R_outer < 80,015,782 → SPANNING expected | R_outer ≥ 80,015,782 → MOAT expected |

The moat boundary is the **farthest point reachable** from origin via Gaussian prime steps ≤6. Any annulus with R_outer below this value should show SPANNING (connectivity exists). The moat appears somewhere just past 80,015,782.

**Local reference:** `~/thinking/pratchett-os/centerpiece/research/gaussian-moat/2026-03-15-tsuchimura-baseline-reconstruction.md` (Appendix B reconstruction from paper Section 4 / Table page 9).

**Validation checkpoint:** A correct implementation at k²=36 MUST return SPANNING for R=80M (R_outer=80,008,192) and MOAT for R values past 80,015,782.

---

## Current State (2026-04-22)

- **Blueprint v3 canonical.** `methodology/lemmas_v2/campaign-blueprint.md` supersedes the sqrt-36/40 pipelines.
- **RTX 4090 instance active.** vast.ai ID 35378303 — destroy when audit complete.
- **K1 overflow bug identified (2026-04-22).** `kernel_sieve.cu` silently truncates candidates at MAX_CANDIDATES_GPU without setting overflow flag. Can cause false MOAT. Fix in progress.
- **campaign-sqrt-36-v2 pending build.** v1 campaigns (`campaign-sqrt-36`, `campaign-sqrt-40`) are stale — use as directory templates only, not as reference for geometry or compositor math.
- **BZ pre-build soundness gate pending.** `build/bz_check.py` not yet implemented. Build must fail if BZ interval is non-empty.

## Canonical Math & Engineering

| Role | File | Scope |
|------|------|-------|
| **Math SSoT** | `methodology/lemmas_v2/tile-operator-definition-v-claude.md` | Soundness/completeness. Lemmas 3, 4, 6, 10, Theorems 9, 11, 12, grid invariants I1/I2/I4, norm-form `geo_I`/`geo_O`, reflection closure. |
| **Engineering SSoT** | `methodology/lemmas_v2/campaign-blueprint.md` | Snapped grid, TileOp v3 (256 B), K4 geo-tests, flag-driven compositor, I/O protocol, test plan, delta plan. |
| Math backlog | `methodology/lemmas_v2/BACKLOG.md` | Non-blocking B1–B10 items. |
| CUDA tuning reference | `docs/tile_internals_cuda.md`, `docs/tile_operations.md` §4–§8 | Montgomery, MR witnesses, shared-mem budgets, kernel launch tuning. **Ignore their geometry and compositor sections** — superseded. |

**Authority chain:** User input > `methodology/lemmas_v2/` > `docs/` (CUDA-only) > code.

Blueprint bakes in three prerequisites from earlier pivots: `C = ⌊√K⌋`, no dead-end pruning, strict I/O port-count equality on shared faces.

Read the math doc before writing proofs or challenging the verdict. Read the blueprint before touching grid, kernel, TileOp, or compositor code.

## Before You Code — Checklist

1. **Grid is snapped** — uniform `S = 256` sub-lattice, offset `(1, 1)`. See §Grid Architecture.
2. **TileOp v3 is 256 B fixed** — per-group inner/outer flags, not per-prime. See §TileOp v3 Format.
3. **K4 emits per-prime `is_inner`/`is_outer`** via the norm-form test; flags aggregate to UF roots. See §Geo-test Integration.
4. **Verdict is flag-driven** — no staircase geometry, no `h1`, no delta. See §Verdict Logic.
5. **Overflow is conservative SPANNING-biased** — sound but potentially false-positive. See §Overflow.

## Grid Architecture (snapped grid)

Uniform sub-lattice at offset `(o_x, o_y) = (1, 1)`, spacing `S = 256`. All tile boundaries fall on global lattice lines; adjacent towers have delta `= k·S`. All faces face-to-face aligned, zero fractional offset. The `(1, 1)` offset is load-bearing, not cosmetic: it keeps the reflection axes `x = 0` and `y = x` strictly inside halo rather than on tile boundaries, sidestepping BACKLOG B1's axis-ownership ambiguity. See `docs/supportive/2026-04-20-codex-axis-tower-analysis.md`. Under snapped grid, Lemma 4 (identical ordered ports on shared faces) holds trivially; all stitching is positional.

- **Collar / face-strip depth:** `C = ⌊√K⌋ = 6` for both `K = 36` and `K = 40`. Load-bearing for edge completeness. Use `floor_isqrt`, never `ceil_isqrt`, for `C`.
- **Halo extension:** `HALO = C = 6`.
- **Pre-filter bound:** `2·R·⌈√K⌉ + 1` — use **ceiling** here, not `C`. A floor-based bound would reject candidate-band primes with `|ε| ∈ (2·R·C, 2·R·√K]` that satisfy the canonical `ε² ≤ 4·R²·K` test, yielding an unflagged `geo_I` component and a potential false MOAT.
- **Octant folding (Theorem 12, D₄).** Pipeline runs on the octant `R`; reflection closure guarantees the octant verdict equals the full-annulus verdict. Side-exposed `face_L` at `i = i_min` and `face_R` at `i = i_max` are not stitched. Defense-in-depth assertion: `R_inner > √(2K)` at campaign init.
- **I1/I2/I4 asserted at build time.** I4 is structurally proven (math doc §Geometric invariants); an empirical one-pass scan is optional defense-in-depth.

## TileOp v3 Format

256 B fixed. Single format — no dual 128/256 B dispatch:

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 4 B | `n[4]` | Port counts `N_I, N_O, N_L, N_R` (face order I, O, L, R) |
| 4 | 192 B | `face_groups[192]` | Concatenated per-port group labels, 48 B per face |
| 196 | 16 B | `inner_flags` | Bit-packed **per-group** inner flag, 128 groups × 1 bit |
| 212 | 16 B | `outer_flags` | Bit-packed **per-group** outer flag, 128 groups × 1 bit |
| 228 | 1 B | `tile_flags` | bit0 OVERFLOW, bit1 EMPTY, bit2 TOWER_CLOSING |
| 229 | 27 B | `reserved` | Alignment / future extension |

Budgets: `sum(n) ≤ 192` ports (observed max ~75 at K=36; 2.5× headroom). `max_label ≤ 128` groups (observed max ~9; extreme headroom). `inner_flags` and `outer_flags` are **per-UF-group**, not per-prime — K4 aggregates per-prime tests by bitwise OR into the group's root.

Port ordinals are assigned by canonical lex sort `(h, p⊥)` of port representatives per face, tie-break `(p⊥, h)`; byte-for-byte deterministic.

## Geo-test Integration (K4)

K4 extends the final UF pass with per-prime norm-form tests. For prime `p = (a, b)` with `‖p‖² = a² + b²`:

```
is_inner_prime(p) iff (‖p‖² − R_inner² − K)² ≤ 4·R_inner²·K
is_outer_prime(p) iff (R_outer² − ‖p‖² + K)² ≤ 4·R_outer²·K
```

Two-stage: int64 `|ε| ≤ 2·R·⌈√K⌉ + 1` pre-filter, then i128 squared comparison as defense-in-depth. Per-prime flags aggregate into the prime's UF-root bits via `atomicOr` on a shared-mem `smem_flags_by_root` buffer; K5 reads and packs into the TileOp's `inner_flags`/`outer_flags`. Estimated cost: `< 5%` K4 slowdown, `< 1.5%` campaign slowdown.

Compositor propagates flags on inter-tile merge: each UF root accumulates `REACH_INNER | REACH_OUTER` bits as tiles ingest.

## Verdict Logic

The campaign is `MOAT` iff no UF root in the grid-wide port graph ever carries both `REACH_INNER` and `REACH_OUTER`; else `SPANNING`. Incremental check: on every union, the merged root's reach bitmask is ORed; if both bits coincide, `spanning_detected_` latches true and the campaign short-circuits. Preserves the O(1)-per-union fix that replaced the O(N²) recompute-from-scratch bug.

No staircase geometry, no side-exposed heuristics. The Exit Lemma (Lemma 10) + Theorem 11 guarantee every `geo_I`/`geo_O` prime is represented through a port of its UF component, so flag-driven port marking is sufficient for the octant verdict. Theorem 12 lifts the octant verdict to the full annulus.

## Overflow & Conservative Spanning

Two overflow modes set `tile_flags |= OVERFLOW_BIT`:

1. **Port-count overflow:** `sum(n) > 192`.
2. **Group-count overflow:** `max_label > 128`.

Compositor response: `mark_tile_as_spanning_conservative` — force the tile's root to `REACH_INNER | REACH_OUTER`, latching SPANNING. **Sound** (a false MOAT is impossible), potentially false SPANNING. Observed rate `< 10⁻⁴` at sqrt-40 operating parameters — acceptable as-is. Monitor in v3; if observed rate exceeds 0.1%, escalate to the 512 B extended-format re-encode path (wired but not implemented).

## Face Convention (HARD RULE)

| Face | Enum | Position | Direction |
|------|------|----------|-----------|
| FACE_I | 0 | Bottom of tile (low b) | Within-tower |
| FACE_O | 1 | Top of tile (high b) | Within-tower |
| FACE_L | 2 | Left side (low a) | Between-tower |
| FACE_R | 3 | Right side (high a) | Between-tower |

Port matching is **strict positional ordinal**: port `i` of `face_O` on tile `T_{i,j}` matches port `i` of `face_I` on tile `T_{i,j+1}`; port `i` of `face_R` on `T_{i,j}` matches port `i` of `face_L` on `T_{i+1,j}`. Port counts on shared faces must be equal (Lemma 4); mismatch = bug (panic in debug, log-and-treat-as-spanning in release). No `h1`. No delta. No q/f decomposition.

## Tile Boundary Convention (HARD RULE)

Tile of side `S = 256` contains **257 × 257 lattice points** (256 segments = 257 endpoints per axis). Tile proper covers closed interval `[0, S]` per axis. Adjacent tiles SHARE boundary lattice points — the column/row at `a_lo + S` belongs to both tile A (`[a_lo, a_lo + S]`) and tile B (`[a_lo + S, a_lo + 2S]`). Half-open `[0, S)` ownership is UNSOUND for moat proofs. Any in-tile check must use `<= S` or `< S+1`. Non-negotiable. See blueprint §4.1.

## Build Parameterization

`K_SQ` is a build parameter, not hardcoded. `make K_SQ=36` or `cmake -DK_SQ=36`. All derived constants auto-computed via `constexpr` (`C = floor_isqrt(K_SQ)`, etc.). Separate build directories: `build-k36/`, `build-k40/`. Default `K_SQ=40`.

## Canonical Status

`tiles-maxxing/` holds all active code. `legacy/` holds first-generation crates; do not treat as reference. Design CUDA kernels from the specs and the blueprint — do not port legacy `.cu` files, they encode pre-snapped-grid assumptions (buffer sizes, phase boundaries, memory layout) that do not survive.

## Directory Layout

```
gaussian-moat-cuda/
├── tiles-maxxing/                 — All active code
│   ├── tile-cpp/                  — C++ reference (libtile.a)
│   ├── tile_cuda_multi_kernel/    — GPU path, 5-kernel pipeline (K1..K5)
│   ├── tiles-compositor/          — Campaign runner + compositor + grid
│   ├── tile-compare/              — Python comparison tools
│   ├── tile-validator/            — Python tile validation
│   └── vast-ai/                   — Cloud GPU deploy scripts
├── methodology/lemmas_v2/         — Math SSoT + engineering blueprint + backlog
├── docs/                          — CUDA kernel tuning reference (geometry/compositor obsolete)
├── legacy/                        — First-generation code; not reference material
├── AGENTS.md                      — This file
└── CLAUDE.md                      — Agent instructions
```

## Working Documents

All working documents (audits, analyses, benchmarks, investigations, design notes) go to `docs/supportive/`. No exceptions.

**Naming:** `YYYY-MM-DD-slug.md`

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

All fields required except `refs` (include when the document targets a specific spec or code file).

## Jetson Interaction Workflow

| Detail | Value |
|--------|-------|
| SSH alias | `ssh jetson` |
| Host | `169.254.24.100` |
| User | `clifford` |
| GPU | Orin Nano (sm_87 Ampere) |

The Mac Mini has no CUDA compiler. All compilation and execution happens on Jetson. Code is written locally, pushed via rsync, built and run remotely. Remote layout: `~/tiles-maxxing/{tile-cuda, tile-cpp}/`. Both trees are needed — `tile-cpp` for cross-validation harnesses.

**Push:** `rsync -avz --delete tiles-maxxing/tile-cuda/ jetson:~/tiles-maxxing/tile-cuda/` (run from repo root).

**Compile:** `ssh jetson "cd ~/tiles-maxxing/tile-cuda && nvcc -arch=sm_87 -O3 -std=c++17 --expt-relaxed-constexpr -lineinfo -o tile_kernel src/main.cu"`

**Run:** `ssh jetson "cd ~/tiles-maxxing/tile-cuda && ./tile_kernel"`

**Pull results:** `rsync -avz jetson:~/tiles-maxxing/tile-cuda/results/ tile-cuda/results/`

Build flags: `-arch=sm_87 -O3 -std=c++17 --expt-relaxed-constexpr -lineinfo`. Tune `--maxrregcount=N` per kernel. For shared memory configuration, set at launch with `cudaFuncSetAttribute` — Orin Nano supports up to 48 KB per SM.

## vast.ai Cloud GPU

Operational docs and deploy scripts in `tiles-maxxing/vast-ai/`. Read `vast-ai/README.md` for the full workflow.

1. **tmux for everything.** SSH drops are common. Any command over 5 seconds runs in tmux.
2. **Do NOT destroy vast.ai instances unless explicitly asked by the user.** If cleanup is needed, use `vastai destroy instance $ID` and verify with `vastai show instances`.
3. **Pin SSH endpoint once.** Cache port and host at session start. Never re-resolve mid-run.
4. **Patch `-arch` flag on remote.** Makefile defaults to `sm_87` (Jetson). Change to `sm_86` (3090), `sm_89` (4090), or `sm_80` (A100) on the remote instance. Do not modify the local Makefile.
5. **Budget awareness.** Track via `vastai show instances`. 3090 ~$0.13/hr, 4090 ~$0.27/hr. Destroy immediately after work.
