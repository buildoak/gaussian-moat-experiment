# Audit — Gaussian Moat Tile-Operator Math + Campaign Blueprint

Date: 2026-04-19

Files audited:
- `tile-operator-definition-v-claude.md`
- `campaign-blueprint.md`
- `BACKLOG.md`

## Executive summary

My overall judgment is:

- **Core transfer-operator idea:** strong. The local compression, shared-face positional stitching, and the `I_ports` / `O_ports` flag-based moat verdict are mathematically well motivated and, in their core graph-theoretic form, look **sound**.
- **What I would sign off on today:**
  - Lemma chain up through **Lemma 5**.
  - **Lemma 4** as the key snapped-grid positional-identification result.
  - **Theorem 9** as the right global equivalence mechanism, **conditional on** the grid-geometry invariants actually holding.
  - **Theorem 11** as the right soundness/completeness statement for the port-graph verdict, **conditional on** (a) the geometric band definition being the intended one and (b) no engineering overflow shortcut changing the semantics.
  - **Theorem 12** reflection closure as conceptually correct and very likely correct as written.
- **What I would not sign off on yet as a full exact theorem package:**
  1. The math doc's **project-specific BZ reconciliation block uses `K = 6`**, while the project / blueprint use **`K = 36` or `K = 40`**. This is a real cross-doc inconsistency.
  2. The proof of the **tower-closing margin corollary** and therefore the fully unconditional proof of **I4** still has a discrete/continuous gap.
  3. The blueprint is **not exact as written** because its overflow path intentionally returns conservative `SPANNING`, which breaks completeness with respect to the exact theorem.
  4. The blueprint's current handling of **UF labels vs raw roots** is underspecified/inconsistent and needs a dense-remap step.

So the correct top-line verdict is:

- **Mathematics:** the core operator logic is good, but the document is still **conditionally complete rather than fully buttoned-down complete**.
- **Blueprint:** faithful in spirit and substantially improved over the previous geometry-heavy compositor, but **not yet faithful enough to the exact theorem** until overflow and label-compaction are fixed.

---

## 1. Math audit — what looks correct

## 1.1 Definitions / Process P / Lemmas 1–3

Reference: `tile-operator-definition-v-claude.md:20-165`

This part is fundamentally fine.

- The strengthened notion of **single local connectivity group** is the right one.
- The extension of `ufs_global` from vertices to connected local groups is valid.
- **Lemma 2** is the correct abstraction: any decomposition into local connected pieces preserves the set of global connected-component labels.
- Applying that abstraction to face-strip connected components in **Lemma 3** is correct.

I do not see a mathematical flaw in this layer.

## 1.2 Shared-face positional equality (Lemma 4)

Reference: `tile-operator-definition-v-claude.md:169-187`

This is the key snapped-grid pivot, and it looks right.

Why it works:
- the face-prime sets on a shared face really are the same world-space set;
- the face-strip induced graph is therefore identical on both sides;
- the along-face coordinate matches because the shared-grid prerequisite aligns the base coordinate;
- the perpendicular coordinate differs only by a constant translation, so lex order is preserved.

This is the cleanest and most valuable theorem in the document. It justifies deleting the old `h1` / geometric matching machinery.

## 1.3 TileOp creation + Lemma 5

Reference: `tile-operator-definition-v-claude.md:190-215`

Also looks correct.

The key step is that a face-strip component is connected inside `G_facestrip_f`, hence also inside `G_tile` because `G_facestrip_f ⊆ G_tile`. Therefore assigning each port the `G_tile` UF label of any of its members is well-defined.

That makes the TileOp exactly the right compressed object: it retains only the boundary ports and the tile-UF component each port belongs to.

## 1.4 Edge coverage / corner closure / Theorem 9 structure

Reference: `tile-operator-definition-v-claude.md:582-760`

The overall structure is the right one:

- **Lemma 6** gives vertex coverage.
- **Lemma 7** gives edge coverage.
- **Lemma 8** is the mechanism that kills the diagonal-gap problem.
- **Theorem 9** then lifts prime connectivity to port-graph connectivity.

I did not find a contradiction in the run-decomposition argument. The proof is a bit long, but the logic is coherent.

