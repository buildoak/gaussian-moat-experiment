# Tile Operator — Soundness and Completeness

> **Theorem.** Given tile composition arranged face-to-face, the Tile Operator faithfully preserves the full connectivity structure between collar regions of designated boundary faces — producing the same connected components among boundary primes as full Union-Find over the tiled region.

**Sound:** if the tiled computation reports connectivity between collar regions, that connectivity exists in G_full.
**Complete:** if connectivity exists in G_full between collar regions of boundary faces, the tiled computation will detect it.
**Faithful** = sound + complete — not just a binary verdict, but the exact equivalence-class structure among boundary primes.

All definitions — G, tile, face, port, group, TileOp (two-step: closure + projection), stitch(f_A, f_B) — are in [tile-operator-definition.md](tile-operator-definition.md).

---

## Lemma 0 — Port-Region Equivalence

**Statement.** Connectivity between collar regions of two faces is equivalent to connectivity between port sets on those faces.

> "Ports on face f₁ connect to ports on face f₂ through stitched tiles" **⟺** "There exists a path in G_full between primes in the collar region of f₁ and primes in the collar region of f₂"

**Proof.**

**(⇒) Port → Region (soundness direction).** Ports correspond to real Gaussian primes with real UF connections. If stitched ports report connectivity from f₁ to f₂, the underlying primes — which lie in the collar regions of f₁ and f₂ — are connected in G_full. stitch only unions real matches; TileOp only reports connections found by UF on real edges. The path exists. □

**(⇐) Region → Port (completeness direction).** Suppose a path P in G_full connects a prime p₁ in the collar region of f₁ to a prime p₂ in the collar region of f₂, passing through a tiled region.

At every tile face the path crosses, the connected component traverses that face — crossing from the interior side to the collar side. By the port definition, this traversal produces a port. Within each tile, the path connects its entry port to its exit port through the interior — giving them the same group. At each shared boundary, stitch matches the corresponding ports.

Therefore the path P is fully represented in the port/stitch framework: a chain of ports connected by groups (within tiles) and by stitch matches (across tiles). The port connectivity from f₁ to f₂ is detected. □

**Consequence.** This equivalence lets us reduce the physical question (connectivity between collar regions) to the algebraic question (connectivity between port sets). The rest of the proof works in the port domain.

---

## Theorem (Port Domain)

By Lemma 0, the Theorem reduces to:

> Given tile composition arranged face-to-face, stitch faithfully preserves the full connectivity structure between port sets of designated boundary faces — producing the same connected components among boundary ports as full Union-Find over the tiled region.

This decomposes into three claims at increasing granularity:

1. **Single tile:** TileOp(T) is faithful for connectivity between port sets of any two faces of T. *(Lemma 1)*
2. **Two tiles:** stitch(R_A, L_B) is faithful for connectivity between port sets of L_A and R_B. *(Lemma 2)*
3. **n tiles:** Composed stitches are faithful for connectivity between inner boundary ports and outer boundary ports. *(Corollary)*

---

## Proposition — Graph Sufficiency

**Statement.** G_sufficient = ∪G_Tᵢ is sufficient for determining connectivity between port sets on boundary faces. G_sufficient may lose edges and vertices from G_full, but it preserves all edges responsible for connectivity transfer between ports.

**Proof.**

G_sufficient is not claimed to be equal to G_full. Edges between primes that lie outside every tile's proper region may be absent. What we show: no edge relevant to port-to-port connectivity is lost.

For any face-to-face tile pair (A, B) sharing boundary F: the collar guarantee (C ≥ ⌈√K⌉) ensures that every edge {p, q} crossing F has both endpoints within the halo of both tiles. Both tiles capture these edges. Within each tile, every edge incident to a prime in the tile's proper region is captured (if p is in the proper region, any neighbor q within distance √K falls within the collar).

Primes that are not in any tile's proper region are not face primes of any tile, hence cannot be ports. They may serve as intermediaries on paths in G_full, but for face-to-face tiles, such primes lie in the collar of some tile T and — by the face-to-face placement — in the proper region of T's neighbor. Their edges are captured.

Therefore: for any query "are ports on face f_A connected to ports on face f_B?", G_sufficient yields the same answer as G_full. □

---

## Lemma 1 — Single-Tile Faithfulness

**Statement.** TileOp(T) is faithful for connectivity between port sets of any two faces of tile T.

**Proof.** Follows the two-step decomposition of TileOp:

**Step 1 (Closure) is faithful.** Local UF computes cl(E_T) — the exact transitive closure within T's halo. By edge completeness, every edge incident to a prime in T's proper region has both endpoints in V_T. The closure resolves all connectivity within the halo completely. No information is lost or fabricated.

**Step 2 (Projection) is faithful for port-to-port connectivity.** The projection encodes which connected components traverse which faces, as ports grouped by interior connectivity. We show nothing is lost:

- **Face-traversing components** — those crossing from the tile's interior through a face boundary to the collar side — are captured as ports by definition. Their group labels encode which ports connect through the interior. All port-to-port connectivity is preserved.

- **Non-traversing face primes** — isolated face primes, and face primes whose components reach the face but never cross to the collar side — are pruned. This is sound: they carry no connectivity transfer information. A component that never crosses a face cannot contribute to any port-to-port path.

- **Interior-only primes** — those not within distance C of any face — are discarded. This is sound: any path between ports that routes through interior primes was already resolved by Step 1. The group label already encodes the connection. Interior primes were intermediaries, not endpoints.

- **Single-face, single-port groups** — pruned. Dead ends: the component enters one face but reaches no other face and no other port. Cannot participate in connectivity transfer across the tile.

