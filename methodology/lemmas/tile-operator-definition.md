# Gaussian Primes Connectivity Transfer Operator

*Tile Operator* (TileOp) for short.

---

## Motivation

The Gaussian primes graph at radius R contains O(R²/ln R) vertices. Even for an annular ring of width W at radius R — the actual computational target — the scale is formidable: at R = 10⁹, W = 8192, the ring contains ~10¹² Gaussian primes. No single UF pass can hold this graph.

The Tile Operator decomposes this problem. It partitions the region into bounded tiles placed on a regular grid, runs independent local UF on each, compresses each result to its boundary connectivity, and composes the compressed results. The composition yields the same connectivity verdict as global UF — proved in [tile-operator-completeness.md](tile-operator-completeness.md).

The specific target: probe an annular ring for **connectivity transfer** from its inner boundary to its outer boundary — determine whether at least one path exists from the inner boundary to the outer boundary where each step connects Gaussian primes within distance √K. If connectivity transfers across the annulus through a chain of tiles — no moat of width √K exists. If no chain connects them — a moat is found.

The operator enables this probe to run in parallel across thousands of GPU threads, each processing one tile independently. Only the composition step is sequential, and it operates on compressed boundary data — not the full graph.

---

## The Graph

**G = (V, E)** over a bounded region of Z[i].

- **V** = Gaussian primes in the region. A point a + bi ∈ Z[i] is a Gaussian prime iff its norm a² + b² is a rational prime (for non-zero real and imaginary parts), or it is an associate of a rational prime p ≡ 3 (mod 4).
- **E** = {{p, q} : p, q ∈ V, ‖p − q‖² ≤ K} for fixed squared step bound K.

K defines the resolution of the connectivity probe. The Gaussian moat conjecture asks: for what K does G remain connected to infinity?

---

## Tile

A tile T is defined by origin (tₓ, tᵧ) ∈ Z² and three parameters:

| Parameter | Symbol | Role |
|-----------|--------|------|
| Side | S | Tile proper spans [tₓ, tₓ + S] × [tᵧ, tᵧ + S] — (S+1)² lattice points |
| Collar | C = ⌈√K⌉ | Extension beyond tile proper on all four sides |
| Halo | — | Full processing domain: tile proper + collar = (S + 1 + 2C)² lattice points |

**Edge completeness guarantee.** For any edge {p, q} ∈ E: the coordinate offset satisfies |Δ| ≤ ⌊√K⌋ ≤ C in each axis. If p lies in any tile's proper region, q lies within that tile's halo. Both endpoints survive in at least one tile's local graph.

**Shared boundary.** A tile by itself can be placed anywhere. But for composition into a structure that probes connectivity soundly and completely, tiles must be placed face-to-face — ports are defined such that they only compose correctly under face-to-face alignment. Under the (S+1)-point convention, adjacent tiles share boundary rows or columns: tile A's outer edge and tile B's inner edge are the same physical row. Both tiles' halos extend C units beyond this shared boundary into their own interiors.

---

## Faces

Each tile has four faces at the edges of its proper region — the interfaces through which tiles communicate:

| Face | Position | Along-face coordinate h | Depth predicate (tile-relative) |
|------|----------|------------------------|---------------------------------|
| **I** (inner) | bottom row | h = col | row ≤ C |
| **O** (outer) | top row | h = col | row ≥ S − C |
| **L** (left) | left column | h = row | col ≤ C |
| **R** (right) | right column | h = row | col ≥ S − C |

A **face prime** is a Gaussian prime within distance C of a face boundary.

The **collar region** of a face f is the set of Gaussian primes within perpendicular distance C of f's boundary line — the strip of primes that participate in connectivity transfer through f. For face-to-face tiles A and B sharing boundary f, every prime in the collar region is a face prime of at least one tile.

---

## Ports

