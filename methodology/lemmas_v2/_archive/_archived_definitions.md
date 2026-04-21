# Foundations — Setting, Graph, Tile, Snapped Grid

**Role:** Ground-floor definitions for the snapped-grid TileOp formalism. Primitives only — no tile-operator construction, no stitching, no completeness proofs.
**Scope:** Setting, prime graph, closure and connected components, Union-Find, tile geometry, collar and halo, tile graph, snapped-grid axiom.
**Does not cover:** face primes, ports, groups, TileOp, stitch, grid composition — these build on this document in subsequent files in `lemmas_v2/`.
**Supersedes:** the *Graph*, *Tile*, and structural parts of *Faces* in `lemmas/tile-operator-definition.md`. The closure algebra in `lemmas/closure-decomposition.md` is unchanged and cited as-is.

---

## Design Commitments

Three load-bearing commitments taken before any primitive is defined. They shape what counts as a legal definition in this document and in files that depend on it.

1. **Snapped grid.** All tile origins lie on the global S-lattice. Fractional tower offsets are not admitted in the formalism. (Formalized in §7.) Rationale: under shifted faces, the universal per-face encoding budget is insufficient for sound-and-complete stitching.

2. **No dead-end pruning.** Ports are never discarded for carrying no connectivity. (Enforced in the forthcoming ports document.) Rationale: pruning in one tile but not its neighbor destroys the shared port vocabulary on the shared face.

3. **Partition-level statements.** Soundness and completeness are phrased as equalities of partitions on V. Union-Find correctness is a separate, once-proved bridge claim. (Formalized in §3.) Rationale: the previous formalism drifted by conflating algorithmic UF state with the equivalence relation.

---

## §0 — Setting

Operate over the Gaussian integers **Z[i]** with squared norm `N(a + bi) = a² + b²`. A Gaussian prime is an element of Z[i] whose norm is a rational prime, or an associate of a rational prime `p ≡ 3 (mod 4)`.

Fix:

- **Region** R ⊂ Z² — a bounded subset of the integer lattice. R is the computational domain.
- **Squared step bound** K ∈ ℕ — the connectivity resolution. Our campaigns: K ∈ {36, 40}.

This document treats R generically. The annular-octant specialization `R = {(x, y) : x ≥ 0, y ≥ x, x² + y² ≤ R²}` lives in a revised grid-composition document.

---

## §1 — The Prime Graph

Define **G = (V, E)** over R:

- **V** = Gaussian primes in R. V is finite.
- **E** = `{{p, q} : p, q ∈ V, p ≠ q, ‖p − q‖² ≤ K}`.

A **path** of length m in G is a sequence `v₀, v₁, …, vₘ` of vertices with `{vⱼ, vⱼ₊₁} ∈ E` for each j. The length-zero path `v` exists for every `v ∈ V`.