Step 1 preserves all connectivity. Step 2 preserves all port-to-port connectivity. TileOp(T) is faithful. □

---

## Lemma 2 — Stitch Faithfulness

**Statement.** stitch(R_A, L_B) is faithful for connectivity from port sets of L_A to port sets of R_B, given adjacent tiles A and B sharing the physical boundary R_A ≡ L_B.

### Soundness

stitch reports only connections found by the four-step mechanics defined in [tile-operator-definition.md](tile-operator-definition.md): wire intra-tile groups (real UF connections), match ports on the shared boundary (real spatial correspondence), union matched groups. No step creates edges absent from G_full.

If stitch(R_A, L_B) reports connectivity from L_A to R_B, that path exists in G_full. □

### Completeness

We must show: every path in G_full from a face L_A prime to a face R_B prime is captured by stitch(R_A, L_B). The shared boundary R_A ≡ L_B is the only place where information could be lost. We analyze all possible situations for Gaussian primes at or near this shared boundary.

#### Part I — Non-ports lose no information

These cases follow directly from the port definition: primes that don't produce ports don't traverse the face, therefore cannot participate in any L_A → R_B path.

**Case 1 — Isolated face prime.** A Gaussian prime on the shared boundary with no neighbors within distance √K. By the port definition, it is not a port — there is nothing to transfer, no path through it. ✓

**Case 2 — One-sided face prime.** A Gaussian prime on the shared boundary whose UF component reaches only the interior side of one tile but never crosses to the collar side. By the port definition, it is not a port — connectivity reached the face but did not traverse it.

This prime cannot participate in an L_A → R_B path: any such path through this prime would require connectivity on both sides of the shared boundary — which would make it a traversal (Case 3 or 4), not a one-sided contact. A one-sided component is a dead end at the boundary. ✓

#### Part II — Ports capture all traversals

These are the load-bearing cases. Every path in G_full that crosses the shared boundary must pass through primes that traverse the face — and we show that stitch captures them all.

**Case 3 — Face prime traversing the boundary.** A Gaussian prime p on the shared boundary is connected to UF components in both tile A and tile B — connectivity traverses the face through p.

*Geometric guarantee:* Every neighbor q of p satisfies ‖p − q‖² ≤ K, hence |Δx|, |Δy| ≤ ⌊√K⌋ ≤ C. Since p is on the shared boundary and the collar extends C units into each tile's interior:

- Tile A's halo includes p and all its neighbors
- Tile B's halo includes p and all its neighbors

Both tiles see p with its complete edge neighborhood. Both tiles' UF correctly resolves p's connectivity to their respective interiors. Both tiles encode p as a port on their side of the shared face (p traverses the face — it meets the port definition in both tiles).

stitch(R_A, L_B) matches these ports by h₁ and unions their groups. The full path through p — from A's interior through p into B's interior — is preserved. ✓

**Case 4 — Off-boundary traversal.** A path in G_full crosses the shared boundary through primes near (but not exactly on) the boundary line.

*Geometric guarantee:* Any edge {p, q} crossing the boundary has ‖p − q‖² ≤ K, so |Δ| ≤ ⌊√K⌋ ≤ C in each coordinate. Both p and q are within distance C of the shared boundary. Both are face primes of both tiles. Both tiles capture edge {p, q} in their local graphs. Since the component crosses the boundary, it traverses the face — both tiles encode the crossing primes as ports.

stitch(R_A, L_B) matches these ports and unions the corresponding groups.

*Extension to paths:* Any path crossing the boundary is a sequence of edges. Each edge either lies entirely within one tile's halo (resolved by local UF in Step 1) or crosses the boundary (captured by both tiles via the collar guarantee, matched by stitch). The full path is preserved. ✓

### Cases are exhaustive

Every Gaussian prime near the shared boundary is either:
- Isolated (Case 1) — not a port, no connectivity
- Connected to one side only (Case 2) — not a port, dead end
- Connected to both sides (Case 3) — a port, captured by stitch
- Part of an off-boundary crossing (Case 4) — a port, captured by stitch

No configuration is uncovered. Soundness and completeness established.

**stitch(R_A, L_B) is faithful for connectivity from port sets of L_A to port sets of R_B.** □

---

## Corollary — n-Tile Composition

By induction on n. Base case (n = 1): faithful by Lemma 1. Inductive step: the result of (n−1) faithful stitches can itself be stitched with a new adjacent tile — this is a single stitch operation, faithful by Lemma 2.

By associativity of stitch, the order of stitching along a chain does not affect the verdict. n-tile composition preserves port-to-port connectivity.

By Lemma 0 (Port-Region Equivalence), port-to-port connectivity is equivalent to collar-region connectivity. Therefore the composed stitches faithfully determine connectivity between collar regions of the inner and outer boundary faces — which is the Theorem.

The spanning verdict and the annular scanning pattern that arranges tiles into towers are described in [grid-composition.md](grid-composition.md). □

---

## Remark — Port Definition and Collar Completeness

The port definition requires that connectivity traverses the face — crossing from the tile's interior to at least one prime on the collar side. This means "connectivity transfer through a face" inherently accounts for the full collar overlap region. Both adjacent tiles see the same face primes in this overlap, which is what makes port matching in stitch well-defined and complete.

A separate proof that "face connectivity equals face + collar connectivity" is not needed — the port definition already requires crossing to the collar side. The collar is not auxiliary to the face; it is what gives the face its depth.

---

**Definition:** [tile-operator-definition.md](tile-operator-definition.md)
**Depends on:** [closure-decomposition.md](closure-decomposition.md)
**Grid composition:** [grid-composition.md](grid-composition.md)