After local UF runs on G_T = (V_T, E_T), face primes carry UF component labels. A **port** is an indicator that connectivity traverses the face at a specific location — evidence that a connected component crosses from the tile's interior, through the face line, to at least one prime on the collar side of the boundary.

Not every face prime produces a port. A face prime that is isolated (no edges) is not a port — there is nothing to transfer. A face prime whose UF component reaches only the interior side of the face but never crosses to the collar side is not a port — connectivity reached the face but did not traverse it.

**Construction.** Sort face primes by h (ties broken by depth, then coordinates). Scan sequentially: if consecutive face primes p, q satisfy ‖p − q‖² > K in full 2D distance, a new port begins. Single-port single-face groups — components that enter the face but traverse nowhere — are pruned.

A connected component may produce multiple ports on the same face (crossing at spatially separated locations) or ports on different faces. Ports are not one-per-group — they are independent indicators of connectivity traversal.

**Port anchor h₁.** Each port has an anchor **h₁** — the along-face coordinate of the port's first (lowest-h) prime. h₁ uniquely locates the port on the face: since ports are spatially contiguous and sorted by h, no two ports on the same face share an h₁.

Given two tiles sharing part or all of a face, h₁ is sufficient information for matching ports across the boundary. Two ports from adjacent tiles that cover the same physical boundary primes will have corresponding h₁ values (equal after accounting for the coordinate offset between tiles). This is what makes the stitch operation well-defined — a port's location on the physical boundary is fully determined by its h₁ and its tile's origin.

Each port carries two pieces of information: its **location** (h₁, face) and its **group label** — the connectivity group it belongs to. A port is a lightweight structure: a location on the face plus a pointer to its connectivity class.

---

## Groups

A **group** is an equivalence class of ports under interior connectivity. Two ports share a group iff their face primes are connected through paths within the tile's full local UF — potentially routing through interior primes that are not face primes themselves.

**Assignment.** Scan faces in order I → O → L → R, ascending h₁ within each face. First encounter of a UF component root assigns the next group ID (1-indexed, u8). A single group may span multiple faces — primes on different tile boundaries connected through the interior.

Groups are the semantic payload of the operator: they encode *which boundary regions connect to which* without revealing *how* the connection routes through the interior.

---

## The Operator

**TileOp(T)** is the complete process that takes the local subgraph G_T and outputs the compressed connectivity transfer verdict for the tile. It decomposes into two steps:

**Step 1 — Closure.** Compute cl(E_T) — the transitive closure of the local edge set via Union-Find. This step IS a closure operator (extensive, monotone, idempotent). The closure decomposition theorem — cl(E) = cl(cl(E₁) ∪ cl(E₂)) — applies to this step, enabling independent per-tile computation.

**Step 2 — Projection.** Identify which connected components traverse each face, encode these as ports grouped by interior connectivity. This step is a faithful surjection — it discards interior routing but preserves all boundary connectivity. It is NOT a closure operator (the input and output domains differ; idempotence does not typecheck).

The composition of these two steps compresses cl(E_T) into **face-to-face reachability**. Primes that carry no connectivity transfer information are discarded: interior primes with no face contact, isolated face primes, and face primes whose components never cross to the collar side of the boundary.

Among groups that survive:

- **Multi-face groups** (ports on two or more faces) — the primary carriers of connectivity transfer across the tile.
- **Single-face, multi-port groups** (multiple ports on one face, connected through the interior) — preserved. They prove that spatially separated regions of the same face connect through the tile's interior.

*Note:* Single-face, single-port groups are pruned — dead ends where a component enters the face but reaches no other face and no other port. No connectivity transfer.

The retained information: which boundary ports connect to which through the tile's interior. **A lossy compression of the tile's full graph, but a lossless compression of its boundary connectivity.**

The end result of TileOp is a lightweight structure: a collection of ports, each carrying a face, an h₁, and a group label. A tile that may contain thousands of primes and edges compresses to a handful of ports. This is what makes composition tractable — stitch operates on ports, not on graphs.