## 1.5 Theorem 11 (moat verdict)

Reference: `tile-operator-definition-v-claude.md:724-760`

This is the right theorem.

What is especially good here:
- You abandoned exposure-based boundary logic in favor of **per-UF-component `inner_flag` / `outer_flag`**.
- That is the correct fix to the old soundness/completeness issue where a boundary-relevant component could sit in the tile interior and be missed by exposed-face heuristics.
- **Lemma 10 (Exit lemma)** is the right bridge between interior flagged primes and boundary ports.

Conditionally, this theorem is strong and I believe the argument is correct.

## 1.6 Theorem 12 (reflection closure)

Reference: `tile-operator-definition-v-claude.md:757-869`

This section is substantially better than most ad hoc “symmetry closure” arguments I see in engineering math docs.

- The **Adjacent Octants Lemma** is the right local-angle restriction.
- The **Monotone Reflection Lemma** is exactly the right inequality.
- The fold construction is believable and, modulo some proof-hardening notes below, appears correct.

I would keep this section.

---

## 2. Math audit — blocking / high-severity issues

## M1. The project-specific BZ reconciliation block uses the wrong `K`

Reference:
- math doc: `tile-operator-definition-v-claude.md:333-357`
- blueprint: `campaign-blueprint.md:35-66`

The math doc defines `K` as the **squared step bound**. That is consistent everywhere else in the document and in the blueprint.

But the explicit “project deployment” BZ check in the math doc is:

- `R_inner = 80·10^6`
- `R_outer = 80 008 192`
- `K = 6`

That is inconsistent with the blueprint, which clearly treats deployment `K` as **36 or 40**.

So the statement

> “Both `BZ_I` and `BZ_O` are prime-free for the project deployment”

is currently proved for the wrong deployment.

### Why this matters

This does **not** break the abstract theorem, but it does break the project-level claim that the norm-form `geo_I` / `geo_O` coincide with the witness-form boundary sets for the actual campaign parameters.

### Recommended fix

Replace the project block with two explicit checks:
- one for `K = 36`
- one for `K = 40`

I re-evaluated the bad zones for the stated radii, and they appear harmless for both values too:

- **For `K = 36`:**
  - `BZ_I` contains the integers `6400000960000035` and `6400000960000036`.
  - `6400000960000035` is composite.
  - `6400000960000036 = 4 * 40000003^2`; since Gaussian-prime norms are `2`, odd primes `≡ 1 mod 4`, or `p^2` for odd primes `p ≡ 3 mod 4`, this value is **not** a Gaussian-prime norm.
  - `BZ_O` contains `6401309827010596 = 80008186^2`, also not a Gaussian-prime norm.
- **For `K = 40`:**
  - `BZ_I` contains `6400001011928891`, composite.
  - `BZ_O` contains `6401309775076432`, composite.

So this looks like a **doc/example bug**, not a fundamental obstacle.

## M2. Tower-closing margin corollary has a real proof gap

Reference: `tile-operator-definition-v-claude.md:366-372`

This is the biggest remaining mathematical issue.

The corollary starts from:

> “Tower height `≤ 1` in column `i` means `y_upper(x) - y_lower(x) ≤ S` for all `x` in the column.”

That implication is **not proved**.

`tower_height(i) ≤ 1` is a **discrete tile-activity statement**.
`y_upper - y_lower ≤ S` is a **continuous strip-height statement**.

Bridging those two is nontrivial. It may be true at your parameter scale, but the current text does not establish it.

### Why this matters

This corollary is then used inside the tower-closing proof of **I4**, and **I4** feeds **Lemma 8**, which in turn feeds **Theorem 9**.

So the current global soundness/completeness story is best described as:

- exact **conditional on I4**, or
- exact **conditional on the tower-closing margin corollary / I4 proof**.

I would not call the entire theorem package “fully rigorous” until this is fixed.

### Two acceptable repair paths

#### Path A — formalize the missing discrete lemma

Add an explicit lemma of the form:

> If a column strip intersects two distinct row strips in the tile sense, then either the continuous strip height exceeds `S` at an integer `x`, or equivalently if the continuous strip height exceeds `S` somewhere then at least two row indices become active.

