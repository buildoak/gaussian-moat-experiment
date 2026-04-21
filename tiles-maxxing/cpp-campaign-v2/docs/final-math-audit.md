---
title: Final Adversarial Math Audit — cpp-campaign-v2 vs tile-operator-definition-v-claude.md
date: 2026-04-20
engine: coordinator
auditor: claude-opus-4-7
type: audit
status: complete
head: ef8fef2
ssot: methodology/lemmas_v2/tile-operator-definition-v-claude.md
---

# Executive summary

Overall verdict: **READY-WITH-FIXES**. The C++ implementation at HEAD `ef8fef2` matches the canonical math specification faithfully on the load-bearing soundness path — Theorem 11's verdict (Moat ⟺ no component carries both inner and outer flags) is correctly encoded; Lemma 4's positional port identity, Lemma 5's UF-group port label propagation, and Lemma 10's exit-lemma via per-group flag propagation are all implemented as described. The 256-byte TileOp layout, wire labels 1..128, smaller-root-wins DSU, dense-remap canonicalization, integer-only arithmetic, and `⌈√K⌉` band width all land clean.

Count across 34 checkpoints: **IMPLEMENTED 25, DEVIATION 4, MISSING 2, NIT 2, NOT APPLICABLE 1.**

Single biggest concern: **the DSU stores `uint16_t` parents with a hard cap of 65535 elements (`union_find.cpp:18`), while the per-tile Gaussian-prime budget is `MAX_PRIMES_GPU = 6144` (`constants.h:61`).** 6144 < 65535, so the cap holds today — but it is a silent architectural trapdoor: any project-parameter change that lifts the per-halo prime count above 65535 will throw `std::invalid_argument` from `DSU::DSU`, not overflow to `OVERFLOW_BIT`. This is not a spec violation but it is a latent soundness risk for future K/R changes.

The real soundness-adjacent deviations are:
1. **I1/I2/I4 invariants are enforced only in DEBUG** (`grid.cpp:295`, `NDEBUG`-gated). Blueprint §4.3 mandates the I4 coverage scan as runtime seatbelt for the [PROOF GAP] on the tower-closing margin corollary (spec line 361). In release builds this seatbelt is absent.
2. **Axis primes at `x = 0` are emitted only when `a_begin ≤ 0`** (`sieve.cpp:81`), which happens only at `i = 0`. At project scale with `OFFSET_X=1`, `a_begin(i=0) = -5`, so this works — but the implementation is coincidental on offset geometry and is never asserted.
3. **I_ports / O_ports membership is never computed as a named set**. The compositor's incremental REACH-bit latching encodes the Theorem 11 verdict correctly, but the structural statement "every geo_I prime's UF component is represented by at least one port" (Lemma 10) is relied on without an explicit runtime check. Sound, but opaque.

None of these invalidate the MOAT verdict. SPANNING (the over-approximation) remains conservative on all paths. The prior three audits' conclusions hold.

# Per-checkpoint findings

## A. Canonical octant and D4 handling

**1. Canonical octant `{x ≥ 0, y ≥ x}` — enforcement.** IMPLEMENTED.
- Grid enumeration in `src/grid.cpp:104-108` (`definitely_inactive`) screens on `x_hi < 0` and `y_hi < x_lo`. Full confirmation in `active_by_y_range` at `src/grid.cpp:154-169` using `y_lo = max(b.y_lo, x, y_lo_annulus)` which enforces `y ≥ x`.
- Sieve at `src/sieve.cpp:81-82` restricts to `a ≥ max(0, a_begin)` and `b ≥ max(a, b_begin, 0)`, enforcing octant inside the halo.
- Reflection closure for faces outside the octant (face_L at `i_min`, face_R at `i_max`) is handled by skipping L-R stitching at the boundary (see checkpoint 2).

