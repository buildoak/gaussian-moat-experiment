# Independent Math-Spec Audit — cpp-campaign-v2

**Auditor:** Opus 4.7 (blinded)
**Date:** 2026-04-21
**Spec under audit:** `/Users/otonashi/thinking/building/gaussian-moat-cuda/methodology/lemmas_v2/tile-operator-definition-v-claude.md`
**Code under audit:** `/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/cpp-campaign-v2/`

---

## Exec Summary

The implementation faithfully mirrors the spec's core soundness chain (Lemma 4 positional Ports equality, Lemma 5/Theorem 9 port-graph lift, Theorem 11 flag machinery, Theorem 12 reflection-closure via side-exposure gating). Soundness against false MOAT verdicts appears preserved at project parameters (R=8e7, K∈{36,40}, S=256) because the one narrow spec-vs-code gap (driver annulus-thickness check) fires outside the real-valued required bound only within a ~3-unit band that project delta=8192 clears by three orders of magnitude. **Zero BLOCKERs**, **2 MAJORs**, **5 MINORs**, **3 NITs**, and several CUDA-port gotchas flagged for the port team. CUDA-port readiness: **green with caveats** — the 256-byte TileOp wire format, smaller-root DSU tie-break, positional-ordinal stitching, and hashing surfaces are all locked and deterministic; the port team needs to plan around the O(N²) within-tile pairwise unions (hot loop, currently CPU-OpenMP-fanned-out) and the variable-branch sieve.

---

## Findings by severity

### MAJOR

#### M1 — Driver annulus-thickness check is weaker than spec; uses `√K` where `√(2K)` is required

- **File:** `apps/campaign_main.cpp:85-99, 359-369`
- **Spec quote (line 352):** *"R_outer − R_inner > S√2 + 2√K."* Squaring: `delta² > (S√2 + 2√K)² = 2S² + 4S·√(2K) + 4K`.
- **Code:**
  ```cpp
  std::uint64_t annulus_thickness_rhs(std::uint32_t k_sq) {
    ...
    return 2ULL * s * s + 4ULL * s * ceil_sqrt_k + 4ULL * k_sq;
  }
  ```
  Uses `4·S·ceil_isqrt(K)` where the correct cross term is `4·S·√(2K)`. At K=36, `ceil_isqrt(36)=6` but `√(2·36)=√72≈8.485`; the driver's RHS is ~18% too small.
