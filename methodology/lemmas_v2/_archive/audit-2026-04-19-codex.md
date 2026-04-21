---
date: 2026-04-19
engine: codex
status: complete
target: tile-operator-definition-v-claude.md
---

# Overall Judgement

The proof chain is not yet canonical-proof sound. The intended structure is plausible for a snapped grid, and Theorem 12's reflection-folding argument appears basically sound under its stated radius assumption, but the tile-operator core has load-bearing definition/proof failures. The largest break is that `face_f_primes` is defined as an unbounded strip of primes in `G_full`, while TileOp construction assigns every such port a `G_tile` UF label as if the full strip were contained in the finite halo. That is false and invalidates Lemmas 3-5, Lemma 4's bridge identity as implemented, Theorem 9, and Theorem 11 unless the face-prime domain is redefined. The next tier of issues is mostly proof hygiene around project-parameter geometry: I4's closing-regime sketch mishandles boundary-only active tiles, I1/I2 rely on continuous arguments without lattice/floor details, and Theorem 11's interior-component contradiction depends on an unstated face-prime coverage lemma. The reflection closure is the best-developed part, but its dependency stack and endpoint assumptions should be made explicit before it is used as the final theorem.

# Prioritized Findings

## [FALSE] — Face-prime strips are unbounded but assigned finite-tile UF labels
- LOCATION: lines 122-124, 171-181; Lemmas 3-5.
- FINDING: The document defines a face-prime set as the full infinite/region-wide strip around a face line, then claims `G_facestrip_f ⊆ G_tile`; this is false for any finite tile halo.
- EVIDENCE: Lines 122-124 say face primes have "no along-face restriction" and are the "full strip within perpendicular distance C". Lines 171-181 define `G_tile` only on `halo(T)`, then line 181 says `G_facestrip_f ⊆ G_tile`. A vertical face strip contains primes arbitrarily far along the face line within `R`; most are outside `halo(T)`, so they have no `ufs_local(.; G_tile)` value and cannot receive `face_f_groups[p]`.
- IMPACT: Breaks the TileOp creation process and every later result that uses `face_f_groups`, including Lemma 5, `G_ports_T`, Theorem 9, and Theorem 11.
- SUGGESTION: Redefine `face_f_primes(T)` as a finite tile-local strip, probably `V(G_full) ∩ halo(T) ∩ {dist_perp_to_face <= C}` with the necessary along-face extent. Then restate Lemma 4 for shared faces and prove the finite sets are identical for snapped face-neighbor tiles.

## [FALSE] — Unbounded face strips make TileOp non-finite
- LOCATION: lines 127-140, 173-185.
- FINDING: With the current `face_f_primes` definition, a single TileOp can contain O(number of primes in a full annular strip), not bounded per tile.
- EVIDENCE: Ports are enumerated over all of `face_f_primes` (lines 129-140). Since `G_full` is later restricted to the whole octant annulus (line 277), a face strip spans the whole annulus in the along-face direction unless explicitly clipped. That contradicts the motivating claim that each GPU thread processes one bounded tile and emits compressed boundary data.
- IMPACT: The analytical object no longer matches the computational TileOp and cannot be the canonical proof for the implementation.
- SUGGESTION: State a finite face domain as part of the Tile definition and prove bounded cardinality from `S`, `C`, and local prime density/caps separately from the connectivity proof.

## [FALSE] — "Equivalent" geo_I/geo_O norm characterizations are not exact
- LOCATION: lines 279-292.
- FINDING: The lattice-witness definition is not generally equivalent to the continuous norm inequalities.
- EVIDENCE: `geo_I` requires a lattice point `q` inside the inner disk within distance `sqrt(K)` of `p` (line 281). The inequality `||p|| <= R_inner + sqrt(K)` only proves that the continuous disk around `p` intersects the continuous inner disk. It does not prove that the intersection contains a lattice point, especially near a tangential contact where the intersection area can be arbitrarily small.
- IMPACT: Does not break Theorem 11's norm-gap proof, which uses only witness implies norm bound, but it makes the definition section false.
- SUGGESTION: Replace "Equivalent characterizations" with one-way implications, or add a separate lattice-density lemma with exact hypotheses if exact equivalence is intended.