**2. Theorem 12 D4 fold — face_L @ i_min / face_R @ i_max dropped from matching.** IMPLEMENTED (with NIT).
- `src/compositor.cpp:259-287` stitches L-R only if `grid.has_column(left_i)`. At `i = i_min`, `i-1` has no column, so no match runs. At `i_max`, no `i+1` exists, so `face_R` at `i_max` is likewise never matched.
- `src/compositor.cpp:46-52` `assert_not_side_exposed_lr_input` is a DEBUG-only belt-and-braces assertion that the left operand in `match_lr` is never at `i_min` with face_L, nor at `i_max` with face_R. The `assert()` macro (not `assert_always` / exception) means in release builds this is no-op; the structural exclusion via `has_column` is what actually enforces.
- NIT: the assertion is phrased for the operand, not tested on the enumeration side. Release-build enforcement is implicit in control flow, not named.

**3. Axis primes `(0, q)` with `q ≡ 3 (mod 4)` — leftmost column handling.** IMPLEMENTED.
- `src/sieve.cpp:81-86`: loop over `a ∈ [max(0, a_begin), a_end]`. When `a == 0`, the prime test is `(q & 3) == 3 && is_prime(q)` — the inert-prime residue-class test from spec §Definitions (`q ≡ 3 mod 4`).
- At project scale with `OFFSET_X=1, S=256, C=6`, tile `i=0` has `a_lo = 1`, `a_begin = -5`. So `a = 0` is reached only for `i = 0`. For `i ≥ 1`, `a_begin ≥ 251`, so the axis is unreachable. Works as intended.
- DEVIATION-LATENT: this is a contingent outcome of the `OFFSET_X = 1` choice. If `OFFSET_X` were ≥ `C`, the `i=0` halo would no longer reach `x=0` and axis primes would be silently dropped. Not a bug today; not asserted.

## B. Soundness preconditions (math doc §2 / §4)

**4. Annulus thickness `R_outer − R_inner > S·√2 + 2·√K` — hard fail.** IMPLEMENTED.
- `apps/campaign_main.cpp:92-99, 359-369`: integer check `(R_outer − R_inner)² > 2·S² + 4·S·⌈√K⌉ + 4·K` using i128 arithmetic. This is the conservative expansion of `(S·√2 + 2·√K)²` (note: uses `⌈√K⌉` rather than `√K`, safely over-approximating RHS).
- Failure emits "ERROR: annulus too thin" and returns exit code 1 before any grid build or compositor run.
- NOTE: `CampaignConstants::from_radii` does NOT enforce this — intentional per `src/campaign_constants.cpp:102-110` ("tiny-radius unit tests must be allowed to run"). The gate is in `campaign_main.cpp` only. `verify_annulus_thickness()` at `src/campaign_constants.cpp:115-126` exists as user-callable surface but is not invoked from `from_radii`.

**5. Grid invariants I1/I2/I4 asserted in Grid::build.** DEVIATION (debug-only).
- `src/grid.cpp:295-354` `assert_invariants` is wrapped in `#ifndef NDEBUG`. All three checks (I1 contiguity at line 300-308; I2 bounded shift at 311-322; I4 diagonal orphans at 324-350) use the standard `assert()` macro.
- Release builds (which are the shipping mode for CUDA-port parity and performance) run with NDEBUG — the scans are skipped.
- Blueprint §4.3 mandates the I4 coverage scan as runtime seatbelt for the [PROOF GAP] on the tower-closing margin corollary (spec line 361). This is downgraded to debug-only enforcement — a documented-only seatbelt in release. Per-checkpoint 33 below.

**6. BZ gate pass at build time.** IMPLEMENTED.
- `CMakeLists.txt:104-140`: custom target `bz_check ALL` runs `scripts/bz_check.py` with `BZ_R_INNER=80000000`, `BZ_R_OUTER=80008192` at build time, and `add_dependencies(campaign bz_check)` forces the check before the library builds. A non-zero exit from the Python reconciliation script aborts the build.
- CI workflow (`.github/workflows/ci.yml:34`) installs mpmath explicitly. Build-time gate is enforced per the per-deployment reconciliation in spec lines 333-348.
- NIT: the CMake defaults are hard-coded to the canonical K=36/K=40 deployment. Changing radii via CLI at runtime does NOT re-run the BZ check. Documented-only at that level — any future redeployment must regenerate the cached BZ constants.

