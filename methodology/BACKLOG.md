---
# Lemmas v2 BACKLOG

Non-blocking items surfaced while pressure-testing the canonical math source of truth at `tile-operator-definition-v-claude.md`. Format: dated entries; each item has a terse title and body. Revisit after the first computational campaign run.

---

## 2026-04-19 — initial pass (B1–B10)

**B1. Active-tile definition at the axis boundary.** With canonical `o_x = 0`, column `−1` can register as "active" via the shared `x = 0` edge primes under the closed-interval proper-region convention. Harmless in practice (face_L strip is empty, side-exposed side is handled by Theorem 12), but the `i_min = 0` claim needs tightening — refine "active" to exclude shared-boundary-only columns. Implementation note: as of the zero-offset alignment fix, code keeps `o_x = o_y = 0` and has an axis regression proving inert axis primes are proper-owned by column zero.

**B2. Lemma 3's inline `[NOTE]` about G_full breadth.** The connectivity claim in the Lemma 3 Corollary is stated "over G_full" but only `G_facestrip_f`-connectivity is used downstream. Simplification opportunity, not a bug.

**B3. "Single local connectivity group" strengthening consolidation.** The tightened definition (`V(G_local) = P^I_j` exactly, `G_local` connected) arrives mid-document; the informal earlier version still lingers in prose. Unify wording in crystallization.

**B4. Theorem 12 axis-ambiguity deterministic rule.** "Pick any valid `g_k` deterministically" needs a concrete tie-break rule for implementation — e.g., prefer `g_k = g_{k−1}` when `y_k` lies on the shared reflection axis.

**B5. Tower-closing margin corollary chain.** Valid but multi-step (`tower_height ≤ 1 → x ≥ R_outer/√2 − S → octant-min corner bound → (R_outer − S√2)² > R_inner²`). Compress into a single clean statement in paper crystallization.

**B6. BZ reconciliation is per-deployment.** Project parameters verified prime-free in BZ_I and BZ_O; any re-deployment must re-run the O(1) BZ integer check at repo init. Add a check script to the build pipeline.

**B7. Edge case — active tile with zero Gaussian primes in halo.** `V(G_tile) = ∅` is admissible (R-lattice point exists in proper, but no Gaussian primes nearby). Pipeline implementation must emit empty TileOp gracefully (no ports, no bridge contribution). Math is fine; code-level concern.

**B8. Notation uniformity.** `G_A` vs `G_{Tile A}`; `ufs_local(w; G_tile)` vs implicit-T forms. Harmonize throughout in crystallization.

**B9. Port enumeration canonical rule determinism.** Spec is correct (representatives are unique per face); implementation needs bit-for-bit deterministic lex sort (ties-free by construction).

**B10. Corollary of Lemma 5 reflexive self-loop.** The "all ports containing `w` lie in the same `G_ports_T` component" argument uses `w ~ w` trivially. Note explicitly in paper that the reflexive self-loop is what does the work — otherwise a reader can miss the mechanism.

---

## 2026-04-19 — Codex xhigh adversarial audit pass (B11–B15)

Findings from the independent pressure-test by Codex gpt-5.4 xhigh (dispatch `01KPK4QEZYWDZG72GWT89AXV4R`). **Verdict: PASS — no blocking issue found.** Items below are proof-hardening notes, not blockers.

**B11. Theorem 9 entry/exit hop — unstated geometric sublemma.** The entry/exit hops rely on: "a port prime `u` lying in `halo(T_u) ∩ halo(T_0)` is a face prime of `T_0` on the face shared with (or adjacent to) `T_u`." This follows from I0 (C < S/2) plus the face-strip definition, but should be named as an explicit micro-lemma and cited rather than left implicit in the proof flow.

**B12. Tower-closing margin corollary — discrete/continuous gap.** The corollary reads `tower_height(i) ≤ 1` as implying the continuous slice height `y_upper(x) − y_lower(x) ≤ S`. Tower activity is tile-based (discrete), but the bound uses the continuous `y_upper/y_lower` arcs. Either derive the continuous bound from the discrete activity predicate explicitly, or record the discrete-to-continuous step as a project-parameter geometric axiom.

**B13. Theorem 11 Case A.2 — tightness.** The contradiction uses `‖u − u'‖ ≤ S√2` (full tile diameter), but the actual no-face-prime bound is `(S − 2C)√2` (strict interior diameter). The current proof is valid under the stated annulus thickness `R_outer − R_inner > S√2 + 2√K`, but the tighter `(S − 2C)√2` would relax the thickness requirement and strengthen the margin. Worth tightening in crystallization.

**B14. Theorem 12 fold — consecutive duplicates on reflection axes.** When `y_k` lies on a reflection axis (`y = x` or `x = 0`), two consecutive applications of different `g_k` can produce `x_k = x_{k+1}` (duplicate vertex). Harmless if paths are interpreted as walks or duplicates are deduplicated, but the proof should state the interpretation explicitly.

**B15. Adjacent Octants Lemma — polar-angle convention + origin exclusion.** The lemma uses `θ_u ∈ [π/4, π/2]` for `u ∈ R` and derives `θ_v ∈ (0, 3π/4)`. The convention `θ = atan2(y, x)` and the implicit `R_inner > 0` (so `‖u‖ > 0`, angle well-defined) should be stated explicitly. Trivially satisfied at project scale; purely a completeness note.

---

## 2026-04-19 — blueprint × math cross-consistency audit (B16–B19)

Findings from the Opus 4.7 cross-doc audit of `campaign-blueprint.md` against `tile-operator-definition-v-claude.md`. One blocking soundness issue (prefilter tightness) was caught and **fixed in-place in the blueprint (§2 and §4.6)**, not carried to backlog. Items below are non-blocking doc/hygiene drifts.

**B16. Assert Lemma-4 prerequisite at stitch time.** Blueprint `match_io` / `match_lr` (§7.3) check `n[FACE_R] == n[FACE_L]` (port-count equality) but do not explicitly re-assert the Lemma 4 prerequisite `tₓ_A = tₓ_B` / `tᵧ_A = tᵧ_B` (uniform offset). Currently guaranteed by Grid construction (§4.1), but an explicit invariant assertion in the stitching path would harden against future grid-builder bugs.

**B17. Blueprint needs a constants glossary.** `SIDE_EXP`, `MAX_PRIMES_GPU`, `BITMAP_WORDS`, `MAX_CANDIDATES_GPU` are used in §6.2 / §8.2 code snippets without inline definitions (inherited from legacy kernels). Add a Glossary section or inline them at first use. Under snapped grid with 257×257 proper region and `C = 6` halo, `SIDE_EXP = S + 1 + 2·C = 269` at project scale.

**B18. 128-group cap and 192 B face_groups budget are engineering artifacts, not math constraints.** The math doc places no upper bound on UF-group count or port count per tile; only the canonical Lemma 4 / Theorem 11 structure. Blueprint §5.2's caps should be labeled explicitly as E-side (engineering) artifacts with the overflow-escalation path as the soundness-preserving fallback.

**B19. Pre-filter documentation trail.** The `2·R·⌈√K⌉ + 1` fix applied on 2026-04-19 (see blueprint §2, §4.6) deserves a one-line reference in the canonical math doc §Tower tiling pre-filter paragraph — specifically that implementations must use `⌈√K⌉` (ceiling), not `⌊√K⌋` (floor) as the pre-filter coefficient, to preserve completeness at non-square K.
---