That is the mathematically strongest fix.

#### Path B — demote this to a deployment invariant

Since the whole section is already under **Project-parameter axioms**, the pre-paper version could simply say:

- I0, I1, I2 are assumed / checked.
- I4 is assumed / checked.
- the optional proof sketch is heuristic support, not the rigorous proof.

Then make the runtime / init scan for I4 **mandatory**, not optional.

For a pre-paper draft, this is completely legitimate and would sharply improve the document’s honesty.

## M3. Theorem 9 entry/exit hops rely on an unstated micro-lemma

Reference: `tile-operator-definition-v-claude.md:633-681`

The proof is fine in spirit, but the following fact is used implicitly:

> If a prime lies in `halo(T) ∩ halo(T')`, where `T, T'` are face-adjacent or diagonally adjacent under `C < S/2`, then that prime is a face prime on the relevant shared/common-neighbor faces.

This is true, but it should be stated explicitly.

I would add a named lemma such as:

> **Halo-overlap face-prime lemma.** Under `C < S/2`, any prime in a face-overlap strip belongs to the corresponding shared-face strips; any prime in a diagonal-overlap corner square belongs to the corresponding common-neighbor face strips.

This is proof-hardening, not a conceptual rewrite.

---

## 3. Math audit — medium / cleanup findings

## M4. The exact target theorem should be stated more cleanly

Right now there are really **two different exactness claims** mixed together:

1. exactness for the **norm-form** boundary sets `geo_I`, `geo_O`;
2. exactness for the **witness/geometric** boundary sets.

The abstract pipeline theorem only needs (1).
The deployment-specific geometric interpretation needs the BZ reconciliation to upgrade to (2).

I would make that separation explicit.

## M5. The exposed-face section is no longer load-bearing

Reference: `tile-operator-definition-v-claude.md:555-579`

In the new proof architecture, the verdict is no longer driven by exposed-face geometry; it is driven by `inner_flag` / `outer_flag` on UF components.

So the exposed-face section is now mostly explanatory / historical. Keeping it in the core proof flow makes the document look more complicated than it really is.

I would move it to an appendix or cut it from the proof spine.

## M6. Process-P can be collapsed into one generic lemma

Reference: `tile-operator-definition-v-claude.md:64-96`

All of `Process P` is really one statement:

> A cover/decomposition of a set into connected local pieces preserves the set of global connected-component labels.

That can replace the current 0–5 bullet list and make the opening 100 lines much shorter.

---

## 4. Blueprint audit — what is faithful and good

## 4.1 The blueprint correctly reflects the strongest parts of the math

Reference: `campaign-blueprint.md:9-31`, `185-378`

The following are faithful and good:

- snapped-grid positional stitching;
- no dead-end pruning;
- face-strip connected components instead of greedy 1-D clustering;
- replacing geometry-heavy boundary extraction with per-UF `inner_flags` / `outer_flags`;
- strict face-port count equality at shared faces;
- reflection closure treated as theorem, not runtime geometry.

This is a much better engineering target than the old staircase/exposure-heavy compositor.

## 4.2 The K4/K5 split is conceptually right

Reference: `campaign-blueprint.md:252-308`

Conceptually, the split is right:

- K4 computes tile-level UF and the per-component inner/outer flags.
- K5 computes face-strip connected components and packs the TileOp.

That matches the math perfectly.

---

## 5. Blueprint audit — blocking / high-severity issues

## B1. As written, the blueprint is not exact because overflow returns conservative `SPANNING`

Reference: `campaign-blueprint.md:468-479`

This is the biggest engineering issue.

The math doc proves an **exact** equivalence. The blueprint says:

- if a tile overflows the 192-port or 128-group budget,
- mark it as conservative spanning,
- which may produce a **false `SPANNING`**.

That means the implemented pipeline is **not complete for moat detection**.
A true moat can be turned into `SPANNING` by overflow.

### Conclusion

This is **not faithful** to the theorem if the stated goal is exact soundness + completeness.

### Fix

Overflow must trigger an **exact slow path**, not a conservative verdict.