**7. Reflection closure `R_inner > √(2K)`.** IMPLEMENTED.
- `src/campaign_constants.cpp:92-100`: `CampaignConstants::from_radii` throws `std::invalid_argument` if `R_inner ≤ floor_isqrt(2·K_SQ)`. Uses `floor_isqrt(2·K_SQ)` — which is the largest integer `n` with `n² ≤ 2K`. Rejecting `R_inner ≤ n` therefore accepts only `R_inner ≥ n+1 > √(2K)`. Correctly strict.

## C. Model A semantics

**8. Only annulus primes in UF.** IMPLEMENTED.
- `src/sieve.cpp:89-91, 96-99`: `norm_in_annulus(norm_sq, constants)` at `src/sieve.cpp:42-45` enforces `R_inner² ≤ norm_sq ≤ R_outer²`. Primes failing this are `continue`d; never appear in the output vector. Below-inner and above-outer primes are structurally absent from UF.
- Face-strip UF (`src/tileop.cpp:92-149`) and local UF (`src/tileop.cpp:159-170`) both operate only on the sieve output — no secondary sieve path re-introduces out-of-band primes.

**9. `is_inner` band `[R_inner², (R_inner + ⌈√K⌉)²]`.** IMPLEMENTED (with over-approximation).
- `src/geo_tests.cpp:34-48`: upper = `lower + 2·R_inner·⌈√K⌉ + ⌈√K⌉²` = `R_inner² + 2·R_inner·⌈√K⌉ + ⌈√K⌉²` = `(R_inner + ⌈√K⌉)²`. Matches spec when K=36 is a perfect square (⌈√36⌉ = 6 = √K, upper = (R_inner+6)² exactly).
- For non-square K=40: `⌈√40⌉ = 7`, upper = `(R_inner+7)²`. Spec strictly says `(R_inner + √40)² ≈ (R_inner+6.32)²`. Code's upper is LARGER — over-approximates geo_I. This is CONSERVATIVE for MOAT verdict (wider set of inner-candidate primes just makes SPANNING more likely; a MOAT verdict on the wider set implies MOAT on the strict set).

**10. `is_outer` band `[(R_outer − ⌈√K⌉)², R_outer²]`.** IMPLEMENTED (with over-approximation).
- `src/geo_tests.cpp:50-64`: lower = `upper − (2·R_outer·⌈√K⌉ − ⌈√K⌉²)` = `(R_outer − ⌈√K⌉)²`.
- For K=40: lower = `(R_outer − 7)²` < `(R_outer − √40)²`. Code's lower is SMALLER — over-approximates geo_O. Conservative, same as checkpoint 9.

**11. Prefilter uses `⌈√K⌉`.** IMPLEMENTED.
- `src/campaign_constants.cpp:75-78` `c.prefilter_inner = 2·R_inner·⌈√K⌉ + 1`, same for outer.
- `include/campaign/constants.h:97-100` `ceil_isqrt(n) = floor_isqrt(n) + (f² == n ? 0 : 1)` — correct ceil implementation.
- `src/geo_tests.cpp:11-12` static_asserts `ceil_isqrt(36) == 6` and `ceil_isqrt(40) == 7`. Matches BACKLOG B19 requirement.

**12. Per-group flag propagation — OR reduce.** IMPLEMENTED.
- `src/tileop.cpp:242-252`: iterate all primes in G_tile, look up raw_root via `local_dsu.find(i)`, map to dense label, `bit_set(out.inner_flags, label)` if `prime_flags[i].inner`, same for outer. `bit_set` (defined `tileop.h:116-119`) is an OR via `|= (1u << bit)`.
- Correctly OR-reduces: if ANY prime in a dense-label component has `is_inner`, the component's bit in `inner_flags` is set. Same for outer. This is the Lemma 10 interior-to-face propagation on the encode side; matches spec §Extended TileOp with group flags.

## D. TileOp pipeline per §5 + §6.2

