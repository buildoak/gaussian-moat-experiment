# Campaign Blueprint — Gaussian Moat Detection (v3, snapped-grid with UF-flag verdict)

**Canonical engineering source of truth.** Complements the mathematical SSoT at `methodology/lemmas_v2/tile-operator-definition-v-claude.md`. Every structural decision below derives from that doc; when they disagree, the math doc wins.

**Status:** pre-implementation (2026-04-19). Supersedes the pipelines in `tiles-maxxing/campaign-sqrt-36` and `tiles-maxxing/campaign-sqrt-40`.

---

## 0. Relation to existing pivots

Two upstream decisions already align the repo with the new canonical math:

- **Snapped Grid Pivot (2026-04-16, `AGENTS.md` §"Snapped Grid Pivot").** All tiles on a uniform `S`-spaced sub-lattice. Under snapped grid, Lemma 4 (identical ordered ports on shared faces) holds trivially; no `h1` bit-steal encoding is needed; all stitching is purely positional.
- **Methodology Alignment (2026-04-15, `AGENTS.md` §"Methodology Alignment").** Tight collar `C = ⌊√K⌋`; no dead-end pruning (required for port-set agreement); I/O port-count strict equality per Lemma 4.

Both are now baseline assumptions. The `campaign-sqrt-36/40` code did NOT yet complete these migrations — the blueprint treats their completion as prerequisite.

---

## 1. Canonical references

| Reference | Purpose |
|---|---|
| `methodology/lemmas_v2/tile-operator-definition-v-claude.md` | Math SSoT (soundness, completeness, verdict definition) |
| `methodology/lemmas_v2/BACKLOG.md` | Non-blocking math items (tracked, not yet addressed) |
| `methodology/lemmas_v2/campaign-blueprint.md` | **This document — engineering SSoT** |
| `docs/tile_internals_cuda.md` | Load-bearing GPU tuning knowledge (preserved) |
| `docs/tile_operations.md` §4–§8 | Sieve / MR / Compact / UF-interior kernel details (preserved) |
| `docs/supportive/2026-04-15-oi-face-stitching-soundness.md` | Shared-boundary 257×257 convention |

Legacy specs in `docs/` cover the pre-pivot design. Consult them for engineering conventions (CUDA kernel budgets, Montgomery, Miller–Rabin, shared-mem budgets) but **ignore their compositor / matching math** — it is superseded by the canonical math doc.

---

## 2. Campaign parameters

Six deployment constants. All `constexpr` except the radii, which are CLI arguments.

| Parameter | Symbol | Source | Project value |
|---|---|---|---|
| Step-squared bound | `K` | `-DK_SQ=N` (build flag) | 36 or 40 |
| Collar / face-strip depth | `C = ⌊√K⌋` | derived (`floor_isqrt`) | 6 (K=36 and K=40) |
| Halo extension | `HALO = C` | derived | 6 |
| Tile side | `S` | `constants.h` | 256 |
| Grid offset | `(o_x, o_y) ∈ ℤ ∩ [0, S)²` | `constants.h` | `(1, 1)` recommended |
| Inner radius | `R_inner` | `--r-inner` | ~80 000 000 |
| Outer radius | `R_outer` | `--r-outer` | `R_inner + 8192` |

**Collar note.** The `AGENTS.md` Methodology Alignment (2026-04-15) already dropped `C` from `⌈√K⌉` to `⌊√K⌋`. At `K = 40` both face-strip depth AND halo extension are now `6` (was `7`). At `K = 36` both are `6`. The sqrt-36/40 codebase has `ceil_isqrt` in places; this must be replaced with `floor_isqrt` uniformly (BACKLOG — lineage traced in Opus report on sqrt-40 diff, §"Face-strip off-by-one"). Under the canonical math `C = ⌊√K⌋` is load-bearing for the face-strip definition and edge completeness proof (math doc line 115).

**BZ reconciliation check.** At build init, a one-shot check confirms that no Gaussian-prime norm sits inside the bad-zone intervals `BZ_I` and `BZ_O` for `(R_inner, R_outer, K)`. Implementation: `build/bz_check.py` (PEP 723 Python; `mpmath` 50-digit endpoints → guarded integer enumeration → Gaussian-norm factorization test). Execute as a pre-build CMake custom target; build fails if any Gaussian-prime norm lies in `BZ_I ∪ BZ_O`. Project status at `(R_inner, R_outer) = (80_000_000, 80_008_192)`: **PASS** for `K = 36` (3 BZ candidates, 0 Gaussian-prime norms) and `K = 40` (2 BZ candidates, 0 Gaussian-prime norms). See `docs/supportive/2026-04-20-codex-bz-check-design.md`.

**Annulus thickness assertion.** `R_outer − R_inner > S·√2 + 2·√K` (required by Theorem 11 Case A.2). At `S = 256, K = 40`: threshold ≈ `375`. Project width `8192 ≫ 375`.

**Adjacent-octants sufficiency.** `R_inner > √(2K)` (required by Adjacent Octants Lemma). At `K ≤ 40`: threshold `≈ 8.94`. Project `R_inner ~ 10⁸` trivially satisfies. Assert at campaign init.

**Integer-overflow pre-filter.** The norm-form test squares `(‖p‖² − R² ± K)` which exceeds int64 outside the candidate band. Use a two-stage test:

```
int64 eps = norm_sq - R_inner_sq - K;
if (llabs(eps) > prefilter_bound_inner) return false;
return ((__int128)eps * eps) <= FOUR_RIN_SQ_K;
```