For V' ⊆ V, the **induced subgraph** `G[V']` has vertex set V' and edge set `E ∩ {e : e ⊆ V'}`.

---

## §2 — Closure and Connected Components

For an edge set E over V, the **closure** `cl(E)` is the equivalence relation on V with `u ~ v` iff there exists a path `u = w₀, w₁, …, wₘ = v` with each `{wⱼ, wⱼ₊₁} ∈ E`.

**Closure-operator properties** (extensive, monotone, idempotent) and the **decomposition theorem** `cl(⋃ᵢ Aᵢ) = cl(⋃ᵢ cl(Aᵢ))` are proved in `closure-decomposition.md` and used without restatement.

**Connected components.** The equivalence classes of `cl(E)` on V. We write `[v]_G` for the class of v.

**Partition.** `P(G) := V / cl(E)` is the set of connected components — a partition of V.

**Restriction of a partition.** For V' ⊆ V and partition P of V:
```
P ↾ V'  :=  { C ∩ V' : C ∈ P, C ∩ V' ≠ ∅ }
```
— a partition of V'.

**Monotonicity on a fixed V.** If `E ⊆ E'`, then `cl(E) ⊆ cl(E')`, and `P(V, cl(E'))` is obtained from `P(V, cl(E))` by merging some classes (never splitting).

---

## §3 — Union-Find as Partition Realizer

**Partition (mathematical object).** A partition of a finite set V is a set of non-empty, pairwise-disjoint subsets whose union is V. Equivalently, an equivalence relation on V.

**Union-Find (algorithmic object).** A data structure maintaining a partition of V under two operations:

- `Find(v)` — returns a canonical representative of the class containing v.
- `Union(u, v)` — merges the classes of u and v.

A UF instance **realizes** a partition P iff its induced partition equals P.

**Separation of concerns.** In this formalism:

- Definitions, lemmas, and theorems are stated over partitions.
- UF is the algorithmic realizer. Its correctness — "the running UF's induced partition equals the intended partition" — is a separate bridge claim, proved once for the construction and then used.
- A UF implementation bug is distinct from a partition-definition bug. The formalism must make it possible to identify which class any observed failure belongs to.

---

## §4 — The Tile

A **tile** `T` is a closed, axis-aligned square region of Z². Parameters:

| Parameter | Symbol | Meaning |
|-----------|--------|---------|
| Origin | (tₓ, tᵧ) ∈ Z² | Lower-left corner |
| Side | S ∈ ℕ | Edge length (our project: S = 256) |

The **tile proper** is the set of lattice points
```
T = [tₓ, tₓ + S] × [tᵧ, tᵧ + S]
```
— `(S + 1)²` lattice points in total. All four sides are **closed** (inclusive).

**Coordinate convention.** Tile-relative coordinates: `col := x − tₓ`, `row := y − tᵧ`. Row 0 is the bottom edge; row S is the top edge. Col 0 is the left edge; col S is the right edge.

**Four faces.** The geometric sides of T:

| Face | Boundary line | Along-face coord h |
|------|---------------|--------------------|
| **I** (inner) | row = 0 | h = col |
| **O** (outer) | row = S | h = col |
| **L** (left) | col = 0 | h = row |
| **R** (right) | col = S | h = row |

Faces in this document are **geometric** — line segments of lattice points. Face primes, ports, and groups are defined in the next document.

**Corner membership.** Each corner point belongs to exactly two faces of its own tile. For example, `(tₓ, tᵧ)` lies on both I and L.

**Shared-boundary convention.** Suppose two tiles `T, T'` have tile-propers that share a full column of lattice points (horizontally adjacent, with `T'` to the right of T and sharing column `x = tₓ + S = t'ₓ`). Then:

1. That shared column is contained in both `T` and `T'` as sets of lattice points.
2. A Gaussian prime at `(tₓ + S, y)` is a single vertex in V and a member of both `V ∩ T` and `V ∩ T'`.
3. Graph-theoretically there is no duplication: one lattice point, one vertex, present in two tiles' proper regions.

Symmetrically for vertical sharing.

---

## §5 — Collar and Halo

The **collar width** is
```
C := ⌊√K⌋
```

The **halo** of tile T is the extended region of lattice points
```
H(T) := [tₓ − C, tₓ + S + C] × [tᵧ − C, tᵧ + S + C]
```
— `(S + 1 + 2C)²` lattice points. The halo is the full local processing domain used to compute connectivity on the tile.

**Collar sufficiency.** For any edge `{p, q} ∈ E`, the per-axis offset `Δ = |x_p − x_q|` or `|y_p − y_q|` satisfies `|Δ| ≤ C`.

*Proof.* If `|Δ| ≥ C + 1 = ⌊√K⌋ + 1`, then `Δ² ≥ (⌊√K⌋ + 1)² > K`, contradicting `‖p − q‖² = Δx² + Δy² ≤ K`. ∎

**Edge completeness (local form).** If `p ∈ V ∩ T` (p is a prime in the tile proper), then for every q with `{p, q} ∈ E`, we have `q ∈ V ∩ H(T)`. (Direct corollary of collar sufficiency.)

*The global form* — "the union of halo-restricted edge sets across all tiles equals E" — is a property of the tiling arrangement (coverage), proved in the revised grid-composition document.

**Bidirectional collar.** For two tiles sharing a boundary row or column under the shared-boundary convention, each tile's halo extends C units beyond the shared boundary into the other tile's territory. Their halos overlap on a strip of width `2C + 1` centered on the shared boundary line (C units on each side, plus the shared line itself). This strip is contained in both halos.

---

## §6 — The Tile Graph

For a tile T:

- `V_T := V ∩ T` — primes in the tile proper.
- `V_T^H := V ∩ H(T)` — primes in the halo.
- `E_T := { {p, q} ∈ E : p, q ∈ V_T^H }` — edges with both endpoints in the halo.
- `G_T := (V_T^H, E_T)` — the **tile graph**, the induced subgraph on halo primes.

**Edge coverage for tile-proper primes.** For any `p ∈ V_T` and any edge `{p, q} ∈ E`, both endpoints are in `V_T^H` (by edge completeness local form). Hence `{p, q} ∈ E_T`.

**Tile closure.** `cl(E_T)` is the equivalence relation on `V_T^H` generated by `G_T`'s edges — computed via the closure operator of §2.

**Tile partition.** The tile's partition over halo primes is `P(V_T^H, cl(E_T))`. Its restriction to the tile proper is `P(V_T^H, cl(E_T)) ↾ V_T`.

---

## §7 — The Snapped Grid

**Axiom (snapped-grid tiling).** All tile origins lie on the global S-lattice:
```
(tₓ, tᵧ) = (iS, jS)   for some (i, j) ∈ Z².
```

We index tiles as `T_{i,j}` with origin `(iS, jS)`.

**Consequences.**

1. **Exact boundary coincidence.** For horizontally-adjacent tiles `T_{i,j}` and `T_{i+1,j}`, the right face of the former and the left face of the latter are the same set of lattice points: the column `{x = (i+1)S} × [jS, (j+1)S]`. Symmetrically for vertical neighbors sharing a row.

2. **Bidirectional collar exact overlap.** Each tile's halo extends C units beyond the shared boundary. Under snapping, both halos contain the same strip of `2C + 1` columns (or rows) around the shared boundary. Consequently `V ∩ H(T_{i,j}) ∩ H(T_{i+1,j})` equals exactly the primes in this shared strip.

3. **Forward claim — face-prime set equality.** Under the snapped grid, the set of lattice points used to define face primes on a shared face is identical from both tiles' perspectives. Hence the face-prime sets on a shared face are equal as sets. (Precise statement deferred to the face-primes document; this claim is why the snapping axiom is load-bearing.)

**Cost of the axiom.**

- **No fractional tower offset.** In the shipping code, adjacent towers may differ in vertical base by a non-multiple of S — the `f = delta mod S` offset in `compositor.cpp`. Under the snapped-grid axiom this offset is disallowed: all tile boundaries fall on the global S-lattice.
- **Arc curvature compensation.** Variable tower heights on the annular octant cannot be adjusted by fractional offset. Any adjacent-tower height difference must be an integer multiple of S. The coverage proof for annular tiling under this constraint is reworked in the revised `grid-composition.md`.
- **Scope note.** The shipping C++/CUDA pipeline operates under a more general regime than this formalism admits. Reconciling the code to snapped-only, or generalizing the formalism to shifted towers, is an explicit later concern — not in scope for `lemmas_v2/`.

---

## Relation to the Previous Formalism

This document replaces the following portions of `lemmas/tile-operator-definition.md`:

- **The Graph** section → §1.
- **Tile** section (origin, side, collar, halo, shared boundary, collar sufficiency, edge completeness) → §4, §5, §6.
- **Faces** section (geometric parts only: coordinate convention, four faces, depth predicates) → §4.

Not replaced (cited from the old lemmas):

- `lemmas/closure-decomposition.md` — the closure algebra is unchanged and used as-is from §2.

Deliberately not covered here (belong to subsequent files):

- Face primes and their along-face extent — `face-primes.md` (next).
- Ports as geometric connected components on a face — `ports.md`.
- Groups as tile-local equivalence classes over ports — `groups.md` (or merged into ports).
- TileOp as (closure, projection) — `tile-operator.md`.
- Stitch operations and modes — `stitch.md`.
- Annular tiling, tower geometry, spanning verdict — revised `grid-composition.md`.
- Soundness and completeness theorems — `tile-operator-completeness.md` (rewritten).

---

**Depends on:** `closure-decomposition.md` (cited, unchanged).
**Used by:** `face-primes.md` (forthcoming) and all subsequent `lemmas_v2/` documents.