---

## Composition

### stitch(f_A, f_B)

The **stitch** operation composes two tiles A and B at a specified face pair. stitch(f_A, f_B) is defined when face f_A of tile A and face f_B of tile B share a physical boundary — fully or partially.

The **overlap region** is the intersection of the physical boundaries of f_A and f_B. For full face-to-face alignment, the overlap is the entire face. For partial overlap (e.g., adjacent towers with a y-offset), the overlap is a contiguous strip where both faces coincide. Ports with h₁ outside the overlap region are unmatched by this stitch — they are left for other stitch operations to cover.

### Two modes

**Mode 1 — Partial overlap (general case).** Faces share a partial physical boundary. Port matching requires h₁ comparison.

**Mechanics:**

1. **Wire intra-tile groups.** Within each tile, union all ports sharing a group label — encoding the internal connectivity that TileOp compressed.
2. **Match ports in the overlap region.** For each port P_A on f_A and port P_B on f_B: compute h₁ correspondence accounting for the coordinate offset between tiles. Ports whose h₁ values map to the same physical boundary location are matched.
3. **Union matched groups.** For each matched port pair, union their groups in the global UF.
4. **Output.** The combined connectivity structure. Ports outside the overlap remain unmatched — available for subsequent stitch operations.

h₁ is sufficient for matching: ports are spatially contiguous on the face, and the collar guarantee (C ≥ ⌈√K⌉) ensures both tiles see the same face primes in the overlap region.

**Mode 2 — Full overlap (aligned case).** Faces share the entire physical boundary with zero coordinate offset. Port matching by h₁ can be pruned — sequential port order suffices, since ports align by positional identity on the shared boundary row/column.

This holds for stitch(O_A, I_B) within a vertical tower, where tiles stack with zero horizontal offset. The h₁ encoding is not needed; group labels alone determine the match.

### Properties

**Associativity.** stitch is associative along a chain: stitch(R_B, L_C) after stitch(R_A, L_B) yields the same connectivity as stitching in any other order. This permits sequential processing without affecting the verdict.

**Scope.** Each stitch is faithful for connectivity through its overlap region. Faithfulness of the full boundary between two adjacent regions requires **stitch coverage** — that the union of all overlap regions across all stitched face pairs covers every crossing edge. This is a property of the tiling arrangement, not of stitch itself.

---

## Tiled Connectivity Transfer Pipeline

The full computation that applies the Tile Operator at scale.

**Input:**
- Region R ⊂ Z[i], graph G = (V, E) with squared step bound K
- Designated boundary face sets F_in (inner) and F_out (outer)

**Preconditions:**
- **Tiles** {T₁, ..., Tₙ} with side S and collar C ≥ ⌈√K⌉
- **Coverage:** every prime in R lies in at least one tile's proper region
- **Stitch coverage:** for every edge {p, q} ∈ E that crosses between adjacent tiles' regions, there exists at least one face pair (fᵢ, fⱼ) where stitch is applied and both p and q fall within the overlap's collar region

**Steps:**
1. **Partition.** Decompose R into tiles satisfying the preconditions.
2. **Transform.** Apply TileOp(Tᵢ) to each tile independently (parallelizable).
3. **Compose.** Apply stitch(fᵢ, fⱼ) for each adjacent face pair. Build global UF over all port groups.

**Output:** The connectivity structure among ports on F_in ∪ F_out — the partition of boundary ports into connected components.

**Spanning verdict:** Do any F_in ports share a component with any F_out ports? If yes — connectivity transfers, no moat. If no — moat found.

---

**Proved sound and complete:** [tile-operator-completeness.md](tile-operator-completeness.md)
**Closure decomposition foundation:** [closure-decomposition.md](closure-decomposition.md)
**Annular instantiation:** [grid-composition.md](grid-composition.md)
**Grid composition, spanning verdict, and annular scanning:** [grid-composition.md](grid-composition.md)