with `prefilter_bound_inner = 2·R_inner·⌈√K⌉ + 1` (symmetric for outer). **CRITICAL — use `⌈√K⌉ = ceil_isqrt(K)`, NOT `C = ⌊√K⌋`.** For non-square `K` (e.g., `K = 40`, `√K ≈ 6.32`), the tighter bound `2·R·C + 1 = 2·R·6 + 1` would be strictly smaller than `2·R·√K + 1 ≈ 2·R·6.32 + 1`, rejecting primes with `|ε| ∈ (2·R·C, 2·R·√K]` that actually satisfy the canonical `ε² ≤ 4·R²·K` test. Such a false-negative would leave a true `geo_I` prime's UF component unflagged and could yield a false MOAT (unsound). Pre-filter cuts virtually all non-candidate primes in one int64 comparison; i128 is the safety net.

---

## 3. Pipeline architecture

Three processes, three stages:

```
Host orchestrator (campaign.cpp)
  ├── builds Grid (§4)
  ├── streams tile-coord bursts to CUDA worker (stdin)
  ├── receives TileOp bursts from CUDA worker (stdout)
  └── feeds TileOp bursts to Compositor column-by-column (§7)

CUDA worker (main.cu)
  └── 5-kernel per-tile pipeline, one CUDA block per tile:
      K1 Sieve → K2 MR → K3 Compact → K4 UF (+ geo tests) → K5 FaceEncode (+ flag pack)

Compositor (compositor.cpp)
  └── per-column UF merge across tiles, marks I_ports / O_ports from flags,
      tracks REACH_INNER | REACH_OUTER per UF root, emits moat verdict
```

**Parallelism model.** Per-burst ≤ 200 K tiles. One CUDA block per tile, 288 threads per block (AGENTS.md §"Operational Learnings 4090 Sessions" point 7 — 288 is the only valid choice). Five kernels launched back-to-back per burst. Host compositor ingests column-by-column (single-threaded); GPU burst `b+1` computes while compositor processes burst `b` (existing double-buffer plumbing).