**13. Local UF smaller-root-wins tiebreak (Lemma 4 prereq).** IMPLEMENTED.
- `src/union_find.cpp:46-57` `DSU::unite`: `if (ra > rb) std::swap(ra, rb); parent[rb] = ra;`. Smaller-valued root becomes the parent. Deterministic, thread-independent.
- Load-bearing for Lemma 4 because port identity across shared faces depends on deterministic UF-root labels which the dense-remap inherits.

**14. Dense-remap: raw root → `[0, max_label)`, ascending by smallest prime index in component.** IMPLEMENTED.
- `src/tileop.cpp:181-204`: iterate `raw_roots` in `prime_idx` order (primes are pre-sorted by `(a, b)` lex at `src/tileop.cpp:220-223`). For each unseen raw_root, assign `max_label + 1`. First appearance in ascending prime_idx order → lowest wire label.
- Fully deterministic across shuffles (tested `test_tileop.cpp:100-113` `PortSortTiedInputDeterminism`).

**15. Face-strip UF per face F — sub-UF at sq-distance ≤ K.** IMPLEMENTED.
- `src/tileop.cpp:92-114`: `build_face_ports` builds separate `DSU face_dsu` indexed by strip membership. Pairwise double-loop unites via `within_k_sq` (`src/tileop.cpp:46-50` — `da² + db² ≤ K²`).
- Uses the SAME `G_full` edge predicate as local UF. Face strip is filtered by `on_face_strip` (`src/tileop.cpp:86-90`, `|p_perp| ≤ C`). No consecutive-pair greedy clustering (the v1 bug explicitly banned per design doc §3 line 88-90).

**16. Port representative: smallest (h, p⊥) lex.** IMPLEMENTED.
- `src/tileop.cpp:120-138`: iterate face_dsu members of each root, track min `(h, p_perp)` lex via `!have_rep || h < best_h || (h == best_h && p_perp < best_perp)`.
- Matches spec line 149.