## [WEAK] — I4 closing-regime proof uses `P active requires j >= i`, which is false
- LOCATION: line 397; I4 proof sketch.
- FINDING: The closing-regime proof rests on a too-strong condition about active tiles near the diagonal.
- EVIDENCE: With canonical offset zero, `T_{i,i-1}` contains the lattice point `(iS,iS)` on its top-left corner, so it can be active on the `y=x` boundary even though `j=i-1`. Thus "P active requires j >= i" is false under the document's closed proper-region convention.
- IMPACT: Weakens I4 exactly in the degenerate corner case the prompt asked to pressure-test. The conclusion may still be true, but the stated proof does not establish it.
- SUGGESTION: Rework the closing proof with the sharp condition `j >= i-1`, explicitly handling boundary-only active tiles and showing the chosen common face-neighbor still contains a lattice point of `R`.

## [WEAK] — I4 regime split is asserted, not defined
- LOCATION: lines 374-399.
- FINDING: "Bulk regime" and "tower-closing regime" are not formal predicates, so the claim that the two regimes cover every diagonal pair is unsupported.
- EVIDENCE: I3 applies when "tower heights >= 2" (line 374), while the closing regime is described as "heights 0 or 1 near x = R_outer/sqrt(2)" (lines 376, 397). There is no formal proof that every diagonal active pair either lies between two height>=2 towers satisfying I3 or in the special closing geometry where the inner bound is irrelevant.
- IMPACT: I4 is an axiom, so this does not directly break later proofs if I4 is accepted. It does make the proof sketch inadequate for validating project parameters.
- SUGGESTION: Define tower height as cardinality, define the regimes as predicates on adjacent tower pairs, and prove exhaustiveness for project parameters.

## [WEAK] — I1 proof sketch uses continuous positive area to infer lattice occupancy
- LOCATION: lines 386-390.
- FINDING: The proof sketch for tower contiguity moves from positive-area strip intersections to "contains at least one lattice point" without a quantitative lattice lemma.
- EVIDENCE: Active tiles are defined by lattice points (line 325), but line 389 argues via continuous positive area and "S moderately large". Thin slivers of positive area can miss `Z^2` without an explicit width/area/inradius bound.
- IMPACT: Weakens the proof that project parameters satisfy I1. Since I1 is declared as an axiom, downstream proofs can still use it if the axiom is externally verified.
- SUGGESTION: Either keep I1 purely axiomatic or add a quantitative lemma for the minimum vertical/horizontal thickness of every intermediate row-strip intersection at `S=256`.

## [WEAK] — I2 proof sketch ignores floor/closed-boundary effects
- LOCATION: lines 370-388.
- FINDING: Boundary slope `<= 1` over a width-S column is not by itself a complete proof that discrete tower endpoint indices shift by at most one.
- EVIDENCE: `j_low` and `j_high` are extrema over all lattice points in a closed strip, not just samples of one continuous boundary value. Closed tile boundaries are shared, and the offset `o_y` can place boundary values exactly on row grid lines.
- IMPACT: Weakens the validation of I2 for project parameters.
- SUGGESTION: Prove the endpoint-index inequalities directly using floor/ceil formulas for `y_lower(x)` and `y_upper(x)` over each closed column strip, including equality cases.

## [WEAK] — Theorem 9 assumes a path with at least one edge
- LOCATION: lines 485-510.
- FINDING: The `u=v` zero-length path case is not handled in the `(=>)` direction.
- EVIDENCE: The proof assigns each edge a home tile and refers to `T_0` and `T_{n-1}` (lines 485, 507). If `u=v`, there are no edges and no home tile. The theorem statement allows arbitrary port primes and does not exclude `u=v`.
- IMPACT: Small formal gap. The result should follow from the Corollary of Lemma 5 plus bridge/corner handling if the two chosen ports containing the same prime are on different tiles, but it needs to be stated.
- SUGGESTION: Add a zero-edge base case before edge home-tile assignment.

