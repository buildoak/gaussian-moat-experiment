# TileOp — Gaussian Primes Connectivity Transfer Operator

---

## Why TileOp Exists

Computing Union-Find over a graph with trillions of vertices cannot be done in a single pass. The graph must be partitioned into pieces that can be processed independently and in parallel. Moreover, we don't need the full UF result — we need to answer a single binary question: does ANY connectivity path span from set A (inner boundary) to set B (outer boundary)?

TileOp is the mechanism for this parallelization. It decomposes the global UF problem into local UF problems on bounded tiles, then composes the results. The key insight: when composing, we can discard all interior information and keep only the connectivity between tile interfaces. The question TileOp answers is:

> Does connectivity transfer from one group of boundary faces to another — specifically, are primes near one face connected to primes near another face through paths inside the tile?

If yes across a chain of tiles from inner boundary to outer boundary — no moat exists. If no chain connects them — a moat is found.

---

## Definition

### Acting Set

V — Gaussian primes in a bounded region of Z[i]. Edge {p, q} in E iff ||p - q||^2 <= K for fixed threshold K. Graph G = (V, E).

### Tile Geometry

A tile T_i is defined by origin (t_x, t_y) and parameters S (side), K (threshold), C (collar width).

**Tile proper:** the square [t_x, t_x + S] x [t_y, t_y + S], containing (S+1)^2 lattice points. This is the region the tile "owns."

**Collar:** an extension of C lattice units beyond the tile proper on all four sides. The collar reaches into neighboring tiles' territory. The CUDA kernel processes the full halo of (S + 1 + 2C)^2 lattice points — tile proper plus collar.

**Shared boundary:** adjacent tiles share boundary rows/columns. Under the (S+1)-point convention, Face O of tile A and Face I of tile B contain the same shared boundary row. Both tiles' kernels see this shared row plus their own collar rows extending inward.

**Constraint:** C >= ceil(sqrt(K)). This guarantees edge completeness — every edge {p, q} in E has both endpoints in at least one tile's halo.

V_i = Gaussian primes within tile proper + collar (the full halo). E_i = edges of E with both endpoints in V_i. Local UF runs on G_i = (V_i, E_i).

### Faces

Each tile has four faces at the edges of the tile proper:

- **I** (inner) — bottom boundary. Interfaces with the tile below in the tower.
- **O** (outer) — top boundary. Interfaces with the tile above.
- **L** (left) — left boundary. Interfaces with the adjacent tower.
- **R** (right) — right boundary. Interfaces with the adjacent tower.

A **face prime** is a Gaussian prime within depth C of the corresponding tile edge, on both sides of the boundary. Face primes span the shared boundary itself and extend C units into the tile proper. Both adjacent tiles see the same face primes in the overlap region — this is what makes port matching between neighbors possible.

### Ports

After local UF runs on G_i, face primes have component labels. A **port** is a maximal connected cluster of face primes on a single face.

Two face primes on the same face belong to the same port iff their full 2D squared Euclidean distance <= K — the same distance metric as the global graph edges, not just along-face separation.

Each port is anchored by its minimum along-face prime; that prime's offset from the tile origin is h1. Ports are the atomic units of boundary connectivity.

### Groups

A **group** is an equivalence class of ports under interior connectivity: two ports belong to the same group iff they are connected through paths within the tile's full local UF (including non-face interior primes).

Group labels (u8, 1-127) are assigned deterministically by scanning faces in order I, O, L, R with ascending h1 within each face.

A single group may span multiple faces — meaning primes on different faces of the tile are connected through its interior.

### The TileOp

TileOp(T_i) = the mapping from faces to ports to groups for tile T_i.

It compresses cl(E_i) — the full local equivalence relation — into interface-to-interface reachability. All interior primes that don't participate in any face port are discarded. The only information retained: which boundary ports connect to which through the tile interior.

### Composition

Given two adjacent tiles T_a, T_b sharing a face:

1. **Match ports:** Identify port pairs on the shared face that correspond to the same physical Gaussian prime. Both tiles see the same shared boundary primes (by the shared-boundary convention). A port in T_a and a port in T_b match if they contain the same boundary prime.

2. **Wire groups:** For each tile, union all ports sharing a group label.

3. **Wire matches:** Union matched port pairs across the shared face.

4. **Read connectivity:** The composed UF now represents the connectivity of the merged region, restricted to boundary primes.

Composition extends to n tiles face-to-face along towers and bands.

**Spanning check:** if any inner-boundary port (Face I of the innermost tile row) shares a UF root with any outer-boundary port (Face O of the outermost tile row), connectivity transfers edge-to-edge. No moat.

---

## Legitimacy: Three Questions

A mathematician looks at TileOp and asks:

### Q1. Why does independent UF per tile, followed by composition, equal global UF?

Start with two tiles. cl(E) = cl(cl(E_1) ∪ cl(E_2)) by the closure decomposition theorem, provided E_1 ∪ E_2 = E (edge completeness). The argument is local — each edge needs to survive in at least one tile — so it extends to n tiles without modification.

→ Uses [closure-decomposition.md](closure-decomposition.md).  
→ Edge completeness proved in Q2.

### Q2. Why does the collar guarantee no edge is lost?

For any edge {p, q} with ||p - q||^2 <= K: the offset in any single coordinate satisfies |delta| <= floor(sqrt(K)). With C >= ceil(sqrt(K)), if p is in any tile's proper region, q is within that tile's collar. Both endpoints in V_i. Edge survives in E_i.

Therefore ∪E_i = E. Edge completeness holds.

### Q3. Why is the port/group compression faithful?

The outer closure in the decomposition theorem — cl(∪cl(E_i)) — only needs to know which boundary primes connect to which other boundary primes through each tile's interior. Interior-only primes are invisible to the merge step; they can be connected or disconnected without affecting the global result, because their edges are fully contained within one tile and already resolved by local UF.

The port/group encoding captures exactly this: ports identify boundary clusters, groups identify which ports connect through the interior. This is the complete information the compositor needs. No less (every boundary connection is represented), no more (interior details are correctly discarded).

→ Port fidelity to be formalized.

---

**Depends on:** [closure-decomposition.md](closure-decomposition.md)  
**Answers:** Is TileOp a legitimate way to compute the connectivity verdict for the Gaussian moat problem?