**Verdict.** Compositor emits `MOAT` if no UF component ever carries both `REACH_INNER` and `REACH_OUTER` bits; else `SPANNING`. Incremental `check_spanning_incremental` path preserved from sqrt-36 (AGENTS.md §"Bug Fixes 2026-04-13" bug #3).

---

## 4. Grid construction

### 4.1 Snapped grid

Tiles on the uniform sub-lattice `{(o_x + iS, o_y + jS) : (i, j) ∈ ℤ²}`. Proper region `[o_x + iS, o_x + (i+1)S] × [o_y + jS, o_y + (j+1)S]` (closed interval → 257×257 lattice points, already a hard rule in `AGENTS.md` §"Tile Boundary Convention"). Halo extends `C` outward in every direction.

**Offset choice: `(o_x, o_y) = (1, 1)`.** Sidesteps BACKLOG **B1** (axis-boundary active-tile ambiguity when `o_x = 0`) at zero structural cost. At project scale the `(1, 1)` offset has identical active-tile count within rounding to `(0, 0)`.

**Canonical octant.** `R = { (x, y) ∈ ℤ² : x ≥ 0, y ≥ x }`, i.e. `arg z ∈ [π/4, π/2]`. All pipeline components (K4 enumeration, grid tower construction, compositor face conventions) use this orientation. Tools may fold via `D₄` symmetry for implementation convenience (`tools/coverage/coverage_verifier.py` folds to `arg z ∈ [0, π/4]`), but results must be reconciled against canonical.

**Axis-adjacent towers.** Use octant-local UF only. K4 does not construct reflection-aware edges; the compositor does not stitch side-exposed faces. Theorem 12 `D₄` closure handles reflection at the verdict level via the Monotone Reflection Lemma. See `docs/supportive/2026-04-20-codex-axis-tower-analysis.md`.

### 4.2 Active-tile enumeration

`T_{i,j}` is active iff its proper region contains at least one lattice point of `R`. For each column `i` with non-empty tower:

```
tower_i = [j_low(i), j_high(i)]     // contiguous by I1
j_low(i)  = min { j : T_{i,j} active }
j_high(i) = max { j : T_{i,j} active }
```

Host-side loop over `i = i_min..i_max`:
1. Over `x ∈ [o_x + iS, o_x + (i+1)S]` compute `y_lower(x) = max(x, √max(0, R_inner² − x²))` and `y_upper(x) = √max(0, R_outer² − x²)`.
2. `j_low(i) ← ⌊(min_x y_lower(x) − o_y) / S⌋`, corrected upward if the candidate tile has no actual R-lattice-point (scan needed only at boundary tiles).
3. `j_high(i) ← ⌊(max_x y_upper(x) − o_y) / S⌋`, symmetric downward correction.

All integer arithmetic. i128 for `R_outer²` at project scale. No `llround(√)` — a recurring bug source in the legacy grid builder (preserved lesson from `grid_spec.md` and the 2026-04-16 tower-height formula fix, `AGENTS.md` §"Snapped Grid Pivot" bug note).

### 4.3 Invariants asserted at build time

- **I1** (tower contiguity): `Tower_i` is a contiguous integer interval.
- **I2** (bounded shift): `|j_low(i+1) − j_low(i)| ≤ 1`, `|j_high(i+1) − j_high(i)| ≤ 1`.
- **I4** (no diagonal orphans): for every diagonally-adjacent active pair, ≥ 1 face-neighbor common to both is active.

**I4 is structurally proven** in the canonical doc (§Geometric invariants, lines 454–538) for both bulk and tower-closing regimes. No runtime gate required for soundness. **Optional defense-in-depth:** a one-pass scan over diagonally-adjacent active pairs at campaign init that verifies I4 empirically — O(|active tiles|), < 1 s at project scale, panics on violation.

**Empirical verifier.** `tools/coverage/coverage_verifier.py` passes at project parameters for both `K = 36` and `K = 40`: `220,994` towers, `8.18 M` active tiles. I1, I2, I4, and annulus-thickness assertions all hold at project and small scale. See `docs/supportive/2026-04-20-codex-coverage-verifier.md`.

### 4.4 Tower-closing regime

Columns `i` with `tower_height(i) ≤ 1` cluster near `x = R_outer/√2` (Tower-closing margin corollary). These are handled by I4's tower-closing case analysis (canonical doc lines 500–536); no special engineering logic beyond the general snapped-grid enumeration. The side-exposed `face_R` at column `i_max` is closed legitimately by Theorem 12 reflection closure (§9).

### 4.5 Grid data structure

```cpp
struct Grid {
  int64_t R_inner_sq, R_outer_sq;
  int K, S, C;
  int o_x, o_y;
  int i_min, i_max;
  std::vector<int> j_low;              // size i_max - i_min + 1
  std::vector<int> j_high;             // size i_max - i_min + 1
  std::vector<int64_t> tower_offset;   // prefix sum → flat-index into all_tiles[]
  int64_t total_tiles;
};

struct TileCoord {
  int32_t i, j;
  int64_t a_lo, b_lo;   // world-space origin = o_x + i*S, o_y + j*S
};
```

Flat-indexed tile list: `all_tiles[tower_offset[i - i_min] + (j - j_low(i))]`. Compositor and CUDA worker share this indexing convention.

### 4.6 Parametrization passed to kernels

Per-tile GPU input (24 B / tile, SoA across the burst):

```cpp
struct TileInput {
  int64_t a_lo, b_lo;   // origin
  int32_t i, j;         // grid index (unused by K1–K3, consumed by K4 + K5)
};
```

Per-campaign `__constant__` memory (uploaded once at campaign start):

```cpp
struct CampaignConstants {
  int64_t R_inner_sq, R_outer_sq;
  int64_t prefilter_inner, prefilter_outer;   // 2*R*ceil_isqrt(K) + 1 each — NOT 2*R*C (floor) — see §2 Integer-overflow pre-filter
  int64_t four_rin_sq_k_hi, four_rin_sq_k_lo; // i128 as hi/lo pair
  int64_t four_rout_sq_k_hi, four_rout_sq_k_lo;
  int K_SQ;
  int S;
  int C;
  int o_x, o_y;
};
__constant__ CampaignConstants c_campaign;
```

Sieve tables (`c_split_barrett[609]`, `c_inert_barrett[619]`) and the per-K backward-offset table (`c_bk_dr/dc`) are unchanged from sqrt-36/40 and uploaded in the same place.

---

## 5. TileOp data structure (v3)

### 5.1 Design principles

- Single fixed-size format. No dual 128 / 256 B overflow path in the hot loop — overflow handled by a dedicated re-encode pass (§10).
- Positional port matching under snapped grid (no `h1`, no bit-steal on any face).
- Per-`G_tile`-UF-component `inner_flags` / `outer_flags` carried verbatim to the compositor.
- CPU (tile-cpp) and GPU (tile_cuda_multi_kernel) produce byte-identical output — regression-gated by the restored `dump_tileops` harness.

### 5.2 Layout (256 B fixed)

```cpp
struct TileOp {                          // 256 B total
  uint8_t n[4];                          //   4 B: N_I, N_O, N_L, N_R  (port counts per face)
  uint8_t face_groups[192];              // 192 B: concatenated group labels per port,
                                         //         face order I, O, L, R
  uint8_t inner_flags[16];               //  16 B: bit-packed inner-flag for groups 1..128
  uint8_t outer_flags[16];               //  16 B: bit-packed outer-flag for groups 1..128
  uint8_t tile_flags;                    //   1 B: bit0=OVERFLOW, bit1=EMPTY, bit2=TOWER_CLOSING
  uint8_t reserved[27];                  //  27 B: future extension / alignment slack
};
static_assert(sizeof(TileOp) == 256);
```

**Port-count budget.** `sum(n) ≤ 192`. Empirically validated 2026-04-20 across 600 sampled tiles at `R ∈ {60M, 80M, 800M}` and `K ∈ {36, 40}`. Peak observed `sum(n) = 104` (46% headroom on 192 budget). Project-scale `R = 80M` max: `sum(n) = 91` for `K = 36`, `73` for `K = 40`. Per-face practical: `N_I + N_O` typically `≤ 10`, `N_L + N_R` typically `10–50`.

**Group-count budget.** `128 groups / tile` (one bit per group in each 16 B flag mask). Peak observed `max_label = 80` (37% headroom on 128 budget). No 256 B overflow occurred in the 600-tile sample. `K = 40` runs roughly 30% lighter than `K = 36` on ports and groups. See `docs/supportive/2026-04-20-codex-tileop-sizing-study.md`.

**Alignment.** 256 B matches existing extended-format infrastructure (`TILEOP_EXT_SIZE = 256` already wired in `tiles-compositor/include/types.h`). Single format removes the 128 / 256 dual-path complexity entirely.

### 5.3 Mapping to canonical math

| Canonical field | Wire location |
|---|---|
| `face_f_groups[1..N_f]` | `face_groups[off_f .. off_f + n[f] − 1]` with prefix-sum offsets `off_I=0, off_O=n[0], off_L=off_O+n[1], off_R=off_L+n[2]` |
| `inner_flags[g]` for `g ∈ 1..128` | Bit `g − 1` of `inner_flags[(g − 1) / 8]` |
| `outer_flags[g]` | Same pattern in `outer_flags` |
| Port ordinal `p ∈ 1..N_f` | Index within face region of `face_groups` |
| Port representative `(h, p⊥)` | Baked in at emit time by canonical lex sort; not transmitted |

### 5.4 Port enumeration (canonical rule)

Per face `f`, enumerate primes in `face_f_primes` by lex `(h, p⊥)`:

- **face_I / face_O**: `h = col = x − (o_x + iS)`, `p⊥ = row − (o_y + jS)` for I; for O, `p⊥ = row − (o_y + (j+1)S)`.
- **face_L / face_R**: `h = row = y − (o_y + jS)`, `p⊥ = col − (o_x + iS)` for L; for R, `p⊥ = col − (o_x + (i+1)S)`.

Ports = connected components of `G_facestrip_f` (induced subgraph of `G_full` on `face_f_primes`). Assign ordinals `1..N_f` by lex-sort of representatives; tie-break by `(p⊥, h)` secondary sort (bit-for-bit deterministic, per BACKLOG **B9**).

**Implementation:** new face-strip UF sub-pass in K5. Replaces the 1-D greedy scan of sqrt-36/40 (`face_extract.cpp:127–162`, `kernel_face_encode.cu:237–283`). Cost negligible — each face strip has `≤ 13 × 257 ≈ 3.3 K` candidate positions, `~40` primes observed post-sieve.

### 5.5 Face-group labels

Each port stores its `G_tile` UF label (NOT a renumbered post-prune label). The label is the canonical `ufs_local(w; G_tile)` integer, in range `1..128`.

**No dead-end pruning** (AGENTS.md §"Methodology Alignment" change #2). Single-port single-face groups are retained because dropping them breaks port-set agreement on shared faces (Lemma 4 strict equality). The old `prune.cpp` / GPU `prune_dead_ends_gpu_k5` code is deleted.

---

## 6. CUDA kernels

### 6.1 Inherited unchanged

- **K1 `kernel_sieve`** — byte-identical across sqrt-36/40; 288 threads / block; Barrett residue marking; 609 split + 619 inert primes in `__constant__` memory. Registered winner config: `maxrregcount = 44`, 4 blocks / SM on sm_89. **Unchanged.**
- **K2 `kernel_mr`** — byte-identical; FJ64_262k 2-round Miller–Rabin; int64 modular exponentiation; L2-cached 512 KB hash table. **Unchanged.**
- **K3 `kernel_compact`** — byte-identical; Hillis–Steele scan in shared memory. **Unchanged.**

### 6.2 K4 `kernel_uf` — extended with per-prime geo tests

**Current:** atomic lock-free UF with 288 threads, `smaller-root-wins` CAS tiebreaker (`kernel_uf.cu:57–75`), outputs `d_parent[MAX_PRIMES_GPU]`.

**New inputs:** `const TileInput* d_inputs` and the four norm-form constants from `c_campaign`.

**New output:** `d_group_flags[num_tiles * 32]` — 32 bytes per tile = 2 bits per UF-label (inner/outer) for up to 128 labels.

**Octant clipping.** K4 halo prime enumeration MUST be clipped to the canonical octant `R = { (x, y) ∈ ℤ² : x ≥ 0, y ≥ x, R_inner² ≤ x² + y² ≤ R_outer² }`. Off-octant halo primes are excluded from the local DSU. Rationale: Theorem 11 proves the verdict for `G_full` restricted to `R`; unioning off-octant primes would compute a strictly larger graph and contaminate the verdict.

**Added work (per prime, after path compression, within the existing final pass):**

```cuda
const TileInput in = d_inputs[tile_idx];
const uint32_t packed = tile_prime_pos[i];
const int row = packed / SIDE_EXP;
const int col = packed % SIDE_EXP;
const int64_t a = in.a_lo - COLLAR + col;
const int64_t b = in.b_lo - COLLAR + row;
const int64_t norm_sq = a*a + b*b;

const int64_t eps_i = norm_sq - c_campaign.R_inner_sq - c_campaign.K_SQ;
const int64_t eps_o = c_campaign.R_outer_sq - norm_sq + c_campaign.K_SQ;

const bool is_inner =
    (llabs(eps_i) <= c_campaign.prefilter_inner) &&
    (((__int128)eps_i * eps_i) <= load_i128(c_campaign.four_rin_sq_k_hi, c_campaign.four_rin_sq_k_lo));
const bool is_outer =
    (llabs(eps_o) <= c_campaign.prefilter_outer) &&
    (((__int128)eps_o * eps_o) <= load_i128(c_campaign.four_rout_sq_k_hi, c_campaign.four_rout_sq_k_lo));

const uint16_t root = atomic_find_root(tile_parent, i);
if (is_inner) atomicOr(&smem_flags_by_root[root >> 2], INNER_BIT << ((root & 3) * 2));
if (is_outer) atomicOr(&smem_flags_by_root[root >> 2], OUTER_BIT << ((root & 3) * 2));
tile_parent[i] = root;
```

`smem_flags_by_root` is a 4 KB shared-memory buffer keyed by UF root (2 bits / root × 128 roots / slice × up to MAX_PRIMES roots → compact into `uint8_t smem_flags_by_root[MAX_PRIMES_GPU / 4]`). At kernel end, the block coalesces `smem_flags_by_root` into the appropriate slice of `d_group_flags`.

**Performance impact.** Two int64 mul-adds + two i128 mul per prime. At ~500 primes/tile × 288 threads, ~500 additional ALU ops / thread. K4 is 27% of campaign time (sqrt-36 profile); the addition is est. `< 5%` K4 slowdown, `< 1.5%` campaign slowdown.

### 6.3 K5 `kernel_face_encode` — refactored

**Removed:**
- Dead-end pruning (`kernel_face_encode.cu:301–389`) — per AGENTS.md §"Methodology Alignment".
- 1-D greedy port-clustering scan (`kernel_face_encode.cu:237–283`) — replaced by face-strip UF.
- `h1` bit-steal on L/R group bytes — no longer needed under positional matching.

**Kept:**
- Parallel face-prime extraction to per-face shared-memory lists.
- Per-face lex sort.
- Single-threaded 256 B packed encode (one thread emits the TileOp).

**Added:**
- **Per-face `G_facestrip_f` UF sub-pass.** For each face (4 of them), run a tiny UF over the face-strip primes using their G_full edges. Produces port assignments (ordinals `1..N_f` = face-strip components). Cost trivial (≤ 40 primes per face).
- **Flag remap.** Read `d_group_flags[tile_idx * 32 + …]` bits per UF-label; pack into 16 B `inner_flags` + 16 B `outer_flags` bit arrays in the TileOp tail.
- **Sentinel emit.** If `sum(N_f) > 192` or `max_label > 128`: set `tile_flags |= OVERFLOW_BIT` and emit zeros in `face_groups` / flag arrays.

**Shared-memory budget.** Existing K5 uses ~11 KB; adding face-strip UF scratch and flag staging brings it to ~13 KB, well under the 48 KB Jetson cap (AGENTS.md §"Build Flags").

### 6.4 Kernel-launch plumbing

`main.cu` `launch_pipeline` acquires two additional pieces:

1. `cudaMemcpyToSymbolAsync(c_campaign, &host_constants, sizeof(CampaignConstants), 0, cudaMemcpyHostToDevice)` — once per campaign.
2. Allocate `d_group_flags[num_tiles_per_burst * 32]` alongside existing `d_parent` etc.
3. Pass `d_inputs` and `d_group_flags` to K4 and K5.

No change to K1/K2/K3 signatures.

---

## 7. Compositor

### 7.1 Core structures (preserved from sqrt-36/40)

- `parent_[global_group_id]` — inter-tile UF over global group IDs.
- `root_reach_[root_id]` — bitmask `REACH_INNER | REACH_OUTER` per UF root. When both bits coincide, `spanning_detected_` flips `true` (preserved incremental spanning check, AGENTS.md §"Bug Fixes 2026-04-13" #3).
- Global group ID scheme: `global_group_id(tile_index, local_label) = tile_index * 128 + local_label`. Requires `tile_index < 2³²/128 = 33.5 M` — fine at project scale (~10 M active tiles).

### 7.2 Per-tile ingest (radically simplified)

The legacy `collect_inner_boundary` / `collect_outer_boundary{_ingest}` block (~750 lines of staircase geometry in `compositor.cpp:495–821`) is **replaced** by a flag-driven pass:

```cpp
void Compositor::mark_tile_ports(const Grid& g, const TileCoord& coord, const TileOp& op) {
  if (op.tile_flags & OVERFLOW_BIT) { mark_tile_as_spanning_conservative(coord); return; }
  if (op.tile_flags & EMPTY_BIT)     return;
  const int offsets[4] = { 0, op.n[0], op.n[0]+op.n[1], op.n[0]+op.n[1]+op.n[2] };
  for (int f = 0; f < 4; ++f) {
    for (int p = 0; p < op.n[f]; ++p) {
      uint8_t g = op.face_groups[offsets[f] + p];
      int64_t gid = global_group_id(coord_index(coord), g);
      if (bit_test(op.inner_flags, g - 1)) mark_inner(gid);
      if (bit_test(op.outer_flags, g - 1)) mark_outer(gid);
    }
  }
}
```

Zero geometry. Zero side-exposed heuristics. The canonical math (Exit Lemma + Theorem 11) guarantees every geo_I / geo_O prime is represented through a port of its UF component.

### 7.3 Cross-tile stitching (Lemma 4, positional)

Under snapped grid, face-adjacent tiles share identical ordered ports. Stitching is strict ordinal matching:

```cpp
// Within-tower:  T_{i,j}.face_O  <->  T_{i,j+1}.face_I
void match_io(const TileOp& A, const TileOp& B, int A_idx, int B_idx) {
  CHECK(A.n[FACE_O] == B.n[FACE_I]);  // Lemma 4 strict equality
  for (int p = 0; p < A.n[FACE_O]; ++p) {
    uint8_t g_A = A.face_groups[face_off(A, FACE_O) + p];
    uint8_t g_B = B.face_groups[face_off(B, FACE_I) + p];
    union_groups(global_group_id(A_idx, g_A), global_group_id(B_idx, g_B));
  }
}

// Between-tower: T_{i,j}.face_R  <->  T_{i+1,j}.face_L
void match_lr(const TileOp& A, const TileOp& B, int A_idx, int B_idx) {
  CHECK(A.n[FACE_R] == B.n[FACE_L]);  // Lemma 4
  for (int p = 0; p < A.n[FACE_R]; ++p) {
    uint8_t g_A = A.face_groups[face_off(A, FACE_R) + p];
    uint8_t g_B = B.face_groups[face_off(B, FACE_L) + p];
    union_groups(global_group_id(A_idx, g_A), global_group_id(B_idx, g_B));
  }
}
```

**No `h1`. No delta. No `q/f` decomposition.** Equivalent to deleting the coordinate-space bridging in legacy `match_lr_with_previous` (`compositor.cpp:393–493`, ~100 lines). Port-count mismatch = bug; panic in debug, log-and-treat-as-spanning in release.

### 7.4 Verdict

`spanning_detected_` is monotone. After the last burst, compositor emits `MOAT` if still false, else `SPANNING`. Early-exit on `true` preserved.

### 7.5 API (frozen surface)

```cpp
class Compositor {
public:
  void init(const Grid& g);
  void ingest_column(int i, const TileOp* column_tileops);
    // column_tileops has (j_high(i) - j_low(i) + 1) contiguous TileOp records.
  bool has_spanning() const;
  Verdict finalize();   // SPANNING | MOAT
};
```

---

## 8. I/O protocol between modules

### 8.1 Host ↔ CUDA worker (per burst, bi-directional pipe)

**Host → CUDA (stdin):**

```
uint32  magic   = 0x4D4F4143   // "CAMP"
uint32  version = 3
uint32  num_tiles
uint32  tiles_payload_bytes     // = num_tiles * sizeof(TileInput) = num_tiles * 24
uint8[] tiles                   // num_tiles * 24 bytes of TileInput (SoA-friendly packed)
```

**CUDA → Host (stdout):**

```
uint32  magic     = 0x544F504F  // "TOPO"
uint32  version   = 3
uint32  num_tiles
uint32  payload_bytes           // = num_tiles * 256
uint8[] tileops                 // num_tiles * 256 bytes, one TileOp per tile, in input order
```

Burst size: `STREAM_CHUNK_SIZE = 200 K` tiles (AGENTS.md §"Operational Learnings" point 9). Buffer: ~10 GB on GPU. No separate `prime_count` prefix (dumps recompute from `n[]` on demand).

### 8.2 CUDA worker internal buffers

All flat global-memory allocations keyed by `tile_idx`:

- `d_inputs[num_tiles_per_burst * sizeof(TileInput)]` — pinned-host staged.
- `d_cand_list[num_tiles * 6144 * 4 B]` — K1 output / K2 input.
- `d_bitmap[num_tiles * BITMAP_WORDS * 4 B]` — K2 output / K3, K4 input.
- `d_prime_pos[num_tiles * MAX_PRIMES_GPU * 4 B]` — K3 output.
- `d_parent[num_tiles * MAX_PRIMES_GPU * 2 B]` — K4 output.
- `d_group_flags[num_tiles * 32 B]` — **new**; K4 output / K5 input.
- `d_tileops[num_tiles * 256 B]` — K5 output.

**Memory budget per burst (200 K tiles, K = 40):** ~10 GB (dominated by `d_cand_list` at 5 GB and `d_bitmap` at 1.9 GB). `d_group_flags` adds only 6.4 MB — negligible. Fits within AGENTS.md §"STREAM_CHUNK_SIZE" budget.

Sync: `cudaDeviceSynchronize()` after K5; `cudaMemcpyAsync(device→host)` of `d_tileops` to pinned host buffer; single `write()` to stdout.

### 8.3 Host ↔ Compositor (in-process)

```cpp
// After a burst completes and tileops land in host memory:
for (int i : columns_in_this_burst) {
  const TileOp* column_ptr = host_tileops + tower_offset[i - i_min] * 256;
  compositor.ingest_column(i, column_ptr);
  if (compositor.has_spanning()) { abort_remaining_bursts(); break; }
}
```

One `ingest_column` per column. Synchronous. Single-threaded compositor.

### 8.4 Double-buffered overlap

Preserve legacy two-stream pattern (burst `b` computing on GPU while compositor processes burst `b − 1`). See AGENTS.md §"Operational Learnings" point 10 — this is the remaining win after the compositor simplification in §7 closes most of the 14% overhead gap.

---

## 9. Reflection closure (Theorem 12)

**No runtime code.** The pipeline runs on the canonical octant `R = { (x, y) ∈ ℤ² : x ≥ 0, y ≥ x }`. Theorem 12 (canonical doc §"Cross-octant symmetry closure") guarantees the octant verdict equals the full-annulus verdict via `D₄` symmetry. Side-exposed faces (`face_L` at `i = i_min`, `face_R` at `i = i_max`) are not stitched to anything; any G_full path that would cross them folds back into an `R`-path by the Adjacent-Octants + Monotone-Reflection construction. See `docs/supportive/2026-04-20-codex-axis-tower-analysis.md`.

Single defense-in-depth assertion at campaign init: `R_inner > √(2·K)` (Adjacent Octants Lemma). Project deployment trivially satisfies.

---

## 10. Overflow handling

Two overflow modes on the hot path:

1. **Port-count overflow** (`sum(n) > 192`): set `tile_flags |= OVERFLOW_BIT`, emit zeros in `face_groups` / `inner_flags` / `outer_flags`.
2. **Group-count overflow** (`max_label > 128`): same.

Compositor's response (`mark_tile_as_spanning_conservative`): treat as having all ports marked both `inner` and `outer` (forces its root to `REACH_BOTH`, producing a SPANNING-biased false positive). Safe (no false MOAT), may produce false SPANNING. **Note:** this triggers the incremental spanning check immediately — a single overflowed tile short-circuits the entire campaign to `SPANNING`. Monitor overflow rates closely; if observed rate exceeds 0.1% at project scale, escalate to the extended-format re-encode path.

**Rate / watchpoint.** Observed v3 overflow rate: 0 of 600 sampled tiles at 256 B. v1 `K = 36` observed 22% overflow under dual 128/256 B dispatch; v3 single-format 256 B is materially less stressed. Production monitor: if deployed overflow rate exceeds 0.1%, investigate sizing-study assumptions (density, connectivity, R regime not covered by sample) and escalate to the extended-format re-encode path. See `docs/supportive/2026-04-20-codex-tileop-sizing-study.md`.

**Deferred escalation path.** The existing `ExtendedTileSideTable` wiring (`grid.h:39–45`, `compositor.cpp:get_tile_data/get_payload_budget`) is retained for a future 512 B extended format carrying 256 groups / 256 flag bits. Not implemented in this blueprint; defer until observed rate exceeds 0.1%.

---

## 11. Determinism & numerical precision

- **Hot path is integer.** i64 for coordinates and norms at `R ≤ 10⁹`. i128 for the squared-epsilon step in geo tests (pre-filter cuts all but candidate-band primes; i128 is defense-in-depth).
- **Integer `√` only at grid-build time** (Newton or f64 ± 1 correction). Never on the per-tile hot path.
- **UF determinism.** Smaller-root-wins tiebreaker in both K4 atomic UF (`kernel_uf.cu:57–75`) and compositor `parent_` (unchanged).
- **Port-enumeration determinism.** Canonical lex `(h, p⊥)`; stable secondary sort by `(p⊥, h)`. Ties broken deterministically.
- **Encode order.** Face order `I → O → L → R`; within-face by port ordinal. Byte-for-byte deterministic.
- **CPU ↔ GPU parity.** `tile-cpp` reference and `tile_cuda_multi_kernel` produce bit-identical 256 B TileOps. Gated by restored `dump_tileops.cpp` + `compare_tileops.py` (parameterized on `TILEOP_SIZE = 256`, not hardcoded).

---

## 12. Testing & validation

**Layer 1 — unit parity (local):**
- `tile-cpp/tests/dump_tileops.cpp` — generate TileOps for fixed coord grid; byte-hash.
- `compare_tileops.py` — CPU vs GPU parity on ~1K tiles at `R ≥ 800 M`, K ∈ {36, 40}.
- `build/bz_check.py` — BZ reconciliation; build fails if BZ non-empty.

**Layer 2 — invariants (local):**
- `tests/test_snapped_grid.cpp` — I1 contiguity, I2 bounded shift, I4 empirical diagonal scan.
- `tests/test_geo_tests.cpp` — norm-form integer test; i128 boundary cases at large R; pre-filter correctness.

**Layer 3 — integration (Jetson or 4090):**
- Small-scale at `R = 850 M, K = 40, ~1 M tiles`. Expected: known `SPANNING` / `MOAT` verdict matching `artifacts/2026-04-14-sweep4/sweep4-results.jsonl`.
- Side-by-side vs legacy campaign-sqrt-40 at fixed R values from the sweep. 100% verdict agreement required.

**Layer 4 — regression:**
- `scripts/replay_sweep4.sh` — re-run the 47-R-value sweep; verdict-identical to legacy.

**Layer 5 — adversarial edge cases:**
- Empty-halo tile (BACKLOG **B7**): confirm `EMPTY_BIT`, zero ports, no bridge contribution.
- Tower-closing column (`tower_height ≤ 1`) near `x ≈ R_outer/√2`: I4 empirical scan passes.
- Port-count overflow: confirm OVERFLOW_BIT emit + conservative-SPANNING compositor response.

---

## 13. Performance targets

Inherited from sqrt-40 at **155 K tiles/s on 4090** (AGENTS.md §"Performance Record"). The blueprint preserves all proven CUDA tuning (288 threads, `maxrregcount=44`, 2-round MR, 10 K sieve limit) and adds:

- **K4 geo-test overhead:** est. `< 5%` K4 slowdown, `< 1.5%` campaign slowdown.
- **K5 face-strip UF (replacing 1-D greedy):** neutral (face strips tiny).
- **Compositor simplification:** **net speedup.** 750 lines of geometry → 20 lines of flag read. Compositor was the remaining 14% bottleneck (AGENTS.md §"Operational Learnings" point 10). Blueprint closes most of that gap.

**Target:** `≥ 148 K tiles/s effective` (up from `134 K` current effective) on 4090, `≥ 3 300 tiles/s` on Jetson Orin.

No new micro-optimization. Pipeline is INT32-bound on the hot kernels (K1, K2, K4); further wins require algorithmic changes (faster MR, reduced sieve iterations) or hardware (H100).

---

## 14. Implementation delta plan

Files changed, grouped by module. Each line ≈ one commit of work.

### 14.1 `tile-cpp/` (C++ reference)
- `include/constants.h` — bump `TILEOP_SIZE = 256`; add `C = floor_isqrt(K_SQ)`; define `R_inner/R_outer/prefilter bounds` from CLI.
- `include/types.h` — new `TileOp` layout per §5.2.
- `src/face_extract.cpp` — replace 1-D greedy scan with face-strip UF port clustering.
- `src/prune.cpp` — **delete.** No dead-end pruning.
- `src/encode.cpp` — new 256 B encoder: byte-per-port `face_groups` + bit-packed `inner_flags` / `outer_flags`.
- `src/geo_tests.cpp` — **new file**: `is_inner_prime(norm_sq)`, `is_outer_prime(norm_sq)`, `per_uf_flags(parent[], prime_coords[])`.
- `src/process_tile.cpp` — call `geo_tests`, updated `face_extract`, prune-free flow.

### 14.2 `tile_cuda_multi_kernel/` (CUDA worker)
- `include/gpu_constants.cuh` — add `CampaignConstants`, `__constant__ c_campaign`, `TILEOP_SIZE = 256`.
- `include/gpu_types.cuh` — new `TileOp` mirror of CPU layout.
- `src/kernel_uf.cu` — per-prime geo test after path compression; accumulate `smem_flags_by_root`; emit to `d_group_flags`.
- `src/kernel_face_encode.cu` — replace 1-D clustering with face-strip UF; delete dead-end prune; add flag pack to 256 B encode.
- `src/main.cu` — upload `CampaignConstants`; pass `d_inputs` + `d_group_flags` to K4 / K5; allocate new buffer.

### 14.3 `tiles-compositor/` (Compositor + runner)
- `include/types.h` — new `TileOp` struct; `TILEOP_SIZE = 256`.
- `include/grid.h` — new `Grid` struct (snapped); tightened `is_tile_dead`; `TileCoord{i, j, a_lo, b_lo}`.
- `include/compositor.h` — new `ingest_column` API.
- `src/grid.cpp` — **rewrite** as snapped-grid enumerator; integer arithmetic; I1/I2/I4 assertions.
- `src/compositor.cpp` — **gut**: delete `collect_inner_boundary`, `collect_outer_boundary{_ingest}`, `match_lr_with_previous`, `match_io_within_tower` (~750 lines). Replace with `mark_tile_ports` + positional `match_io` / `match_lr` (~50 lines net).
- `src/tileop_parse.cpp` — new 256 B parser.
- `src/campaign.cpp` — BZ check; upload `CampaignConstants`; new ingest API; accept `--r-inner`, `--r-outer` CLI flags.

### 14.4 Tests
- Restore `tile-cpp/tests/dump_tileops.cpp` at 256 B; parameterize struct format on `TILEOP_SIZE`.
- Restore `tile-cpp/tests/compare_tileops.py` likewise.
- New `tests/test_geo_tests.cpp`.
- New `tests/test_snapped_grid.cpp`.

### 14.5 Scripts / ops
- `build/bz_check.py` — BZ reconciliation (mpmath 50-digit). Script is drafted in canonical math doc §Tower tiling.
- `scripts/replay_sweep4.sh` — regression replay of `artifacts/2026-04-14-sweep4/`.

**Net code size:** est. `+800` lines new, `−1000` lines deleted (mostly compositor staircase + `prune.cpp`). Net decrease.

---

## 15. Open decisions & cross-references

### 15.1 Locked by 2026-04-20 empirical / analytic pass

1. **TileOp wire size.** `256 B` single format. Sizing study found 0 overflow in 600 sampled v3 tiles with large headroom (§5, §10).
2. **Axis-adjacent towers.** Octant-local UF only. No reflection-aware K4 edges; no side-exposed compositor stitching (§4, §9).
3. **Canonical octant.** Pipeline orientation is `x ≥ 0, y ≥ x`; folded tooling must reconcile back to this orientation (§4).
4. **K4 halo clipping.** Enumerate and union octant primes only; off-octant halo primes are excluded from local DSU (§6.2).
5. **Overflow threshold.** Defer 512 B extended format until deployed overflow rate exceeds 0.1% (§10).

### 15.2 Decisions requiring user sign-off before coding

1. **Double-buffered async compositor.** Blueprint retains legacy double-buffer. If tighter GPU / compositor coupling is desired (async pipeline per AGENTS.md §"Operational Learnings" point 10), add as scope.
2. **Build parameterization.** Keep dual `build-k36/` `build-k40/` (current), or consolidate to runtime-switchable build? Current CMake supports both.

### 15.3 Tracked (non-blocking)

- `methodology/lemmas_v2/BACKLOG.md` items **B1–B10** (initial math-side pass).
- Codex xhigh auditor findings (dispatched 2026-04-19; merge into BACKLOG when report returns).

### 15.4 Deferred engineering decisions

- **Double-buffer compositor async:** closes remaining ~3% overhead after the compositor simplification. Not on critical path.
- **H100 target:** AGENTS.md §"Operational Learnings" point 8 — remaining wins require H100-class INT32 throughput. Defer.
- **ncu warp profiling (AGENTS.md §"Warp Profiling Investigation TODO"):** orthogonal to this blueprint; do independently.

---

*End of blueprint v3. Revisit after first computational campaign run.*