## [WEAK] — Theorem 9's entry/exit hop assumes overlapping host tiles are adjacent without citing `C < S/2`
- LOCATION: lines 507-510.
- FINDING: The endpoint mismatch argument uses the same halo-intersection classification as run boundaries but does not carry the required `C < S/2` assumption.
- EVIDENCE: Line 321 states distant halos are disjoint provided `C < S/2`; Theorem 9 uses the classification at lines 487 and 509 without listing `C < S/2` among theorem assumptions.
- IMPACT: The proof is valid for project parameters but not as generally stated.
- SUGGESTION: Promote `C < S/2` to a global tiling assumption before the grid classification or include it in Theorem 9.

## [WEAK] — Theorem 11 relies on non-empty ports without saying so
- LOCATION: lines 571, 577.
- FINDING: The proof chooses "any face prime in port" and `w_I in port_I`, but the TileOp data structure only lists group labels; the non-emptiness guarantee comes indirectly from the port construction.
- EVIDENCE: Lemma 3 establishes non-empty ports if the port construction is well-defined (line 146), but Theorem 11 does not cite that fact. This matters because the extended flags are indexed by group labels, not by explicit prime witnesses in the serialized TileOp.
- IMPACT: Minor once the face-prime domain is fixed; currently masked by the larger face-strip bug.
- SUGGESTION: State that every port vertex denotes a non-empty prime set and cite Lemma 3 before choosing representatives.

## [WEAK] — A.2/B norm-gap proof uses "no face primes" to imply proper-interior containment too broadly
- LOCATION: lines 579-581.
- FINDING: The proof says a `G_tile` component with no face primes has all members in the proper-region interior, but this depends on face primes covering every halo point outside the proper region.
- EVIDENCE: Under the current unbounded-strip definition, a prime outside proper but in the halo is indeed within perpendicular distance C of at least one face line; under any future finite clipped definition, this must remain true, including corner halo points.
- IMPACT: This is fixable, but it is a hidden dependency of the contradiction in Theorem 11.
- SUGGESTION: Add a lemma: `V(G_T) \ proper(T)` is contained in the union of the four face-prime sets of T. Then A.2/B may cite it.

## [GAP] — Lemma 1 is stated but not actually proved
- LOCATION: lines 65-70.
- FINDING: Lemma 1 lists equivalent statements but does not supply a proof from the definitions of `ufs_global(I)` and `I~O`.
- EVIDENCE: Lines 67-70 restate the result rather than proving both directions. The proof is easy, but for a canonical proof document it should be explicit.
- IMPACT: Low mathematical risk, but it is the first link in the declared proof chain.
- SUGGESTION: Add a two-direction proof: a path from `u in I` to `v in O` gives equal component labels; equal labels give a connected component containing both, hence a path.

## [GAP] — Lemma 4 only proves the vertical shared-face case
- LOCATION: lines 156-164.
- FINDING: Lemma 4 states horizontal face-sharing is symmetric, but the proof only treats vertical sharing and asserts both tiles have the same `t_y`.
- EVIDENCE: Line 164 says "both tiles have `t_y = j_A*S`" for the shared vertical face. That is true for `A=T_{i,j}`, `B=T_{i+1,j}`. The statement also claims horizontal sharing by symmetry, but the horizontal case requires matching `t_x` and translating row/perpendicular coordinates, and is not written.
- IMPACT: Load-bearing because bridge edges for I/O faces need the same ordered-port identity.
- SUGGESTION: Prove both vertical and horizontal cases explicitly after fixing the finite face-prime domain. Include the uniform offset `o`, not only `i*S`.