Good options:
- re-encode the tile in a 512 B / extended format;
- or spill the tile to a CPU exact encoder;
- or re-run that tile in a dedicated “big TileOp” path.

But “overflow => spanning” cannot coexist with a claim of completeness.

## B2. UF labels vs raw roots are inconsistent; a dense remap is missing

Reference:
- `campaign-blueprint.md:236-239`
- `campaign-blueprint.md:258-287`
- `campaign-blueprint.md:303-306`
- `campaign-blueprint.md:326-344`

The blueprint simultaneously says:

- each port stores a `G_tile` UF label in `1..128`;
- K4 records flags keyed by **raw root indices** (`root = atomic_find_root(...)`);
- K5 overflows if `max_label > 128`;
- compositor uses `global_group_id(tile_index, local_label)` with local labels assumed dense.

Those are not yet reconciled.

### Why this matters

Raw UF roots are typically prime indices, not dense component IDs.
A tile can easily have only 8 actual components but a raw root index like `417`.
If you treat that root index as the wire label, you will overflow constantly or mispack flags.

### Concrete fix

Add a deterministic **dense remap in K5**:

1. collect all distinct raw UF roots that appear on at least one emitted port;
2. sort them deterministically (e.g. ascending raw root or first-seen in canonical port order);
3. assign dense labels `1..m`;
4. write `face_groups` using those dense labels;
5. pack `inner_flags` / `outer_flags` using the same dense labels.

Then the **group-count overflow** criterion becomes `m > 128`, not `max_raw_root > 128`.

### Related storage consequence

If K4 outputs flags keyed by raw roots, then `d_group_flags[num_tiles * 32]` is too small unless `MAX_PRIMES_GPU <= 128`.

So either:
- K4 must output flags for **all possible raw roots** (size proportional to `MAX_PRIMES_GPU`), and K5 compresses them; or
- K4 and K5 must share a compaction table.

Right now this is underspecified.

## B3. The BZ build-fail condition is worded incorrectly

Reference: `campaign-blueprint.md:51`

The blueprint says:

> “Build fails if BZ is non-empty.”

That is not the right condition.

The correct condition is:

> build fails if the bad zone contains a **Gaussian-prime norm** (equivalently, if witness-form and norm-form boundary sets differ).

A bad zone interval can contain harmless composite or square integers and still be perfectly acceptable.

In fact, for the actual project radii and `K = 36`, `BZ_I` contains **two integers**, both harmless. So a literal “non-empty” failure condition would reject a valid deployment.

## B4. The host/compositor burst-indexing snippet appears wrong

Reference: `campaign-blueprint.md:443-449`

The snippet

```cpp
const TileOp* column_ptr = host_tileops + tower_offset[i - i_min] * 256;
```

looks wrong in two ways:

1. if `host_tileops` is already `TileOp*`, multiplying by `256` is wrong pointer arithmetic;
2. `tower_offset[...]` is a **global** offset into the full tile list, but `host_tileops` is a **per-burst** buffer.

So for bursts after the first one, this indexing will point into the wrong place unless a burst-local base offset is subtracted.

This is not a theorem issue, but it is a real blueprint / implementation bug.

## B5. The blueprint treats I4 as “fully proved”; today it should still be runtime-asserted

Reference: `campaign-blueprint.md:120-130`

Because of the math issue in **M2**, I would not yet rely on the tower-closing proof of I4 as the sole source of truth.

My recommendation:
- keep the mathematical proof sketch;
- but in implementation, make the I4 scan **mandatory** at init for exact campaigns.

That gives you exactness at the deployment level while the paper-level proof is polished.

---

## 6. Blueprint audit — medium findings

## B6. Cross-tower matching loop should explicitly use overlapping `j`-range

Reference: `campaign-blueprint.md:352-378`

The pseudo-code shows how to match a single pair `T_{i,j}` ↔ `T_{i+1,j}` but does not say explicitly that the compositor should loop only over

```text
j ∈ [max(j_low(i), j_low(i+1)), min(j_high(i), j_high(i+1))]
```

That is implied by the math but should be written down in the blueprint to avoid accidental overmatching.

## B7. Lemma 4 prerequisites should be asserted in the stitching path