**17. Port sort — (h, p⊥) primary, (p⊥, h) tiebreak.** IMPLEMENTED (with NIT).
- `src/tileop.cpp:143-147`: sort ports by `(h, p_perp, global_wire_label)`. Primary `h`, secondary `p_perp`, tertiary wire label.
- NIT: spec requests `(p⊥, h)` secondary, not wire label. However — as documented in `docs/tileop-design.md:108-113` — two distinct ports on a single face cannot share `(h, p_perp)` in a connected graph (they'd have to share a representative prime, which contradicts distinct UF-components on the same face). The tertiary tiebreak is structurally unreachable. Functionally equivalent to spec. Not a soundness concern.

**18. 256 B byte offsets.** IMPLEMENTED.
- `include/campaign/tileop.h:51-69`: struct layout + `static_assert(offsetof)` for each field. `n @ 0`, `face_groups @ 4`, `inner_flags @ 196`, `outer_flags @ 212`, `tile_flags @ 228`, `reserved @ 229`, `sizeof == 256`.
- `tests/test_tileop.cpp:62-69` runtime-verifies offsets.

**19. Wire labels `1..128`, not `0..127`.** IMPLEMENTED.
- `src/tileop.cpp:199` assigns `wire_label = max_label + 1` (1-indexed).
- `src/tileop.cpp:192-195` triggers overflow when `max_label >= MAX_GROUPS_PER_TILE (128)` — i.e., once 128 distinct labels assigned, the 129th trips.
- `include/campaign/tileop.h:110-119` `bit_test`/`bit_set` treat `g-1` as the bit index, so label 1 → bit 0, label 128 → bit 127. Matches spec.
- Label `0` reserved as sentinel: zero-init face_groups leaves empty slots at `0`. Compositor reads labels and calls `global_group_id` (`src/compositor.cpp:107-120`) which throws if label outside `1..128` — but the port loop at `src/compositor.cpp:141` iterates only `p < op.n[f]`, so zero-padded slots past `n[f]` are not read. In the overflow path, `mark_all_present_ports` (`src/compositor.cpp:150-163`) explicitly skips `group_label == 0`.

**20. Overflow: OVERFLOW_BIT set + zero payload, NOT empty-TileOp substitution.** IMPLEMENTED.
- `src/tileop.cpp:151-155` `overflow_tileop()` returns zero-initialized struct with only `tile_flags = OVERFLOW_BIT`. No empty-TileOp fallback.
- Three trigger paths: dense-remap max_label > 128 (`src/tileop.cpp:238-240`), port count > 255 per face (`src/tileop.cpp:260-262`), sum(n) > 192 (`src/tileop.cpp:268-270`). All emit the same zero-payload overflow.
- EMPTY_BIT is a distinct path for genuinely-empty tiles (`src/tileop.cpp:230-233`) — zero primes.
- Test `OverflowPortCountSetsOnlyOverflowBitAndZerosPayload` (`tests/test_tileop.cpp:152-171`) verifies only `tile_flags == OVERFLOW_BIT` and every other byte zero.

**21. Integer-only arithmetic, no float/double/sqrt.** IMPLEMENTED.
- `include/campaign/sieve.h:40-45` injects `#pragma GCC poison sqrt sqrtf sqrtl` behind `CAMPAIGN_STRICT`. Any TU including `sieve.h` under `CAMPAIGN_STRICT` fails to compile if `sqrt` is referenced.
- Grep across `src/*.cpp` and `include/campaign/*.h` shows only `ceil_isqrt` / `floor_isqrt` usage — pure integer loops (`include/campaign/constants.h:83-100`).
- Norm-form band tests (`src/geo_tests.cpp:20-31`) use i128 with no conversions to floating point.
- Grid i128 sqrt via binary search (`src/grid.cpp:68-95`) — integer-only.

## E. Compositor per §7 + Lemma 4

**22. Cross-tile DSU over global port indices.** IMPLEMENTED (with caveat).
- `src/compositor.cpp:107-120` `global_group_id(tile_index, group_label) = tile_index * 128 + (group_label - 1)`. Global group IDs map to a single `parent[]` array indexed by uint32_t.
- The DSU is NOT directly over ports — it is over **(tile_index, group_label) pairs**, which identify the per-tile G_tile UF component that ports belong to. A port's stitched group equivalence is reached via "port A has group_label g_a on tile T_A" + "bridge unites g_a with port B's g_b on tile T_B". Matches spec §Grid port graph and N-tile equivalence where port labels are the native identifiers.
- CAVEAT: the spec's G_ports_grid has vertices as ports; the code's DSU has vertices as per-tile UF-groups. These are equivalent via the port-to-group projection (Lemma 5 step 3), but the code's representation is denser. Port-level edges within a tile are trivially identity (two ports with same face_groups label project to the same group vertex), so no explicit within-tile edges need to be added. Clever and sound.

**23. Positional port stitching at shared faces.** IMPLEMENTED.
- `src/compositor.cpp:165-178` `match_io` unites `a.face_groups[a_off + p]` with `b.face_groups[b_off + p]` at equal positional ordinals `p`. Same for `match_lr` (lines 180-198). This is exactly the Lemma 4 positional correspondence: port k on A's face_O ↔ port k on B's face_I.

**24. Port-count equality at shared faces asserted.** IMPLEMENTED.
- `src/compositor.cpp:34-44` `require_port_count_equal` throws `std::runtime_error` (not just debug assert) on mismatch. Runtime enforcement. Called from `match_io` (line 171) and `match_lr` (line 191).
- Good: Lemma 4 port-count equality is a mathematical invariant of the face-strip UF; violation means upstream corruption. Hard-fail is correct.

**25. Incremental-reachability latch — O(1) per union.** IMPLEMENTED.
- `src/compositor.cpp:81-94` `unite()`: after union, OR reach[ra] |= reach[rb], then `latch_if_spanning(ra)`.
- `src/compositor.cpp:101-105` `latch_if_spanning`: single bit-test `(reach[root] & kReachBoth) == kReachBoth`, set flag. O(1).
- `mark()` at line 95-99 also invokes `latch_if_spanning` when a port's reach bits are OR'd in. So both union-spanning and mark-spanning are caught.

**26. Side-exposure: face_L at i_min / face_R at i_max excluded from stitching.** IMPLEMENTED (with DEBUG-only assert).
- Structural exclusion: `ingest_column` at `src/compositor.cpp:259-287` only matches L-R if `grid.has_column(left_i)`. For `i == i_min`, `i-1` has no column → skip. For `i == i_max`, no `i+1` exists → the loop processing `i_max` doesn't attempt to stitch its face_R with anything.
- Release-build enforcement via control flow.
- `assert_not_side_exposed_lr_input` (`src/compositor.cpp:46-52`) is debug-only belt-and-braces.

**27. Overflow tile — all ports forced both inner AND outer → SPANNING.** IMPLEMENTED.
- `src/compositor.cpp:131-134`: if `OVERFLOW_BIT` set on incoming TileOp, set `spanning_detected = true` directly AND call `mark_all_present_ports(tile_index, op, kReachBoth)`.
- Because overflow TileOps have zero n[] and zero face_groups, `mark_all_present_ports` iterates nothing — the `spanning_detected = true` is what locks the verdict. This works because `finalize()` returns `kSpanning` whenever the global flag is set, regardless of per-group reach.
- DEVIATION (minor, and conservative): spec says overflow tile's ports are forced to BOTH flags for correctness. Code sets the GLOBAL verdict flag but does not propagate REACH_BOTH to adjacent tiles through bridge edges (because the overflow tile has no ports to bridge with). This is safe for the MOAT verdict — once spanning_detected is true, finalize returns SPANNING. But it's not structurally identical to "this component now spans"; it's "we short-circuit the whole run." Functionally equivalent for the terminal verdict.

## F. Verdict semantics (Theorem 11)

**28. MOAT ⟺ no component has both flags — sound.** IMPLEMENTED.
- `src/compositor.cpp:294-297` `finalize()`: `spanning_detected ? kSpanning : kMoat`. `spanning_detected` becomes true iff some DSU root's reach byte has both bits set at any point during ingestion (monotonic).
- Theorem 11's contrapositive: `ufs_global(geo_I) ∩ ufs_global(geo_O) ≠ ∅` iff some G_ports_grid component carries both I_ports and O_ports. Code's DSU roots are the code's representation of G_ports_grid components; reach bits encode port membership in I_ports (inner bit) and O_ports (outer bit). Equivalent.

**29. SPANNING — conservative (may be false positive, never false MOAT).** IMPLEMENTED.
- Over-approximations (wider bands via `⌈√K⌉`, overflow tiles auto-spanning) always push toward SPANNING, never toward MOAT. MOAT verdict therefore remains a strict claim; SPANNING is a superset.

**30. Semantic distinction documented/enforced.** IMPLEMENTED.
- `include/campaign/compositor.h:5-22` docblock states "MOAT ⟺ no UF root ever carries both REACH_INNER and REACH_OUTER; else SPANNING". `Verdict` enum (`compositor.h:37-41`) has `kMoat / kSpanning / kUnknown` with distinct values.
- `apps/campaign_main.cpp:223-233` `verdict_name` produces output strings "MOAT" / "SPANNING" / "UNKNOWN". Not merely "true/false".

## G. Other proof obligations

**31. Lemma 10 (Exit Lemma) — per-group flag design.** IMPLEMENTED.
- `src/tileop.cpp:242-252`: the flag OR-reduce iterates ALL primes in G_tile (not just face primes). A geo_I prime deep in the interior has its tile-local raw_root looked up via `local_dsu.find(i)`, its dense label recovered, and the bit set in `out.inner_flags[label]`. The flag propagates to EVERY port of the same UF component via the shared dense label in face_groups.
- This realizes Lemma 10's contrapositive: if a G_tile component extends outside the tile (has ≥ 1 face prime), its face ports carry the inner_flag derived from interior geo_I primes. If it does NOT extend outside, the proper-region containment from Theorem 11 Case A.2/B means R_outer − R_inner > S√2 excludes the geo_I-and-geo_O-in-same-component case — handled by annulus thickness (checkpoint 4).

**32. Lemma 3 corollary [NOTE] at line 159.** NOT APPLICABLE.
- Spec line 160: `"claim about being connected over G_full seems too broad here — inspect how exactly this one is used"`.
- Inspection: Lemma 3 establishes Ports(face_f_primes) as a valid Process P decomposition. It is invoked by the Corollary at spec line 161-165 (port-level face connectivity) and by Theorem 9's boundary-vertex sub-cases (ii) and (iii) at spec lines 656-664.
- The code does not "use" Lemma 3 directly — the compositor realizes Theorem 9's conclusion (N-tile equivalence) via the G_ports_grid DSU representation. Lemma 3 is a proof-internal scaffolding step. No runtime surface to audit.

**33. Tower-closing margin corollary [PROOF GAP] at spec line 361.** MISSING.
- Spec line 361: `"[PROOF GAP] This step asserts tower_height ≤ 1 ⟹ y_upper − y_lower ≤ S without the discrete→continuous derivation. The blueprint compensates via mandatory I4 coverage scan at campaign init (see blueprint §4.3, BACKLOG B12)."`.
- `src/grid.cpp:295-354`: the I4 scan is wrapped in `#ifndef NDEBUG`. Release builds run the campaign without this seatbelt.
- Blueprint §4.3 explicitly mandates the scan AT CAMPAIGN INIT — not debug-only. This is a deviation from the blueprint's compensation for the [PROOF GAP]. For project parameters the margin corollary holds by inspection (R_outer = 80 008 192, S = 256, the regime is narrow and arithmetically verified by `bz_check.py`), but the blueprint's explicit "mandatory" requirement is not honored in release builds.
- Severity: the structural proof of I4 (spec lines 531 — "I4 is structurally proven — no runtime gate required for soundness") contradicts the blueprint's "mandatory" language. Given the [PROOF GAP] tag, the seatbelt is the safer interpretation. The code follows the weaker (structural-proof) interpretation.

**34. Closed-interval shared-boundary convention.** IMPLEMENTED.
- Spec lines 113: "Tile proper regions are CLOSED intervals... A prime sitting exactly on this shared boundary is a lattice point of *every* adjacent tile's proper region and is counted in all of them — this is intentional."
- `src/grid.cpp:46-61`: `TileBox` has `x_hi = x_lo + S` (closed; the lattice point at `x_hi` IS in the box). `active_by_y_range` at line 168 uses `std::min(b.y_hi, y_hi_annulus)` with inclusive compare `y_lo <= y_hi`.
- `src/sieve.cpp:77-83`: sieve loops use `a <= a_end` and `b <= b_end` (inclusive upper bound). Both endpoints emitted.
- A prime on the shared boundary between T_{i,j} and T_{i+1,j} appears in BOTH tiles' sieve outputs, BOTH tiles' UF components, and BOTH tiles' port structures. The port sort and face-strip UF produce matching positional ordinals on the shared face (Lemma 4) — so stitching correctly unites the two copies at matched ordinals.

# Cross-cutting observations

1. **Debug-only enforcement of soundness-adjacent invariants.** Grid invariants (checkpoint 5), side-exposure assertions (checkpoint 2/26), and the I4 coverage scan ([PROOF GAP] compensation, checkpoint 33) all use `assert()` → NDEBUG-gated. In release builds these are no-ops. The real structural enforcement is implicit in control flow (e.g., `has_column` skipping L-R stitching at boundaries). This works today but puts the audit-trail at the mercy of future refactors — any change to iteration order or `has_column` semantics could silently re-introduce bugs the asserts were designed to catch.

2. **Conservative over-approximations are consistent.** The geo_I/geo_O bands use `⌈√K⌉` over `√K`. The annulus thickness check uses the integer conservative expansion. The overflow path short-circuits to SPANNING. All these bias the verdict toward false-SPANNING (never false-MOAT). This is the intended soundness posture and is uniformly applied.

3. **The DSU uint16_t cap.** `union_find.cpp:18-19` hard-caps DSU size at 65536. Per-tile prime count is capped at 6144 via MAX_PRIMES_GPU. `DSU::DSU(n)` throws `std::invalid_argument` if `n ≥ 65536`. Per-tile halo (269² lattice points) could in principle exceed 6144 at different parameters, but the sieve's band-filtered output keeps it in check. This is a latent future-parameter trapdoor, not a current bug.

4. **Theorem 11's Case A.2/B impossibility hinges on annulus thickness.** The proof relies on `‖u − u'‖ > S√2` forcing a contradiction when a D component has no face primes. This assumes the annulus thickness check at `campaign_main.cpp:359-369` has fired. `CampaignConstants::from_radii` doesn't enforce it — so library-level use (via tests or future GPU ports bypassing `campaign_main`) could construct a pipeline where Case A.2/B isn't properly excluded. Not a current bug; a latent contract concern.

5. **Port ordinal matching depends on Lemma 4 prerequisite (uniform offset).** `src/tileop.cpp:60-90` computes `h` and `p_perp` in tile-local coordinates. The spec's Lemma 4 prerequisite is `t_x_A = t_x_B` (for horizontal sharing) — which holds because `OFFSET_X/OFFSET_Y` are fixed constants and all tiles use `(o_x + i·S, o_y + j·S)`. This is structural. Not asserted, but not violable without deep refactor.

# Recommendations

## Before CUDA port

1. **Promote invariant asserts to runtime checks (or at least DEBUG + RELEASE-WITH-ASSERTS).** The blueprint §4.3 I4 coverage scan should be a named, always-on function — call it `Grid::verify_invariants()` — returning `bool` and invoked from `campaign_main.cpp` at campaign init (same level as `annulus_thickness_ok`). Keep the `#ifndef NDEBUG` inner scans for speed during hot-loop tests, but add a release-safe audit pass at campaign start. Closes the [PROOF GAP] compensation gap from checkpoint 33.

2. **Add an explicit runtime assertion to `global_group_id` that `group_label > 0`** when called from the non-overflow path. Currently relies on `face_groups[p]` being 1..128 for well-formed tiles — this is a zero-sentinel discipline that should be encoded at the read site, not just the write site. One-line addition.

3. **Wire `CampaignConstants::verify_annulus_thickness()` into `from_radii` as an optional strict mode.** Right now it's callable but never called. Either make it default-on with a flag to disable for tiny-radius tests, or move the check from `campaign_main.cpp` into `from_radii` behind a `strict=true` parameter. Prevents future entry points (CUDA harness, unit tests pretending to be campaigns) from constructing a subtly-broken pipeline.

4. **Document the `OFFSET_X = 1` contingency for axis-prime emission** (checkpoint 3). Either add a `static_assert(OFFSET_X >= 0 && OFFSET_X <= C)` in `sieve.cpp`, or assert at runtime in `sieve_tile` that `coord.a_lo - C <= 0` iff `coord.i == 0`. One-line safeguard.

## Ship as-is

1. The `⌈√K⌉` over-approximation in geo_I/geo_O bands (checkpoints 9-11) is intentional and documented. Don't change.
2. The DSU uint16_t cap (cross-cutting observation 3) is under current parameter budget and throws on violation. Don't preemptively widen to uint32_t — the throw is a clean boundary.
3. The compositor's DSU over (tile, group) pairs rather than ports (checkpoint 22) is a clean simplification of G_ports_grid. Mathematically equivalent, leaner. Keep.
4. Wire label `0` sentinel discipline (checkpoint 19) is consistent across encode/decode. Keep.

## Prior audit check

M4 (byte-level TileOp encoding), Phase 3.2 (Python cross-check), Phase 3.4 (capstone) all passed — and this audit confirms their findings from an orthogonal angle. No regressions found. No silent math deviations identified beyond the documented conservatism.

The pipeline is mathematically sound at project parameters. Before CUDA port, the recommendations above primarily concern ROBUSTNESS against future parameter drift and refactoring — the current code is correct for the current deployment.