## [GAP] — The "Fact" defines single local connectivity groups incorrectly for arbitrary subsets
- LOCATION: lines 47-53, 55-60.
- FINDING: A Process-P element is a set of primes, but the "single local connectivity group" definition is phrased as `S := V(G_random)` for an arbitrary subgraph, not as an arbitrary subset contained in one connected component.
- EVIDENCE: Process P condition 3 says each `P^I_j` is a single local connectivity group (line 59), but no `G_random` is explicitly attached to each `P^I_j` unless one constructs an induced connected subgraph on that set. The current wording also says "Equivalently, V(G_random) is a single connected component of G_random" (line 47), which is stronger/different than "contained in one component of a larger local graph".
- IMPACT: Definitions are usable by intent, but formally muddy in Lemma 2 and all port applications.
- SUGGESTION: Define a single local connectivity group as any non-empty `S subset V(G_local)` on which all vertices have the same `ufs_local(.;G_local)` value.

## [GAP] — Theorem 9 depends on every run endpoint being a face prime of the run's home tile
- LOCATION: lines 489-497.
- FINDING: The proof asserts run-boundary vertices are face primes of their home tiles, but only proves it for face/diagonal halo intersections, not for the transition caused by changing arbitrary home-tile assignments.
- EVIDENCE: If an edge lies in multiple tile halos, arbitrary home-tile assignment can create a run boundary between two tiles even when the path has not geometrically crossed a face. The boundary vertex is in both halos, but the proof still needs to show it lies within C of a relevant face of each tile. This follows from the halo-intersection classification for face/diagonal neighbors, but it is not spelled out for arbitrary assignment choices.
- IMPACT: Formal proof gap in the main equivalence theorem.
- SUGGESTION: Either choose home tiles by a deterministic rule that changes only when necessary, or add a lemma: if `x in halo(T) ∩ halo(T')` for distinct adjacent/diagonal snapped tiles, then `x` is a face prime of each tile on the appropriate face(s).