Reference: `campaign-blueprint.md:352-378`

Port-count equality is checked, but the alignment prerequisite (`t_x` or `t_y` equality via the snapped grid) is not asserted directly.

This is guaranteed by the grid builder, but I would still assert it in debug builds because a future grid regression here would silently invalidate the stitching theorem.

---

## 7. Concrete recommendations before claiming “sound + complete”

If the goal is to be able to say, without hedging, that the pipeline is mathematically sound and complete for the project campaign, I would do these four things first:

1. **Fix the BZ deployment block** in the math doc so it uses `K = 36` / `40`, not `6`.
2. **Either finish the discrete proof of I4** in the tower-closing regime, or state I4 as a checked deployment invariant and make the init scan mandatory.
3. **Replace conservative overflow with an exact fallback path**.
4. **Add dense label compaction** from raw K4 roots to TileOp-local labels in K5.

Once those are done, I would be comfortable calling the approach exact at the intended deployment level.

---

## 8. Best simplification path for the math document

This is where the biggest payoff is available.

## S1. Split the whole document into three layers

### Layer A — abstract graph compression theorem

No Gaussian primes. No annulus. No octants.

Define:
- a finite graph `G`;
- a collection of tile subgraphs `G_T` covering all edges;
- boundary/face-prime sets on each tile;
- shared-face identification;
- corner-closure assumption.

Then prove the abstract version of:
- Lemma 4,
- Lemma 10,
- Theorem 9,
- Theorem 11.

This becomes a pure graph theorem.

### Layer B — geometric instantiation on the snapped grid

Now prove that the octant annulus with your snapped grid satisfies the abstract assumptions:
- edge coverage;
- shared-face equality;
- diagonal closure / I4;
- reflection closure assumptions.

### Layer C — deployment instantiation of boundary sets

Finally add the small deployment-specific lemmas:
- witness-form vs norm-form `geo_I` / `geo_O`;
- BZ reconciliation;
- project parameter checks.

That separation will remove a lot of narrative interleaving and make the proof much easier to audit.

## S2. Replace “Process P” with one short lemma

Instead of the current 0–5 condition list, state one lemma:

> **Connected-piece label preservation.** If `P(I)` is any finite family of nonempty connected subsets of `I` whose union is `I`, then the set of global connected-component labels realized on `P(I)` is exactly the set realized on `I`.

Then ports become just one application of that lemma.

That should shrink the front matter noticeably.

## S3. Remove exposed-face classification from the proof spine

The new theorem no longer depends on exposed-face heuristics for correctness.
So the exposed-face section should move to:
- intuition,
- appendix,
- or be deleted.

That alone will make the proof look much simpler and more modern.

## S4. State one clean “Grid Regularity Assumption” bundle

Today I0–I4 are scattered between proof and project-specific argument.
For a pre-paper draft, I would write:

> **Grid Regularity Assumptions.** The snapped-grid tiling used in deployment satisfies I0–I4.

Then:
- either prove them in a separate appendix;
- or verify them computationally and cite the verifier.

This immediately cleans up the main proof chain.

## S5. Turn Theorem 9 into a quotient-graph statement

Conceptually, `G_ports_grid` is the quotient of the boundary-prime world by:
- tile-local boundary connectivity inside `G_T`, and
- shared-face identity across tiles.

If you rewrite Theorem 9 in that language, the current long run-boundary proof can be shortened considerably.

Even if you keep the constructive proof, lead with the quotient interpretation. It tells the reader in one sentence why the theorem is true.

---

## 9. Bottom line

If I had to summarize in one paragraph:

- The **new approach is materially better** than the previous one. The move to snapped-grid positional stitching plus UF-component `inner`/`outer` flags is the right mathematical architecture.
- The **core theorem is close**: the local compression and global composition logic are strong.
- The two things still preventing an unconditional sign-off are:
  - the **I4 tower-closing proof gap**, and
  - the **blueprint’s non-exact overflow/label handling**.
- The fastest route to a clean paper is to **separate abstract graph theory from geometric deployment lemmas**, and to demote the remaining heavy geometry to either an appendix or a verified deployment invariant.