- **Why it matters:** For K=36, S=256, spec requires `delta > 374`; driver's surrogate admits `delta > 370.6`. A deployment with `R_outer − R_inner ∈ [371, 374]` passes the driver gate but fails the spec precondition that Theorem 11 Case A.2 relies on — `‖u − u'‖ ≤ S√2` contradiction. At project `delta = 8192`, irrelevant; at close-to-boundary edge parameters, a false **MOAT** could be reported for a genuinely spanning configuration. Note: `CampaignConstants::verify_annulus_thickness()` uses a stronger surrogate (`2S + 2·ceil√K`) but the driver doesn't call it.
- **Fix:** Replace `ceil_sqrt_k` with `ceil_isqrt(2 * k_sq)` in `annulus_thickness_rhs`, OR delete the bespoke driver check and call `constants.verify_annulus_thickness()` (the stronger conservative surrogate already implemented).

#### M2 — OpenMP fan-out can throw `std::overflow_error` / `std::logic_error` / `std::invalid_argument` from parallel workers; uncaught C++ exceptions propagating out of an `omp parallel for` region are UB per the OpenMP spec

- **File:** `apps/campaign_main.cpp:419-424`, in concert with `src/sieve.cpp:47-52` (`checked_norm_sq` throws `std::overflow_error`), `src/sieve.cpp:64-70` (`packed_pos_for` throws `std::logic_error`), `src/tileop.cpp:212` (`build_tileop_for_primes` throws `std::invalid_argument`), and `src/union_find.cpp:27-35` (DSU size cap throws).
- **Code:**
  ```cpp
  #pragma omp parallel for schedule(dynamic, 64)
  for (std::int64_t k = 0; k < ...; ++k) {
    tileops[idx] = campaign::process_tile(active_tiles[idx], constants, grid);
  }
  ```
- **Why it matters:** At project scale (R≤1e9, R²<2^62) none of these exceptions will actually be thrown. But if a CUDA-port QA script or a future "run at 2× scale" investigation passes `R_outer > 2^32`, the driver's own header check (`apps/campaign_main.cpp:346-352`) allows it; `CampaignConstants::from_radii` rejects `R_outer > 2^32`, but a caller bypassing that (e.g., a research harness using the Grid/TileOp primitives directly and then calling `process_tile` under OpenMP) will hit uncaught-throw-from-parallel-region UB. This is also a CUDA-port hazard: the port team will be translating these throw sites to some error-code channel, and should know which sites are actually "cannot happen at project params" vs "load-bearing precondition."
- **Fix:** Wrap `process_tile` call in a per-iteration try/catch, store a per-thread error flag, and aggregate after the parallel region. Or promote the relevant preconditions to compile-time `static_assert`s where they reduce to compile-time constants.

---

### MINOR

#### m1 — Axis-prime coverage of Lemma 6 breaks under offset `(1, 1)` but pipeline remains operationally sound via face_L / Theorem 12

- **File:** `src/grid.cpp:191-220` (`find_i_min` starts at `i = 0`), `include/campaign/constants.h:54-55` (`OFFSET_X = OFFSET_Y = 1`), `src/sieve.cpp:96-113` (axis-prime emission into T_{0,j}'s halo).
- **Spec quote (line 581, Lemma 6 proof):** *"let i := ⌊(a − o_x)/S⌋ and j := ⌊(b − o_y)/S⌋, and take T = T_{i,j}. … (a, b) lies in T's proper region."*
- **Why it matters:** For an axis prime `(0, q)` with `q` rational prime ≡ 3 (mod 4) and `‖(0, q)‖² ∈ [R_inner², R_outer²]` — these exist in geo_I at project parameters (e.g., primes near `q ≈ 8e7`) — mathematical `⌊(0 − 1)/S⌋ = −1`, so the proper-region-carrying tile would be `T_{-1, j}`. The code hard-codes `i ≥ 0` in `find_i_min`, so `T_{-1, j}` is never active and Lemma 6's conclusion fails for these primes. Operationally, the sieve places `(0, q)` into `T_{0, j}`'s halo (col = −1 ∈ [−C, C]), so `(0, q)` is a face_L prime of `T_{0, j}` and its `inner_flag` is marked via the normal port machinery. Face_L at `i_min = 0` is side-exposed and legitimately excluded from L-R stitching per Theorem 12's reflection closure. Soundness holds at project params; the spec's Lemma 6 statement just doesn't literally cover the offset=(1,1) axis-prime case. This is a spec/code drift flagged in the backlog (`B1`) and addressed via the `OFFSET_X = 1` hack; fine as long as the drift is documented.
- **Fix:** Either (a) add a note in the spec acknowledging the o_x=1 offset and how axis primes are covered via halo + face_L + reflection closure, or (b) change spec Lemma 6 to read "proper OR halo" — the pipeline's actual guarantee.

#### m2 — `CampaignConstants::verify_annulus_thickness()` surrogate `2S + 2·ceil√K` is stronger than spec bound `S√2 + 2√K` but is only enforced when `strict=true` is passed; default `from_radii` call path does not enforce

- **File:** `src/campaign_constants.cpp:103-123`, `128-139`; called with `strict=false` at `apps/campaign_main.cpp:372-379`.
- **Why it matters:** `CampaignConstants::from_radii(...)` defaults to `strict=false`, so the library itself doesn't enforce annulus thickness. The enforcement lives in `campaign_main.cpp`'s bespoke check (see M1). Alternative entry points (tests, a future CUDA harness, integration scripts) that instantiate `CampaignConstants` without calling through `campaign_main` can construct pipelines with too-thin annuli without triggering any gate. The header comment at `include/campaign/campaign_constants.h:97-98` acknowledges this explicitly: *"Any new entry point that produces a real verdict SHOULD use strict=true."* That's a convention, not a gate.
- **Fix:** Flip the default to `strict=true` and explicitly pass `strict=false` from tests that need tiny-radius construction. Or add `verify_annulus_thickness()` as a callable-from-CUDA static method on `CampaignConstants` and require CUDA-port code to call it.

#### m3 — `find_tower`'s `j_low` descent is bounded by `> 0`; misses active tiles at `j < 0` under small-radius configurations

- **File:** `src/grid.cpp:281-287`
- **Code:**
  ```cpp
  while (j_low > 0 && is_active_tile(i, j_low - 1, g)) --j_low;
  ```
- **Why it matters:** At project scale `j_low ≈ 312500` so this is a no-op. At tiny-radius test configurations (e.g., `R_inner=10000`, `R_outer=10032`) with offset `(1,1)`, `T_{i, -1}` might have an active lattice point in its proper region at `(x, 0)` with `y = 0, x ≤ R_outer`. The loop stops at `j_low = 0` and silently excludes `T_{i, -1}`. This is a completeness bug (would-be-active tiles excluded), not a soundness bug (the tiles that are included are all genuinely active). For the octant `y ≥ x ≥ 0`, `T_{i, -1}` with `j = −1` has `y_lo = o_y + (−1)·S = 1 − S = −255` and `y_hi = 1`. Octant-satisfying lattice points need `y ≥ x ≥ 0`; at `j = −1` these live in `y ∈ [0, 1]`, `x ∈ [1, 1]` → `(1, 1)` only, which has `‖(1,1)‖² = 2` — never in any realistic project annulus. So this is **practically harmless at current parameters**, but the bound is incorrect in principle.
- **Fix:** Remove the `j_low > 0` guard, or justify it with a static assertion that project parameters preclude `j_low < 1`.

#### m4 — `is_inner_prime` / `is_outer_prime` take `std::int64_t norm_sq` but `Prime::norm_sq` is `std::uint64_t`; silent narrowing at `R_outer` > 2^31.5

- **File:** `include/campaign/geo_tests.h:27-33`, `src/geo_tests.cpp:34-64`, `src/process_tile.cpp:22-28`
- **Code:**
  ```cpp
  const auto norm_sq = static_cast<std::int64_t>(prime.norm_sq);
  prime_flags.push_back(internal::PrimeGeoFlags{
      is_inner_prime(norm_sq, constants),
      is_outer_prime(norm_sq, constants),
  });
  ```
- **Why it matters:** At project scale `R_outer² ≈ 6.4e15 < 2^53 < 2^63` so casting `uint64_t → int64_t` is always value-preserving. For hypothetical deployments with `R_outer > 2^31.5 ≈ 3.04e9`, `R_outer² > 2^63` and the cast yields a negative int64; `is_inner_prime` / `is_outer_prime` return `false` for `norm_sq < 0`, silently mis-classifying every such prime as neither inner nor outer — **false MOAT risk at >2^31.5 radius scale**. `CampaignConstants::from_radii` rejects `R_outer > 2^32` at construction so the driver gate catches this; still, consumer-surface type mismatch is a latent hazard.
- **Fix:** Change signatures to `std::uint64_t norm_sq`; drop the cast in `process_tile.cpp`; drop the `if (norm_sq < 0) return false;` guard.

#### m5 — Driver's `annulus_thickness_ok` uses `unsigned __int128` for `delta²` but `annulus_thickness_rhs` returns `std::uint64_t`; fine at project but a silent contract mismatch

- **File:** `apps/campaign_main.cpp:92-99`
- **Why it matters:** `2·S² + 4·S·ceil√K + 4·K` at project params is ~137360 — fits in `uint32_t`. But `lhs` is `unsigned __int128`, so the comparison `lhs > (unsigned __int128)(rhs_u64)` is well-defined. Works correctly; the only issue is that at S=65536 hypothetical scale, `2S² = 8.5e9` overflows `uint32_t` but fits in `uint64_t`; at S=2^32 scale, `2S²` overflows `uint64_t`. Not project-relevant; noted for CUDA-port type discipline.
- **Fix:** Promote `annulus_thickness_rhs` return type to `unsigned __int128` for consistency.

---

### NIT

#### n1 — `build_local_dsu` is O(n²) pairwise (and so is `build_face_ports`' face-strip DSU); a k-d-tree or simple grid-bucket would be O(n) at K_SQ≤40

- **File:** `src/tileop.cpp:159-170`, `src/tileop.cpp:92-114`
- **Why it matters:** Per-tile prime count n ~ 200-600 at project parameters (MAX_PRIMES_GPU = 6144 headroom); n² = 40K-360K iterations per tile × millions of tiles. This is fine on CPU with OpenMP, but **this is the exact hot loop the CUDA port will need to re-architect**. GPU warps with O(n²) data-dependent-branch comparisons per tile will badly underperform a bucketed-neighbor scheme.
- **Fix:** For CPU reference: consider a single-column grid-bucket build that unions within a 2C+1 window. Definitely surface as a CUDA-port backlog item.

#### n2 — `DSU::roots()` is O(N·α(N)) but uses `std::find` inside the loop, making it O(N²) for unique roots

- **File:** `src/union_find.cpp:72-84`
- **Why it matters:** Called from `build_face_ports` via `face_dsu.roots()`. For face-strip DSU sizes of ~50-200, N² = 2500-40K. OK for CPU; another CUDA-port hot-spot to flag.
- **Fix:** Use a `std::unordered_set<int32_t>` or two-pass (sort, then unique) instead of linear search.

#### n3 — `Compositor::State::find` is iterative path compression with full compression, but the fallback mark-all path in `mark_all_present_ports` re-calls `find` inside a loop that already calls `global_group_id` once — minor redundant work

- **File:** `src/compositor.cpp:148-180`
- **Why it matters:** Nit.
- **Fix:** Refactor shared validation.

---

## CUDA-port gotchas (explicit)

These are spec-faithful but will bite the CUDA port team:

1. **Heap-in-hot-loop:** `sieve_tile` returns `std::vector<Prime>`, `build_local_dsu` allocates a DSU per tile, `build_face_ports` allocates per-face port vectors and face-strip DSUs. Every per-tile call does ~8-10 heap allocations. On GPU, each tile gets a thread block with shared memory budget — the port team must pre-allocate per-block scratch.
2. **O(N²) unions:** see n1.
3. **Unbounded but bounded recursion:** none — all `find` are iterative.
4. **Data-dependent divergence:** `sieve_tile`'s axis-vs-interior branch (`if (a == 0)`), `is_split_prime_norm`'s conditional Miller-Rabin call, `is_gaussian_prime_norm`'s square-check path — all are uniform per tile row but will divide warp-mates if the tile sits on the x=0 boundary. At i=0 (only), the first warp will straddle axis/off-axis primes.
5. **Thread-safety of `DSU::find`:** uses `mutable` path compression. Read-only concurrent calls are NOT safe — each DSU instance is single-thread. CUDA port needs lock-free or per-thread copies. (Currently safe because each CPU thread owns its own tile's DSU.)
6. **Static-state `is_prime`:** the FJ64 witness table lookup. Per-tile primality tests are embarrassingly parallel but the port needs to stage the witness table in `__constant__` memory; the `mr_witness_set_sha256` is already pinned for parity.
7. **Exception-based error flow:** see M2. GPU kernels cannot throw. Port team will need explicit error-code returns from `process_tile`.
8. **Host-only `std::sort`, `std::ofstream`, `nlohmann::json`:** snapshot writer is host-only by design; GPU-side only produces the tile buffer.

---

## Spec ↔ code mapping

| Spec invariant | Enforced in |
|---|---|
| Tile proper is closed `[o_x+iS, o_x+(i+1)S] × ...` with boundary sharing | `src/grid.cpp:53-61` (`tile_box`), `src/sieve.cpp:89-92` (halo bounds) |
| Collar `C = ⌊√K⌋` sufficiency | `include/campaign/constants.h:102-104`, enforced in `src/sieve.cpp` halo width |
| Snapped-grid alignment (t_x/t_y common offset) | `include/campaign/constants.h:54-55` (`OFFSET_X=OFFSET_Y=1`), enforced everywhere via `o_x + i*S` |
| Lemma 3 (Ports = face_f_strip UF components) | `src/tileop.cpp:92-114` — face_dsu unions within face strip |
| Lemma 4 (positional port equality across shared face) | Implicit via identical `(h, p⊥)` coord computation: `src/tileop.cpp:60-84` (`face_h`/`face_perp`). Tested only via `require_port_count_equal` in `src/compositor.cpp:34-44`. No coordinate-level test. |
| Lemma 5 (port's UF label = member's G_tile UF group) | `src/tileop.cpp:242-252` (writing `wire_label_by_raw_root[raw_root]` into both face_groups and inner/outer flags for each prime's root) |
| Theorem 9 (N-tile port/prime connectivity) | `src/compositor.cpp` global DSU on `tile*128 + (g-1)` IDs, with bridges from `match_io`/`match_lr` |
| Exit lemma (Lemma 10) | Implicit — ensured by correct face-strip collar width `|Δ| ≤ C` (see collar sufficiency above) |
| Theorem 11 (moat ⟺ no shared G_ports_grid component with both flags) | `src/compositor.cpp` `latch_if_spanning`, `reach` bit merging on `unite`/`mark` |
| Theorem 12 reflection closure (side-exposed faces legitimate) | `src/compositor.cpp:46-61` (`assert_not_side_exposed_lr_input`) + structural `has_column` filter |
| I0 (C < S/2) | At project params C=6, S=256 — asserted implicitly, no runtime check |
| I1 tower contiguity | `src/grid.cpp:310-320` in `check_shape_invariants` |
| I2 bounded boundary shift | `src/grid.cpp:322-343` |
| I4 no diagonal orphans (covers tower-closing) | `src/grid.cpp:345-373`, always-on via `verify_invariants()` called from `campaign_main.cpp:392-399` |
| Annulus thickness (R_outer − R_inner > S√2 + 2√K) | `apps/campaign_main.cpp:359-369` — **see M1 for surrogate bug** |
| Adjacent Octants Lemma precondition (R_inner > √(2K)) | `src/campaign_constants.cpp:91-101` — uses correct `floor_isqrt(2 * K_SQ)` bound |
| BZ reconciliation (geo_I_w = geo_I at project params) | Offline pre-commit (`bz_check.py`); not enforced at runtime. Blueprint §2 Per-deployment reconciliation codified in spec but not re-checked in code. |
| TileOp 256-byte layout | `include/campaign/tileop.h:52-69` static_asserts |
| DSU determinism (smaller-root-wins) | `src/union_find.cpp:55-66` and `src/compositor.cpp:90-102` |
| Canonical port enumeration (lex h, p⊥) | `src/tileop.cpp:143-147` — with tertiary label tiebreaker that cannot fire in practice |
| Dense-remap 1..128 labeling | `src/tileop.cpp:181-204`, overflow trigger at label 129 |
| OVERFLOW_BIT conservative SPANNING | `src/compositor.cpp:148-153` |
| MR witness-table pinning | `include/campaign/campaign_constants.h:28-29`, SHA-256 cross-checked in snapshot header |

---

## Soundness summary

**False MOAT possible?** No at project parameters, given:
- Correct Lemma 4 positional matching (verified via coordinate computation)
- Correct Exit lemma collar width (C = ⌊√K⌋)
- Correct Theorem 11 flag propagation (OR bits in DSU root)
- OVERFLOW tiles correctly trigger spanning latch
- Annulus thickness comfortably exceeds even the weaker driver bound at `delta=8192` vs `~374`.

**False SPANNING possible?** Yes, as allowed by Model A (annulus-only UF); below-R_inner / above-R_outer primes are muted. No spec divergence here.

**Latent risk at edge parameters:** M1 driver bound + m4 geo_test type mismatch together define a narrow parameter window (close to annulus-thickness boundary AND R > 2^31.5) where false MOAT becomes theoretically possible. No project configuration currently sits in that window.