## [GAP] — Theorem 11's contrapositive applies Lemma 10 to D without proving D is the relevant component in the tile
- LOCATION: line 579.
- FINDING: The proof says Lemma 10 would produce a face prime in "u's `G_{T_{u'}}` UF group"; this should be `u'`'s group and needs the path from `u'` to outside `V(G_{T_{u'}})` to be inside D.
- EVIDENCE: D is the `G_full` component containing `u'`, so such a path exists if D extends outside the tile. The proof has the ingredients, but the variable name is wrong and the component relation is not unpacked.
- IMPACT: Mostly wording, but in a proof this is a fragile load-bearing step.
- SUGGESTION: Correct `u` to `u'` and write the contrapositive application explicitly.

## [GAP] — Corollary combining Theorems 11 and 12 omits Theorem 11's assumptions
- LOCATION: lines 690-694.
- FINDING: The full-annulus corollary cites only `R_inner >> sqrt(K)` and omits the annulus thickness and tiling axioms needed by Theorem 11.
- EVIDENCE: Theorem 11 depends on the earlier annulus thickness assumption (lines 294-299), tile coverage/edge coverage, I4, and the corrected TileOp definitions. Theorem 12 only adds `R_inner > sqrt(2K)` (line 655).
- IMPACT: The final headline equivalence is under-specified.
- SUGGESTION: State the corollary under the conjunction of all project-parameter assumptions: snapped grid, `C<S/2`, I1/I2/I4 or their verified replacements, annulus thickness, and `R_inner > sqrt(2K)`.

## [INCONSISTENCY] — Vestigial offset-tower language remains in a snapped-grid formalism
- LOCATION: line 122.
- FINDING: The face-prime definition justifies unrestricted along-face extent by referring to "offset tiles (adjacent towers with different base_y)", which is explicitly outside the strictly snapped formalism.
- EVIDENCE: The critical context requires f=0 snapped grid only. Line 122 imports the old offset-tower rationale directly into the canonical proof.
- IMPACT: More than cosmetic because it motivates the unbounded face-strip definition that breaks TileOp finiteness.
- SUGGESTION: Remove offset-tower rationale and replace it with a snapped-grid reason for whatever finite face domain is actually needed.

## [INCONSISTENCY] — `G_full` silently changes meaning mid-document
- LOCATION: lines 22-25, 273-278, 590-598.
- FINDING: `G_full` first means a graph over an arbitrary bounded region, later means the octant-restricted graph, and later `G_full|_A` means the full-annulus graph.
- EVIDENCE: Line 22 defines `G_full` generically; line 277 says "From this point on, G_full is ... restricted to R"; lines 596-598 then use `G_full|_A`.
- IMPACT: This is a real source of ambiguity in Theorem 12 and in definitions of `I_A`, `O_A`, `geo_I`, and `geo_O`.
- SUGGESTION: Use distinct names, e.g. `G_R` for the octant graph and `G_A` for the full-annulus graph. Keep `G_full` only for the abstract initial graph or remove it.

## [INCONSISTENCY] — "Port" denotes both a prime set and a graph vertex
- LOCATION: lines 127-140, 201-206, 473-475.
- FINDING: The document alternates between ports as sets of primes and ports as vertices `(T,f,p)` of `G_ports_grid`.
- EVIDENCE: Lines 140 and 188 treat `port_p` as a set containing primes. Lines 201-204 define port graph vertices as `(f,p)`. Lines 473-475 say a prime "sits in a port" and `port_u` is tile-tagged.
- IMPACT: Increases risk of invalid applications of Lemma 5 and Theorem 9, especially where the proof says "any port containing u" while using graph vertices.
- SUGGESTION: Introduce separate terms: `PortSet_T(f,k)` for the prime set and `PortVertex(T,f,k)` for the graph vertex.

## [INCONSISTENCY] — Closed tile proper regions create shared ownership but no partition convention
- LOCATION: lines 100, 306-309, 325, 432-436.
- FINDING: Tiles have closed proper regions, so seam lattice points belong to multiple active tiles, but some prose still speaks as if strips partition `R`.
- EVIDENCE: Lines 100 and 306-309 use closed intervals. Line 345 says column-strips partition into closed ranges, which overlap on boundaries. Lemma 6 chooses one tile via floor but does not discuss the other active owners.
- IMPACT: Mostly safe because later lemmas need at least one owner, not unique ownership. But any "partition" wording is false under the shared-boundary convention.
- SUGGESTION: Replace partition language with closed cover language and state explicitly that multiple proper owners are allowed and harmless.

## [INCONSISTENCY] — The exposed-face classification is not used by the actual verdict
- LOCATION: lines 416-429, 536-542.
- FINDING: The document defines inner/outer/side-exposed faces, then the verdict deliberately ignores exposure and uses UF flags.
- EVIDENCE: Lines 424-429 classify exposed faces; lines 536-542 define `I_ports`/`O_ports` by component flags independent of exposed faces.
- IMPACT: Not mathematically harmful, but confusing in a canonical proof because it suggests a boundary-interface proof that the document no longer uses.
- SUGGESTION: Either mark exposed-face classification as non-load-bearing intuition, or remove it from the canonical proof path.

## [ORPHAN] — `stitch(TileOp_A, TileOp_B)` output is never used downstream
- LOCATION: lines 256-264.
- FINDING: The binary stitching operator producing `TileOp_AS` and `TileOp_BS` is defined but later the proof uses `G_ports_grid` directly, not iterative stitched TileOps.
- EVIDENCE: After line 264, `TileOp_AS`, `TileOp_BS`, and `stitch()` do not appear in the proof chain; only bridge edges and connected components of `G_ports_grid` matter.
- IMPACT: Dead formalism. It can mislead readers into thinking associativity/order of stitching has been proved or is required.
- SUGGESTION: Remove `stitch()` from the proof doc or demote it to implementation commentary after the graph-theoretic proof.

## [ORPHAN] — Two-tower interface structure is not part of the main proof
- LOCATION: lines 403-413.
- FINDING: The corollary is explicitly descriptive and depends on I3, while the main proof says it uses I4 instead.
- EVIDENCE: Line 376 says I3 is only for the descriptive corollary and the main chain goes through I4. No later theorem cites the corollary.
- IMPACT: Dead weight in a truth-first canonical proof; also imports "shifted towers" language that can be confused with forbidden offset-tower language.
- SUGGESTION: Move it to a supportive/geometric notes section or remove it from the canonical proof.

## [AMBIGUITY] — `q²` notation in geo definitions is undefined for lattice points
- LOCATION: lines 281-282, 596-597.
- FINDING: The definitions use `q² < R_inner²` and `q² > R_outer²` for `q ∈ Z²`, but `q²` is not defined.
- EVIDENCE: Elsewhere the document uses `x² + y²` or `||p||²`. For a vector/lattice point `q`, `q²` could be misread as complex square rather than squared norm.
- IMPACT: Easy-to-fix ambiguity in a boundary definition used by Theorems 11 and 12.
- SUGGESTION: Replace with `||q||²`.

## [AMBIGUITY] — Active tile includes boundary-only contact without saying whether that is intentional
- LOCATION: lines 325, 397.
- FINDING: A tile is active if its closed proper region contains even one boundary lattice point of `R`, which can create boundary-only active tiles.
- EVIDENCE: This is exactly what invalidates `P active requires j >= i` in the I4 proof. A tile below `y=x` can be active if it contains a seam/corner point on `y=x`.
- IMPACT: The convention is likely intended and compatible with shared boundaries, but proof sketches must handle it.
- SUGGESTION: State explicitly that boundary-only active tiles are active, then audit all geometric sketches for equality cases.

## [AMBIGUITY] — Theorem 11's "moat within R" is graph-restricted, not geometric full moat
- LOCATION: lines 563-567, 585, 690-694.
- FINDING: Theorem 11 proves disconnection inside `G_full` as restricted to `R`, but the phrase "Gaussian moat ... within the annular octant R" could be read as a full-annulus moat before Theorem 12.
- EVIDENCE: Line 277 redefines `G_full` to the octant graph. Theorem 12 is needed to lift to `A`.
- IMPACT: Reader-facing ambiguity, not a proof break.
- SUGGESTION: Rename the Theorem 11 verdict "octant-restricted moat verdict" and reserve "full-annulus moat" for the corollary after Theorem 12.

# Things I Tried To Break But Couldn't

- The collar sufficiency calculation with `C = floor(sqrt(K))` is sound: if an integer coordinate offset exceeds C, its square exceeds K.
- Lemma 2's Process-P lift is sound once "single local connectivity group" is cleaned up; overlapping Process-P elements do not break the set-of-global-labels equality.
- The diagonal transition strategy in Theorem 9 is conceptually correct under I4: if a boundary vertex lies in the corner overlap of two diagonal tile halos, routing through an active common face-neighbor can express the transition with face bridges.
- I initially suspected line 321's distant-halo disjointness was false for index differences of 2. Rechecking the interval arithmetic shows it is correct under `C < S/2`: a one-tile index gap leaves a positive physical gap of `S - 2C`.
- Theorem 11's A.2/B norm-gap contradiction is structurally sound if the face-prime sets are finite halo-covering strips and if annulus thickness is assumed. Witness membership gives the needed one-way norm bounds.
- The Adjacent Octants Lemma's angular bound survives the "v radially inward" stress test: for any point within distance `d` of a radius-`r` point, the maximum angular displacement is bounded by `arcsin(d/r)` when `d < r`.
- The Monotone Reflection Lemma computations are correct for both adjacent reflection axes.
- Theorem 12's stepwise folding proof appears sound: changing the group element at each step is not itself a bug, because `x_k = g_k^{-1}(y_k)` and the next induction step applies the new inverse to the next original edge.
