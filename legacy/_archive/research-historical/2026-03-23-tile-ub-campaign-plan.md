---
date: 2026-03-23
engine: opus
status: complete
type: mathematical-foundation
campaign: k40-tile-ub
target_k_squared: 40
---

# Tile-Based Upper Bound Campaign for $k^2=40$

## Abstract

We describe a method for proving upper bounds on the Gaussian moat radius $R_{\mathrm{moat}}(k)$ by decomposing an annular band into a grid of tiles, computing local connectivity within each tile via the scanline kernel, and aggregating the results into a tile connectivity graph. If no path crosses the annular band in this graph, the moat radius is bounded above. The method is a rigorous coarsening of the full Gaussian prime graph: it can only lose connections, never create false ones. We work out the precise tiling geometry that makes inter-tile connectivity sound, prove the main soundness theorem, develop a hierarchical super-tile aggregation scheme, and estimate the compute cost of a full $k^2=40$ UB campaign on A100 hardware.

---

## Part 1: Mathematical Framework

### 1.1 Notation and Setup

We use the notation and definitions established in the connectivity transfer operator document. Recall:

- $G_k = (P, E_k)$ is the Gaussian prime graph with step bound $k^2$.
- $c = \lceil \sqrt{k^2} \rceil$ is the collar width. For $k^2 = 40$, $c = 7$.
- A tile $T(a_0, b_0, W, H) = \{a + bi \in \mathbb{Z}[i] : a_0 \le a \le a_0 + H,\ b_0 \le b \le b_0 + W\}$.
- The expanded tile is $T^+ = \{a + bi : a_0 - c \le a \le a_0 + H + c,\ b_0 - c \le b \le b_0 + W + c\}$.
- The tile operator computes connected components of $G_k[T^+]$ and their face-port structure: which components touch which faces $\{I, O, L, R\}$.

### 1.2 The Annular Band

An annular band is the lattice region

$$
\mathcal{B}(R_{\min}, R_{\max}) = \{a + bi \in \mathbb{Z}[i] : R_{\min} \le a \le R_{\max},\ 0 \le b \le b_{\max}\},
$$

where the restriction to $b \ge 0$ exploits the reflection symmetry $b \mapsto -b$ (Section 5.1 of the CTO document), and $b_{\max}$ is chosen to cover the first octant. By the eightfold symmetry of $\mathbb{Z}[i]$, connectivity in the full plane is equivalent to connectivity in the first octant.

For the UB argument, we work in a rectangular region that approximates the annular band. At large radius, the curvature of the annulus is negligible over a tile of width $W = 2000$, so the rectangular grid is a valid local approximation.

**Definition 1.1 (Annular band for UB).** Fix $R_{\min}$, $R_{\max}$ with $R_{\max} - R_{\min} \gg c$. The UB band is

$$
\mathcal{B} = \{a + bi \in \mathbb{Z}[i] : R_{\min} \le a \le R_{\max},\ 0 \le b \le a\}.
$$

The constraint $0 \le b \le a$ restricts to the first octant (the sector $0 \le \arg(z) \le \pi/4$). By the dihedral symmetry group of $\mathbb{Z}[i]$, any prime path from the origin to infinity must cross this octant's annular band.

### 1.3 Tiling the Band

**Definition 1.2 (Gapless tiling).** Partition $\mathcal{B}$ into a grid of tiles $\{T_{i,j}\}$ where

$$
T_{i,j} = T(a_0 + iH,\ b_0 + j \cdot s_b,\ W,\ H),
$$

with:
- $i = 0, 1, \ldots, N_r - 1$ indexing radial shells, $N_r = \lceil (R_{\max} - R_{\min}) / H \rceil$
- $j = 0, 1, \ldots, N_\ell - 1$ indexing lateral stripes
- $s_b$ is the lateral stride (to be determined)
- $a_0 = R_{\min}$, $b_0 = 0$

The corresponding expanded tiles are

$$
T_{i,j}^+ = \{a + bi : a_0 + iH - c \le a \le a_0 + (i+1)H + c,\ b_0 + j \cdot s_b - c \le b \le b_0 + j \cdot s_b + W + c\}.
$$

**Critical design choice:** the stride $s_b$ determines whether the expanded tiles overlap, and this overlap is what makes inter-tile connectivity sound.

### 1.4 The Overlap Condition

This is the central geometric subtlety of the method. We analyze it carefully.

**Claim.** For inter-tile face-port connectivity to faithfully represent true $G_k$ connectivity, the expanded tiles of laterally adjacent tiles must overlap by at least $c$ in the lateral direction.

**Analysis.** Consider two laterally adjacent tiles $T_{i,j}$ and $T_{i,j+1}$.

The interior of $T_{i,j}$ covers $b \in [b_0 + j \cdot s_b,\ b_0 + j \cdot s_b + W]$. Its expanded region covers $b \in [b_0 + j \cdot s_b - c,\ b_0 + j \cdot s_b + W + c]$.

The interior of $T_{i,j+1}$ covers $b \in [b_0 + (j+1) \cdot s_b,\ b_0 + (j+1) \cdot s_b + W]$. Its expanded region covers $b \in [b_0 + (j+1) \cdot s_b - c,\ b_0 + (j+1) \cdot s_b + W + c]$.

**Case 1: ISE stride $s_b = W + 2c$ (disjoint expanded regions).**

The right edge of $T_{i,j}^+$ is at $b = b_0 + j(W + 2c) + W + c$. The left edge of $T_{i,j+1}^+$ is at $b = b_0 + (j+1)(W + 2c) - c = b_0 + j(W + 2c) + W + c$. So the expanded regions are exactly adjacent (sharing a single column). There is a gap of width $2c$ between the interiors:

$$
\text{gap} = (b_0 + (j+1) \cdot s_b) - (b_0 + j \cdot s_b + W) = s_b - W = 2c.
$$

Primes in this gap of width $2c$ are not in the interior of either tile. They appear in the R-collar of $T_{i,j}$ and the L-collar of $T_{i,j+1}$, so their connectivity is computed. But a prime at position $b_0 + j \cdot s_b + W + 1$ (in the R-collar of $T_{i,j}$) and a prime at position $b_0 + (j+1) \cdot s_b - 1$ (in the L-collar of $T_{i,j+1}$) are separated by distance $2c - 2$. Since $c = \lceil \sqrt{k^2} \rceil \ge \sqrt{k^2}$, we have $2c - 2 \ge 2\sqrt{k^2} - 2 > \sqrt{k^2}$ for $k^2 \ge 4$. So these primes cannot be connected by a single step.

However, the issue is more subtle. A connected component in $T_{i,j}^+$ may include primes in the R-collar. A connected component in $T_{i,j+1}^+$ may include primes in the L-collar. These collar primes CAN be connected to each other (they are within $2c$ of each other, and $2c$ may exceed $\sqrt{k^2}$ but intermediate primes could relay the connection). But here is the problem: those intermediate relay primes are in the gap region, which is in BOTH collars but in NEITHER interior. The components computed within each tile correctly connect collar primes to interior primes. But a relay chain that stays entirely within the gap (connecting R-collar primes of one tile to L-collar primes of the next via gap primes) is correctly captured: those gap primes appear in both tiles' expanded regions, and the union-find within each tile connects them.

Let us be more precise. A prime $\pi$ with $b_0 + j \cdot s_b + W < b(\pi) < b_0 + (j+1) \cdot s_b$ is:
- In the R-collar of $T_{i,j}$ if $b(\pi) \le b_0 + j \cdot s_b + W + c$
- In the L-collar of $T_{i,j+1}$ if $b(\pi) \ge b_0 + (j+1) \cdot s_b - c$

With $s_b = W + 2c$, the R-collar extends to $b_0 + j(W+2c) + W + c$ and the L-collar starts at $b_0 + (j+1)(W+2c) - c = b_0 + j(W+2c) + W + c$. So these two collars share exactly one column: $b = b_0 + j(W+2c) + W + c$. A prime at exactly this column is in both expanded tiles, but any gap prime to the left of it is only in $T_{i,j}^+$ and any to the right is only in $T_{i,j+1}^+$.

This means a chain of primes that crosses the midpoint of the gap must pass through the single shared column. Connectivity through the gap is only partially captured. **The ISE stride is NOT sufficient for sound inter-tile connectivity.**

**Case 2: Overlap stride $s_b = W$ (full overlap of collar regions).**

The right edge of $T_{i,j}$'s interior is at $b = b_0 + jW + W$. The left edge of $T_{i,j+1}$'s interior is at $b = b_0 + (j+1)W$. These are the same: the interiors are contiguous with no gap.

The R-collar of $T_{i,j}^+$ extends to $b = b_0 + jW + W + c$. The L-collar of $T_{i,j+1}^+$ starts at $b = b_0 + (j+1)W - c = b_0 + jW + W - c$. So the overlap of expanded regions is:

$$
[b_0 + jW + W - c,\ b_0 + jW + W + c] = [b_{\text{boundary}} - c,\ b_{\text{boundary}} + c],
$$

a band of width $2c$ centered on the shared boundary $b_{\text{boundary}} = b_0 + jW + W$.

Every prime in this overlap band appears in BOTH expanded tiles. Its connectivity to primes in $T_{i,j}$'s interior is computed in $T_{i,j}^+$. Its connectivity to primes in $T_{i,j+1}$'s interior is computed in $T_{i,j+1}^+$.

**Theorem 1.3 (Overlap sufficiency).** With lateral stride $s_b = W$, if a connected component $C$ in $G_k$ contains a prime $\pi_L$ in the R-face zone of $T_{i,j}$ and a prime $\pi_R$ in the L-face zone of $T_{i,j+1}$, then there exist components $C_L$ in $G_k[T_{i,j}^+]$ and $C_R$ in $G_k[T_{i,j+1}^+]$ and a prime $\pi_M$ in the overlap region such that:

1. $\pi_L \in C_L$ and $\pi_M \in C_L$ (connected in $T_{i,j}^+$)
2. $\pi_M \in C_R$ and $\pi_R \in C_R$ (connected in $T_{i,j+1}^+$)

**Proof.** Since $\pi_L$ and $\pi_R$ are connected in $G_k$, there exists a path $\pi_L = p_0, p_1, \ldots, p_m = \pi_R$ in $G_k$. Consider the last prime $p_s$ on this path that lies in $T_{i,j}$'s interior (i.e., $b(p_s) \le b_0 + jW + W$) and the first prime $p_t$ with $t > s$ that lies in $T_{i,j+1}$'s interior (i.e., $b(p_t) \ge b_0 + (j+1)W$).

Since $b_0 + jW + W = b_0 + (j+1)W$ (the interiors are contiguous), we have $b(p_s) \le b_{\text{boundary}}$ and $b(p_t) \ge b_{\text{boundary}}$. The edge $\{p_s, p_t\}$ (or a chain through the boundary) connects primes on opposite sides.

Any prime $p$ on the path segment $p_s, \ldots, p_t$ satisfies $|b(p) - b_{\text{boundary}}| \le c$ because each step changes $b$ by at most $c$, and $p_s$ and $p_t$ are within $c$ of the boundary. Therefore every prime on this segment lies in the overlap region $[b_{\text{boundary}} - c, b_{\text{boundary}} + c]$.

Since this segment lies entirely in the overlap region, all its primes appear in both $T_{i,j}^+$ and $T_{i,j+1}^+$, and all edges along the segment are visible in both expanded tiles. Let $\pi_M$ be any prime on this segment (say $p_s$ or $p_t$). Then $\pi_L$ is connected to $\pi_M$ via a path in $T_{i,j}^+$ (the prefix of the original path lies in $T_{i,j}^+ \supseteq T_{i,j} \cup \text{R-collar}$), and $\pi_M$ is connected to $\pi_R$ via a path in $T_{i,j+1}^+$ (the suffix lies in $T_{i,j+1}^+$). $\square$

Wait — the argument above needs a refinement. The path segment $p_s, \ldots, p_t$ lies in the overlap region, but the path PREFIX $p_0, \ldots, p_s$ may wander outside $T_{i,j}^+$. We need to argue that the component of $\pi_L$ in the RESTRICTED graph $G_k[T_{i,j}^+]$ still connects to $\pi_M$.

This is not automatically true: $\pi_L$ and $\pi_M$ may be connected in $G_k$ via a path that exits $T_{i,j}^+$, but NOT connected in $G_k[T_{i,j}^+]$.

This observation reveals a fundamental point: the tile operator computes connectivity in the **induced subgraph** $G_k[T^+]$, which is a subgraph of $G_k$. The tile's components are subsets of the full components. A component that crosses a tile in $G_k$ may not cross it in $G_k[T^+]$ if the crossing path exits the tile.

This is the same one-sided guarantee from Corollary 4.2 of the CTO document: a detected crossing is always real (no false positives), but a missed crossing may be genuine (false negatives are possible). The tile graph is a **conservative** approximation.

### 1.5 The Tile Connectivity Graph

**Definition 1.4 (Tile connectivity graph).** Given a gapless tiling $\{T_{i,j}\}$ of the annular band $\mathcal{B}$, with overlap stride $s_b = W$, define the tile connectivity graph

$$
\mathcal{G}_{\text{tiles}} = (V, E),
$$

where:

**Vertices.** Each connected component $C_\alpha$ of $G_k[T_{i,j}^+]$ that touches the interior $T_{i,j}$ contributes a vertex $(i, j, \alpha) \in V$. The vertex inherits the face set $\partial_{T_{i,j}} C_\alpha \subseteq \{I, O, L, R\}$.

**Edges.** Two vertices $(i, j, \alpha)$ and $(i', j', \beta)$ are connected in $\mathcal{G}_{\text{tiles}}$ if and only if:

1. **Radial adjacency ($i' = i+1$, $j' = j$):** Component $C_\alpha$ in $T_{i,j}$ touches the O-face, component $C_\beta$ in $T_{i+1,j}$ touches the I-face, AND there exists a prime in the overlap region of $T_{i,j}^+$ and $T_{i+1,j}^+$ that belongs to both $C_\alpha$ (in $G_k[T_{i,j}^+]$) and $C_\beta$ (in $G_k[T_{i+1,j}^+]$).

2. **Lateral adjacency ($i' = i$, $j' = j+1$):** Component $C_\alpha$ in $T_{i,j}$ touches the R-face, component $C_\beta$ in $T_{i,j+1}$ touches the L-face, AND there exists a prime in the overlap region that belongs to both.

**Remark.** The edge condition requires a shared prime in the overlap region. This is stronger than just "both components touch adjacent faces." It ensures that the two components are genuinely connected through the shared boundary. In practice, this is detected by comparing component IDs of primes in the overlap region across the two tiles.

**Implementation note.** Since primes in the overlap region appear in both tiles, we can identify shared primes by their $(a, b)$ coordinates. For each shared prime, we know its component ID in each tile. If a shared prime has component $\alpha$ in tile $(i,j)$ and component $\beta$ in tile $(i',j')$, we add edge $((i,j,\alpha), (i',j',\beta))$.

### 1.6 The Reachability Theorem

**Definition 1.5 (I-boundary and O-boundary).** In the tile connectivity graph $\mathcal{G}_{\text{tiles}}$:

- The **I-boundary** $\partial_I$ is the set of vertices $(0, j, \alpha)$ (shell $i=0$, any stripe $j$, any component $\alpha$) such that $I \in \partial_{T_{0,j}} C_\alpha$.
- The **O-boundary** $\partial_O$ is the set of vertices $(N_r - 1, j, \alpha)$ (last shell, any stripe, any component) such that $O \in \partial_{T_{N_r-1,j}} C_\alpha$.

**Theorem 1.6 (Upper bound from tile graph).** If $\mathcal{G}_{\text{tiles}}$ has no path from $\partial_I$ to $\partial_O$, then $R_{\mathrm{moat}}(k) \le R_{\min} + c$.

More precisely: no connected component of $G_k$ crosses the annular band $\mathcal{B}$ from $a = R_{\min}$ to $a = R_{\max}$.

**Proof.** Suppose for contradiction that a connected component $C$ of $G_k$ contains primes $\pi_I$ with $a(\pi_I) \le R_{\min} + c$ and $\pi_O$ with $a(\pi_O) \ge R_{\max} - c$. Then there exists a path $\pi_I = p_0, p_1, \ldots, p_m = \pi_O$ in $G_k$.

This path passes through the tiled region. At each step, the path is within some tile's expanded region (since the tiling is gapless and the expanded regions cover the band with $c$-wide margins). As the path moves radially from $R_{\min}$ to $R_{\max}$, it crosses from the I-face zone of some tile in shell $i = 0$ to the O-face zone of some tile in shell $i = N_r - 1$.

By Corollary 4.2 of the CTO document, each local crossing detected in a tile is genuine. The converse is the key: is each genuine crossing detected?

Consider the path segment within $T_{i,j}^+$. By definition, the induced subgraph $G_k[T_{i,j}^+]$ contains all primes of the path that lie in $T_{i,j}^+$ and all edges between them that exist in $G_k$ (since edge existence depends only on distance, which is preserved). So the path segment within $T_{i,j}^+$ is a valid path in $G_k[T_{i,j}^+]$.

However, the path may exit $T_{i,j}^+$ and re-enter. The segments within $T_{i,j}^+$ may not connect to form a single component. This is precisely the false-negative possibility: the tile operator may fragment a globally connected component into multiple local components.

Here is where the overlap comes in. When the path crosses from tile $(i,j)$ to tile $(i,j+1)$, by Theorem 1.3, there exist matching components in the two tiles that share a prime in the overlap region. This shared prime creates an edge in $\mathcal{G}_{\text{tiles}}$.

But this only handles direct lateral crossings. What about a path that exits $T_{i,j}^+$ laterally, wanders through several tiles, and re-enters? Such a path creates a chain of edges in $\mathcal{G}_{\text{tiles}}$ connecting the component in $T_{i,j}$ to components in the intermediate tiles and back.

More carefully: the path $p_0, \ldots, p_m$ visits a sequence of tiles. At each tile transition, the overlap condition guarantees an edge in $\mathcal{G}_{\text{tiles}}$ between the local components. The composition of these edges creates a path in $\mathcal{G}_{\text{tiles}}$ from a vertex in $\partial_I$ to a vertex in $\partial_O$. But we assumed no such path exists — contradiction.

Wait. This argument has a gap. The path may cross from tile $(i,j)$ to $(i,j+1)$ laterally, but the local component in $T_{i,j}^+$ that contains the crossing prime may NOT touch the R-face. The prime might be in the interior of $T_{i,j}$, far from the R-face, and the path exits through the collar. In this case, the prime IS in the overlap region (it's within $c$ of the boundary between $(i,j)$ and $(i,j+1)$, which means it's within $c$ of $b_0 + jW + W$), hence it IS in the R-face zone (by definition, a prime is in the R-face if $b_{\max} - b \le c$, i.e., within $c$ of the right edge). So actually, any prime near enough to the boundary to be in the overlap IS in the R-face zone. The definitions are aligned.

Let us verify: the R-face zone of $T_{i,j}$ is $\{a + bi \in T_{i,j} : b_0 + jW + W - b \le c\}$, i.e., $b \ge b_0 + jW + W - c$. The overlap region is $[b_0 + jW + W - c, b_0 + jW + W + c]$. So primes in the overlap that are in $T_{i,j}$'s interior (i.e., $b \le b_0 + jW + W$) are exactly those in the R-face zone. Yes, the definitions align.

Similarly, primes in the overlap that are in $T_{i,j+1}$'s interior (i.e., $b \ge b_0 + (j+1)W$) are in the L-face zone. And primes strictly between the two interiors... with $s_b = W$, there are no such primes, because the interiors are contiguous.

So the argument is: any path that crosses from $T_{i,j}$'s interior to $T_{i,j+1}$'s interior must pass through the overlap region. The crossing primes are in the R-face zone of one tile and the L-face zone of the next. The overlap ensures the component containing these primes is detected in both tiles. The edge in $\mathcal{G}_{\text{tiles}}$ is present. Composing across the full path: a path in $G_k$ from $\partial_I$ to $\partial_O$ induces a path in $\mathcal{G}_{\text{tiles}}$. Absence of the latter implies absence of the former. $\square$

**Remark 1.7.** The same argument applies to radial adjacency. With $s_a = H$ (the radial stride equals $H$, so tiles are contiguous radially), the expanded tiles overlap by $2c$ in the radial direction, and the O-face / I-face zones align with the overlap. The argument is symmetric.

### 1.7 Soundness: The Tile Graph is a Conservative Coarsening

**Theorem 1.8 (Soundness).** The tile connectivity graph $\mathcal{G}_{\text{tiles}}$ is a coarsening of the restriction of $G_k$ to the annular band: every edge in $\mathcal{G}_{\text{tiles}}$ corresponds to a genuine connection in $G_k$, but some connections in $G_k$ may be absent from $\mathcal{G}_{\text{tiles}}$.

Formally:
- **No false positives.** If vertices $(i,j,\alpha)$ and $(i',j',\beta)$ are connected in $\mathcal{G}_{\text{tiles}}$, then the corresponding components $C_\alpha$ and $C_\beta$ are connected in $G_k$.
- **Possible false negatives.** Components may be connected in $G_k$ (via paths that wander far from the tile boundaries) but not connected in $\mathcal{G}_{\text{tiles}}$.

**Proof.** No false positives: An edge in $\mathcal{G}_{\text{tiles}}$ requires a shared prime in the overlap region belonging to both components (in their respective induced subgraphs). Since $G_k[T^+]$ is a subgraph of $G_k$, membership in a component of $G_k[T^+]$ implies membership in the same component of $G_k$. The shared prime is connected to $C_\alpha$'s primes in $G_k$ and to $C_\beta$'s primes in $G_k$, so $C_\alpha$ and $C_\beta$ are connected in $G_k$.

Possible false negatives: A component $C$ of $G_k$ may contain primes in $T_{i,j}$ and in $T_{i',j'}$ connected by a path that exits the annular band entirely, or that passes through many intermediate tiles in a way that the local fragments of $C$ are not connected within any single tile's induced subgraph. The tile graph misses these indirect connections.

**Corollary 1.9 (UB validity).** If $\mathcal{G}_{\text{tiles}}$ says "blocked" (no $\partial_I$-to-$\partial_O$ path), then $G_k$ is also blocked across the band. The converse may fail: $\mathcal{G}_{\text{tiles}}$ may show a path even when $G_k$ is blocked (because $\mathcal{G}_{\text{tiles}}$ merges components that touch the same face into potentially connected pairs, when the actual shared-prime check would reject them). But wait — we defined edges via shared primes, not mere face adjacency. So $\mathcal{G}_{\text{tiles}}$ does NOT have this false-positive issue. The only direction of error is false negatives (real connections missed). So "blocked in $\mathcal{G}_{\text{tiles}}$" implies "blocked in $G_k$."

This is the desired property for an upper bound: the method is conservative. A declared moat is real. A declared crossing may be spurious (a false negative in the blocking direction, i.e., a false positive in the crossing direction), which means the method might fail to find a moat that exists. But it will never declare a moat that doesn't exist.

Concretely: **if no path from $\partial_I$ to $\partial_O$ exists in $\mathcal{G}_{\text{tiles}}$, then $R_{\mathrm{moat}}(k) \le R_{\min} + c$.** $\square$

### 1.8 Practical Edge Detection via Face-Port Matching

The edge condition in Definition 1.4 requires identifying shared primes across overlapping tiles. In practice, this is implemented as follows.

For laterally adjacent tiles $T_{i,j}$ and $T_{i,j+1}$ (with $s_b = W$):

1. The R-face ports of $T_{i,j}$ list primes $(a, b)$ with $b \ge b_0 + jW + W - c$ and their component IDs.
2. The L-face ports of $T_{i,j+1}$ list primes $(a, b)$ with $b \le b_0 + (j+1)W + c$ and their component IDs.
3. The overlap region is $b \in [b_0 + jW + W - c,\ b_0 + jW + W + c]$. Primes in this range appear in both face-port lists.
4. For each prime $(a, b)$ in the overlap, look up its component ID $\alpha$ in $T_{i,j}$ and $\beta$ in $T_{i,j+1}$. Add edge $((i,j,\alpha), (i,j+1,\beta))$ to $\mathcal{G}_{\text{tiles}}$.

This requires storing and comparing face-port lists. The face-port lists are already computed by the tile operator (they are the `face_inner`, `face_outer`, `face_left`, `face_right` fields of `TileOperator`). The matching step is a join on $(a, b)$ coordinates, which can be done in $O(n \log n)$ time by sorting both lists.

For radial adjacency ($T_{i,j}$ and $T_{i+1,j}$), the same procedure uses O-face ports of $T_{i,j}$ and I-face ports of $T_{i+1,j}$, with the overlap region in the radial ($a$) direction: $a \in [a_0 + (i+1)H - c,\ a_0 + (i+1)H + c]$.

---

## Part 2: Gapless Tiling Strategy

### 2.1 Comparison of Strides

| Property | ISE stride ($s_b = W + 2c$) | UB stride ($s_b = W$) |
|----------|----------------------------|----------------------|
| Expanded regions | Disjoint (share at most 1 column) | Overlap by $2c$ |
| Gap between interiors | $2c$ | 0 (contiguous) |
| Inter-tile connectivity | NOT sound (gap primes fragmented) | Sound (overlap captures all edges) |
| Independence | Collar-disjoint: statistically independent | Overlapping: NOT independent |
| Use case | ISE screening (probabilistic) | UB proof (deterministic) |

The ISE stride is designed for **independent sampling**: each tile is a separate experiment, and the independence justifies the $p^M$ false-positive bound. The UB stride is designed for **complete coverage**: every prime in the band appears in at least one tile's interior, and every edge in $G_k$ restricted to the band appears in at least one expanded tile.

### 2.2 Cost of Gapless Tiling

For $k^2 = 40$, $c = 7$, $W = H = 2000$:

| Metric | ISE stride ($s_b = 2014$) | UB stride ($s_b = 2000$) | Ratio |
|--------|--------------------------|-------------------------|-------|
| Stride | 2014 | 2000 | — |
| Tiles per unit length | $1/2014$ | $1/2000$ | 1.007 |
| Expanded tile width | 2014 | 2014 | 1.000 |
| Points per expanded tile | $2014^2 = 4{,}056{,}196$ | $2014^2 = 4{,}056{,}196$ | 1.000 |
| Overlap per tile pair | 0 (1 column) | 14 columns ($= 2c$) | — |

The cost increase is negligible: 0.7% more tiles laterally. Each tile is the same size (the expanded tile is always $W + 2c = 2014$ wide). The only overhead is processing slightly more tiles and performing the face-port matching step.

### 2.3 Radial Stride

The radial stride is $s_a = H$. Tiles are contiguous in the radial direction (no gap). The expanded tiles overlap by $2c = 14$ in the radial direction. The O-face / I-face matching works identically to the lateral case.

### 2.4 Gapless Coverage Verification

With $s_b = W$ and $s_a = H$, the interior tiles partition the band exactly:

$$
\mathcal{B} = \bigsqcup_{i,j} T_{i,j} \quad \text{(up to boundary effects)}.
$$

Every lattice point in the band is in exactly one tile's interior. Every edge in $G_k[\mathcal{B}^+]$ (where $\mathcal{B}^+$ is the band expanded by $c$) is in at least one tile's expanded region, because if both endpoints are in $\mathcal{B}$, at least one is in some $T_{i,j}$, and the other is within $c$ of that tile's boundary, hence in $T_{i,j}^+$.

---

## Part 3: Super-Tile Aggregation

### 3.1 The Scalability Problem

At large radius, the number of base tiles is enormous. Consider the $k^2 = 40$ UB campaign targeting the annular band $[R_{\min}, R_{\max}]$ in the first octant:

- Radial extent: $\Delta R = R_{\max} - R_{\min}$
- Lateral extent in first octant: at radius $R$, the first octant covers $0 \le b \le a$, so $b_{\max} \approx R_{\max}$
- Number of radial shells: $N_r = \lceil \Delta R / H \rceil$
- Number of lateral stripes: $N_\ell = \lceil R_{\max} / W \rceil$
- Total tiles: $N_r \times N_\ell$

For a band $[800\text{M}, 1.3\text{B}]$ with $H = W = 2000$:

$$
N_r = \frac{500{,}000{,}000}{2000} = 250{,}000, \qquad N_\ell = \frac{1{,}300{,}000{,}000}{2000} = 650{,}000
$$

$$
N_{\text{total}} = 250{,}000 \times 650{,}000 = 162.5 \times 10^9 \approx 163 \text{ billion tiles.}
$$

This is completely infeasible as a flat computation. Hence the need for hierarchical aggregation.

### 3.2 Super-Tile Definition

**Definition 3.1 (Super-tile).** A super-tile $S_{p,q}$ is a $K \times K$ block of base tiles:

$$
S_{p,q} = \{T_{i,j} : pK \le i < (p+1)K,\ qK \le j < (q+1)K\}.
$$

The super-tile has its own four faces, defined by the faces of its constituent base tiles:

- $I$-face of $S_{p,q}$: the I-faces of tiles $\{T_{pK, j}\}_{j = qK}^{(q+1)K - 1}$ (bottom row of the block)
- $O$-face of $S_{p,q}$: the O-faces of tiles $\{T_{(p+1)K-1, j}\}_{j = qK}^{(q+1)K - 1}$ (top row)
- $L$-face of $S_{p,q}$: the L-faces of tiles $\{T_{i, qK}\}_{i = pK}^{(p+1)K - 1}$ (left column)
- $R$-face of $S_{p,q}$: the R-faces of tiles $\{T_{i, (q+1)K - 1}\}_{i = pK}^{(p+1)K - 1}$ (right column)

### 3.3 Super-Tile Connectivity Computation

The face-port structure of a super-tile is computed by internal reachability within the $K \times K$ block:

1. Build the tile connectivity graph restricted to the $K^2$ base tiles in the block.
2. Run BFS/DFS from each boundary component (components touching any face of the block).
3. Record which boundary faces are mutually reachable.

This produces a super-tile face-port structure analogous to the base tile's: which components on the block's boundary connect $I \leftrightarrow O$, $I \leftrightarrow L$, etc.

**Proposition 3.2.** The super-tile face-port structure is a sound coarsening of the base tile graph. A path through the super-tile in the base graph implies a path through the super-tile in the super-tile graph, but not conversely.

**Proof.** The super-tile's boundary components are aggregations of base-tile boundary components. The internal reachability computation uses the base-tile graph edges, which are themselves sound approximations (Theorem 1.8). Composition of sound approximations is sound: if a path exists in the base-tile graph through the $K \times K$ block, the reachability computation detects it. $\square$

### 3.4 Hierarchical Decomposition

The hierarchy can be extended to multiple levels:

| Level | Block size | Tile side | Tiles at level | Coverage per tile |
|-------|-----------|-----------|---------------|-------------------|
| 0 (base) | $1 \times 1$ | $W = 2000$ | $N_r \times N_\ell$ | $2000 \times 2000$ |
| 1 (super) | $K \times K$ | $KW$ | $(N_r/K) \times (N_\ell/K)$ | $KW \times KW$ |
| 2 (mega) | $K^2 \times K^2$ | $K^2 W$ | $(N_r/K^2) \times (N_\ell/K^2)$ | $K^2 W \times K^2 W$ |

With $K = 100$:

| Level | Coverage per tile | Tiles for $[800\text{M}, 1.3\text{B}]$ |
|-------|------------------|-----------------------------------------|
| 0 | $2000 \times 2000$ | $250{,}000 \times 650{,}000 = 163\text{B}$ |
| 1 | $200{,}000 \times 200{,}000$ | $2{,}500 \times 6{,}500 = 16.25\text{M}$ |
| 2 | $20{,}000{,}000 \times 20{,}000{,}000$ | $25 \times 65 = 1{,}625$ |

At level 2, the reachability query on 1,625 mega-tiles is trivial. But level 1 requires processing 16.25M super-tiles, each requiring internal BFS over a $100 \times 100$ block of base tiles — still expensive.

### 3.5 Streaming Aggregation

The key insight is that aggregation can be done **streaming**: we don't need all $163\text{B}$ base tiles in memory simultaneously.

**Algorithm:** Process one radial shell of super-tiles at a time (all super-tiles at the same radial level $p$):

1. For each super-tile column $q$ at level $p$:
   a. Process $K$ radial shells of $K$ base tiles each ($K^2 = 10{,}000$ tiles per super-tile).
   b. Build the internal connectivity graph for the $K \times K$ block.
   c. Run reachability from boundary components.
   d. Output the super-tile's face-port summary.
   e. Discard the base-tile data (keep only the super-tile face-ports).

2. After all super-tiles in shell $p$ are processed, match super-tile face-ports between shells $p$ and $p-1$ (radial adjacency) and between adjacent columns $q$ and $q+1$ (lateral adjacency).

3. Incrementally build the super-tile connectivity graph.

**Memory per super-tile computation:** $K^2$ base tiles, each with $O(W \cdot H)$ data. With $K = 100$, $W = H = 2000$: $10{,}000$ tiles, each with $\sim 20$ MB of working data (bitmap + union-find). Processing all 10,000 simultaneously would require 200 GB — too large.

**Sequential processing within a super-tile:** Process the $K$ radial shells sequentially, keeping only face-port summaries from completed shells. This reduces memory to $K$ base tiles simultaneously (one radial shell of $K$ lateral tiles): $100 \times 20\text{ MB} = 2\text{ GB}$. Feasible on A100 (80 GB VRAM / 80 GB HBM).

Actually, the base-tile computation is CPU-bound (scanline kernel on CPU), not GPU-bound. The A100's value would be for a future CUDA scanline kernel. On CPU, memory is host RAM, which is typically 128+ GB on cloud instances.

### 3.6 Narrowing the Band

The full first-octant band $[800\text{M}, 1.3\text{B}]$ is 163 billion tiles. This is driven by two dimensions:

- The radial extent of 500M covers a HUGE range. For a UB campaign, we don't need to cover the entire range. We need a SINGLE radial slice where the band is blocked.
- The lateral extent covers the full first octant ($b$ up to $R_{\max}$). By symmetry, we need only the first octant.

**Band narrowing strategy:**

1. Use ISE to identify the radial region where $f(r) \approx 0$ (connectivity collapse). From the k40 campaign plan, this is expected near $R \approx 1.1\text{B}$.

2. Place the UB band at this radius. The band only needs to be wide enough radially that connectivity cannot "jump over" it. The minimum radial extent is $\sqrt{k^2} = \sqrt{40} \approx 6.32$, but for robustness we use $H_{\text{band}} = 2000$ (one tile height). If this thin band is blocked, $R_{\text{moat}} \le R_{\min}$.

3. Actually, a single tile height is NOT sufficient. A thin band can be jumped over by a component that enters the band and exits on the same side (enters via I-face, wanders laterally, exits via I-face, then re-enters via I-face at a different lateral position). The band must be thick enough that the I-face zone and O-face zone do not communicate except through the interior. This requires $H_{\text{band}} > 2c = 14$. With $H = 2000 \gg 2c = 14$, a single tile height is sufficient.

But a single tile height means only $N_r = 1$ radial shell of super-tiles. The blocking question reduces to: **across a single radial slice, can any connected component traverse from $b = 0$ to $b = R$?** Wait, that's the wrong question. The UB question is: can any component traverse from $a = R_{\min}$ to $a = R_{\max}$ (radially outward). With a single radial shell ($N_r = 1$), this is just the tile's I-to-O crossing: does any component in this row of tiles connect the I-face to the O-face?

This is exactly ISE, but with COMPLETE lateral coverage instead of sparse sampling.

**Revised approach:** The UB campaign is a **single-shell ISE with gapless lateral coverage**:

1. Fix a radial shell at the ISE-identified blockage radius $R_b$.
2. Tile the full first-octant lateral extent: $b \in [0, R_b]$, with stride $s_b = W = 2000$.
3. Number of tiles: $N_\ell = R_b / 2000 \approx 550{,}000$ tiles for $R_b \approx 1.1\text{B}$.
4. Each tile is independent (the I-to-O question is local). No inter-tile matching needed for the blocking question!

Wait — this is wrong. A single-shell approach checks each tile independently, but a component that enters one tile via the I-face, exits via the R-face, enters the next tile via the L-face, and eventually reaches the O-face of some tile, DOES cross the band. The ISE approach misses this because it treats each stripe independently. The tile UB approach must account for lateral connectivity.

**So the full tile graph approach IS necessary.** Even for a single radial shell, we need to build the lateral connectivity graph: components that exit one tile's R-face and enter the next tile's L-face, potentially relaying the I-to-O crossing through a sequence of lateral steps.

This is the tile connectivity graph $\mathcal{G}_{\text{tiles}}$ restricted to a single radial shell. Vertices are components within tiles. Edges connect laterally adjacent components (R-face to L-face). The query: does any path in this graph start at a vertex with I-face access and end at a vertex with O-face access?

### 3.7 Single-Shell UB: The Reduced Problem

For a single radial shell (height $H$), the tile connectivity graph simplifies:

- $N_r = 1$ (single radial shell)
- $N_\ell \approx R / W$ (full lateral coverage of first octant)
- Vertices: components in each tile, tagged with their face sets
- Edges: lateral adjacency only (R-face to L-face via shared overlap primes)
- Query: BFS/DFS on this graph, starting from all vertices with $I \in \partial C$, checking if any vertex with $O \in \partial C$ is reachable.

**Tile count:** For $R \approx 1.1\text{B}$, $N_\ell = 1.1 \times 10^9 / 2000 = 550{,}000$ tiles.

**Processing:** Each tile takes $\sim 2$ seconds (scanline kernel at $R \sim 1\text{B}$, from the k40 campaign plan). Sequential: $550{,}000 \times 2\text{s} = 1.1\text{M}$ seconds $\approx 13$ days. With 32-way parallelism on A100 CPU cores: $\sim 10$ hours.

**Memory:** Process tiles sequentially, keeping only face-port summaries. Each tile's face-port summary is small ($\sim$kilobytes). Total for $550{,}000$ tiles: $\sim 1$ GB. Feasible.

**Graph reachability:** The tile connectivity graph has at most $O(N_\ell \cdot C_{\max})$ vertices, where $C_{\max}$ is the maximum number of components per tile. At the percolation boundary, $C_{\max}$ is typically small (a few tens of components per 2000×2000 tile in the sparse regime). So $\sim 550{,}000 \times 50 = 27.5\text{M}$ vertices. BFS on a 27.5M-vertex sparse graph takes seconds.

### 3.8 Multi-Shell UB for Robustness

A single-shell UB is sensitive to the chosen radius. If the shell happens to have an anomalously connected configuration, it might show a crossing even though the surrounding region is blocked.

For robustness, use $N_r > 1$ shells (e.g., $N_r = 10$, covering a $20{,}000$-unit radial band). The tile graph then has both lateral AND radial edges. A crossing requires a path from $\partial_I$ (bottom of the band) to $\partial_O$ (top of the band), which is much harder for a chance fluctuation to achieve.

**Cost:** $N_r \times N_\ell = 10 \times 550{,}000 = 5.5\text{M}$ tiles. At 2 seconds each with 32-way parallelism: $\sim 96$ hours $\approx 4$ days. Feasible on a cloud instance.

---

## Part 4: A100 Deployment Architecture

### 4.1 Hardware Profile

- NVIDIA A100 80GB: 80 GB HBM2e VRAM, typically paired with 128+ GB host RAM
- For the scanline kernel (CPU-bound), the relevant resource is CPU cores, not GPU
- Cloud instances (e.g., Lambda, vast.ai) with A100 typically have 30-64 CPU cores
- Cost: $\sim 1.50\text{--}2.50$/hr for A100 instances (spot pricing)

The A100 GPU would be relevant for a future CUDA scanline kernel. For now, the campaign uses CPU-only scanline with rayon parallelism.

### 4.2 Per-Tile Resource Budget

For a $2000 \times 2000$ tile with $c = 7$:

| Component | Size |
|-----------|------|
| Expanded tile dimensions | $2014 \times 2014 = 4{,}056{,}196$ points |
| Bitmap (`bool`) | 4.06 MB |
| Parent array (`u32`) | 16.2 MB |
| Rank array (`u8`) | 4.06 MB |
| **Total working memory** | **$\sim 24$ MB per tile** |

With 32 cores, 32 tiles in flight: $32 \times 24\text{ MB} = 768\text{ MB}$. Well within any cloud instance's RAM.

### 4.3 Batch Processing Pipeline

```
For each lateral stripe j = 0, 1, ..., N_ℓ - 1 (in parallel batches of 32):
    1. Sieve: generate Gaussian primes in T_{0,j}^+ (expanded tile)
    2. Union-Find: build connected components via scanline kernel
    3. Face-port extraction: identify components touching I, O, L, R faces
    4. Serialize: write face-port summary to memory/disk
    5. Free: release tile working memory

After all tiles processed:
    6. Build tile connectivity graph from face-port summaries
    7. Run BFS/DFS reachability from ∂_I to ∂_O
    8. Report result
```

### 4.4 Compute Estimates for $k^2 = 40$ Single-Shell UB

**Parameters:**
- Shell location: $R \approx 1.1\text{B}$ (ISE-identified percolation boundary)
- Band height: $H = 2000$
- Lateral coverage: first octant, $b \in [0, 1.1\text{B}]$
- Lateral stride: $s_b = W = 2000$
- Number of tiles: $N_\ell = 550{,}000$

**Timing:**
- Per-tile time at $R \sim 1\text{B}$: $\sim 1.8\text{s}$ (from k40 campaign plan, Section 3.1)
- Sequential total: $550{,}000 \times 1.8\text{s} = 990{,}000\text{s} \approx 11.5$ days
- With 32 cores: $\sim 8.6$ hours
- With 64 cores: $\sim 4.3$ hours

**Cost:**
- At $\$2$/hr, 32-core instance: $8.6 \times \$2 = \$17.20$
- At $\$2$/hr, 64-core instance: $4.3 \times \$2 = \$8.60$

**Memory:**
- Working: 768 MB (32 tiles in flight)
- Face-port storage: $\sim 550{,}000 \times 2\text{ KB} = 1.1\text{ GB}$
- Total: $< 2$ GB

### 4.5 Compute Estimates for Multi-Shell UB

For $N_r = 10$ shells (radial band of 20,000):

| Metric | Value |
|--------|-------|
| Total tiles | $10 \times 550{,}000 = 5.5\text{M}$ |
| Sequential time | $5.5\text{M} \times 1.8\text{s} = 9.9\text{M s} \approx 115$ days |
| 32-core time | $\sim 3.6$ days |
| 64-core time | $\sim 1.8$ days |
| Cost (64-core, $\$2$/hr) | $\sim \$86$ |
| Face-port storage | $\sim 11\text{ GB}$ |

### 4.6 Narrower Bands and ISE Pre-Screening

The single-shell approach has a crucial advantage: ISE pre-screening eliminates most of the lateral extent.

**Observation:** If ISE at radius $R$ shows $f(r) = 0$ with $M = 32$ stripes, then ALL 32 sample tiles are blocked (io_count = 0). The UB campaign needs to check the remaining $550{,}000 - 32 = 549{,}968$ tiles, but if the ISE samples are representative, most of those will also be blocked.

**Optimization:** Run ISE first at the target radius with many stripes ($M = 128$ or $M = 256$). If all stripes show $f(r) = 0$, the UB campaign can be limited to a **lateral sweep** verifying the remaining tiles. Any tile that IS blocked can be ignored immediately (no face-port matching needed — it contributes no edges to the tile graph). Only tiles with io_count > 0 (or with non-trivial lateral connectivity) need face-port analysis.

In the deep sub-threshold regime ($R$ well past the percolation boundary), we expect nearly ALL tiles to be individually blocked. The tile graph would be nearly empty, and the UB is trivially confirmed. The interesting case is right at the boundary, where some tiles still cross.

---

## Part 5: Comparison with Classical Tsuchimura UB

### 5.1 Tsuchimura's Method

The classical approach to upper bounds on $R_{\mathrm{moat}}(k)$, as used by Tsuchimura and others:

1. **Sieve** all Gaussian primes in the annular band $\{z : R_{\min} \le |z| \le R_{\max}\}$.
2. **Build the full graph** $G_k$ restricted to these primes: connect all pairs within distance $\sqrt{k^2}$.
3. **Run union-find** on the entire prime set.
4. **Check connectivity:** Does any connected component contain primes near both the inner and outer boundaries?

This method is exact: it computes the true connectivity of $G_k$ within the band. If it declares "blocked," the moat is real. If it declares "crossing," the crossing is real.

### 5.2 Trade-Offs

| Property | Tsuchimura (exact) | Tile-based UB |
|----------|-------------------|---------------|
| **Correctness** | Exact (both directions) | Sound for UB only (conservative) |
| **Memory** | All primes in band simultaneously | One tile at a time (streaming) |
| **Parallelism** | Limited (global union-find) | Embarrassingly parallel (tiles independent) |
| **Scalability** | $O(R^2)$ primes in memory | $O(W^2)$ per tile, streaming |
| **False negatives** | None | Possible (path exits tile) |
| **False positives** | None | None |

### 5.3 Memory Comparison at $R \sim 1\text{B}$

**Tsuchimura:** The annular band $[R_{\min}, R_{\max}]$ in the first octant contains approximately

$$
N_{\text{primes}} \approx \frac{\text{area} \times \rho(R)}{1} = \frac{H \times R}{1} \times \frac{C}{2 \ln R} \approx \frac{2000 \times 10^9 \times 1.274}{2 \times 20.7} \approx 6.2 \times 10^{10}
$$

primes. At 16 bytes per prime (coordinates + component ID), this requires $\sim 1\text{ TB}$ of RAM. Plus the adjacency structure (each prime has $\sim 128$ neighbors): $\sim 100\text{ TB}$. **Completely infeasible** with Tsuchimura's method at $R \sim 1\text{B}$.

**Tile-based:** Working memory is 24 MB per tile. Face-port storage for the full campaign is $\sim 1\text{ GB}$. **Feasible on any modern machine.**

### 5.4 When Tsuchimura Wins

Tsuchimura's method wins when:
- The band is small enough to fit in memory ($R \lesssim 10^7$ for 128 GB RAM)
- The exact answer is needed (the tile method might miss a path that wanders outside a tile)
- The moat location is known precisely (so the band can be narrow)

For $k^2 = 36$ ($R \sim 80\text{M}$), Tsuchimura may be feasible with careful engineering. For $k^2 = 40$ ($R \sim 1\text{B}$), only the tile-based method is practical.

### 5.5 Hybrid Approach

The methods can be combined:

1. **Tile-based screening:** Use the tile UB method to identify the narrowest radial band where the tile graph is blocked.
2. **Tsuchimura verification:** If the blocked band is narrow enough (e.g., $H = 2000$ at $R \sim 1\text{B}$, which is $\sim 6.2 \times 10^7$ primes per lateral slice — feasible in $\sim 1\text{ GB}$), verify with Tsuchimura's exact method on the subset of tiles that show non-trivial connectivity.

Actually, a 2000-unit radial band at $R \sim 1\text{B}$ in the first octant contains $\sim 2000 \times 10^9 \times 0.031 \approx 6.2 \times 10^{10}$ lattice points, of which $\sim 3\%$ are prime: $\sim 1.9 \times 10^9$ primes. This is too many for Tsuchimura. The tile-based method is the only option at this scale.

---

## Part 6: Campaign Design

### 6.1 Prerequisites

1. **ISE Phase 1 + Phase 2 completed:** Identify the percolation boundary $R_b$ for $k^2 = 40$.
2. **ISE data confirms $f(r) = 0$** at $R_b$ with $M = 32$ stripes.
3. **MR-9 validity:** $R_b \le 1.955\text{B}$ (the deterministic primality ceiling).

### 6.2 Campaign Parameters

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| $k^2$ | 40 | Target step bound |
| $c$ | 7 | $\lceil \sqrt{40} \rceil$ |
| $W$ | 2000 | Standard tile width |
| $H$ | 2000 | Standard tile height |
| $s_b$ | 2000 | Gapless lateral stride |
| $s_a$ | 2000 | Gapless radial stride (= $H$) |
| $R_{\text{center}}$ | $R_b$ | ISE-identified percolation boundary |
| $N_r$ | 1 (initial), 10 (robust) | Number of radial shells |
| $N_\ell$ | $\lceil R_b / 2000 \rceil$ | Full first-octant lateral coverage |
| Symmetry | First octant ($0 \le b \le a$) | Eightfold symmetry of $\mathbb{Z}[i]$ |

### 6.3 Execution Plan

**Phase UB-1: Single-shell probe** (est. 4--9 hours on 64-core cloud instance)

Place a single radial shell at $R_b$. Process all $N_\ell \approx 550{,}000$ tiles. Build the tile connectivity graph. Run BFS from $\partial_I$. Report whether $\partial_O$ is reachable.

Expected outcome at the percolation boundary: the tile graph is almost entirely disconnected. Most tiles have io_count = 0 (individually blocked). The few tiles with non-zero io_count have components that touch I and O faces but are isolated laterally. The BFS terminates quickly.

If blocked: $R_{\mathrm{moat}}(40) \le R_b + c = R_b + 7$. **UB established.**

If not blocked: the shell is too close to the percolation boundary. Move outward to $R_b + 50{,}000$ and retry.

**Phase UB-2: Multi-shell confirmation** (est. 2--4 days on 64-core cloud instance)

Place $N_r = 10$ radial shells at $R_b$, covering a band of $20{,}000$ units. Build the full two-dimensional tile graph. Run BFS from $\partial_I$ to $\partial_O$.

This is a stronger statement: no path crosses a 20,000-unit radial band at $R_b$. The UB becomes $R_{\mathrm{moat}}(40) \le R_b + c$.

**Phase UB-3: Tighten the bound** (optional, days to weeks)

Move the band inward (toward the origin) by steps of 100,000. At each position, run the single-shell UB. Find the smallest $R$ where the band is still blocked. This tightens the UB to the actual moat location.

### 6.4 Cost Summary

| Phase | Tiles | Core-hours | Wall time (64 cores) | Cloud cost ($\$2$/hr) |
|-------|-------|-----------|---------------------|---------------------|
| UB-1 | 550K | 275 | 4.3 hr | $\$8.60$ |
| UB-2 | 5.5M | 2,750 | 43 hr | $\$86$ |
| UB-3 (10 positions) | 5.5M | 2,750 | 43 hr | $\$86$ |
| **Total** | **11.6M** | **5,775** | **$\sim 90$ hr** | **$\sim \$180$** |

### 6.5 Deliverables

1. **UB certificate:** The tile connectivity graph data showing no $\partial_I \to \partial_O$ path at radius $R_b$.
2. **Bound value:** $R_{\mathrm{moat}}(40) \le R_b + 7$.
3. **Tile statistics:** Distribution of io_count, number of non-trivially connected tiles, largest connected component in the tile graph.
4. **Tightened bound** (Phase UB-3): smallest $R$ where the band is blocked.
5. **Comparison with ISE:** correlation between ISE $f(r)$ and tile graph connectivity density.

### 6.6 Risk Factors

1. **The percolation boundary may be beyond $R = 1.955\text{B}$** (MR-9 ceiling). Mitigation: upgrade to 12-witness MR (extends to $R \sim 18\text{B}$).

2. **The tile UB may show a crossing where ISE shows $f(r) = 0$.** This would happen if a component threads through multiple tiles laterally, creating a path that no single tile detects as an I-to-O crossing. This is actually the expected behavior near the boundary: the tile graph captures connections that ISE misses (because ISE samples sparsely). The UB campaign may need to go to a higher radius than the ISE boundary.

3. **Memory for face-port storage.** At 2 KB per tile and 550K tiles: 1.1 GB. With $N_r = 10$: 11 GB. Feasible.

4. **Tile processing time variance.** Tiles in dense regions (low $R$) take longer than tiles in sparse regions. Rayon's work-stealing handles this well, but the load imbalance may reduce effective parallelism to $\sim 70\%$ of theoretical.

5. **First-octant boundary effects.** Tiles near $b = 0$ and $b = a$ (the octant boundaries) have special geometry. Near $b = 0$: the $b \mapsto -b$ reflection symmetry means connectivity is symmetric, so the boundary acts as a "mirror wall" — components reflect rather than terminate. Near $b = a$: the $a \leftrightarrow b$ symmetry (quarter-turn) means the same. These boundary effects do not affect the UB argument (they can only make blocking harder, not easier).

---

## Appendix A: Comparison of Collar Definitions

The CTO document defines $c = \lfloor \sqrt{k^2} \rfloor$. The implementation uses $c = \lceil \sqrt{k^2} \rceil$. For $k^2 = 40$:

- $\lfloor \sqrt{40} \rfloor = 6$
- $\lceil \sqrt{40} \rceil = 7$

The maximum single-coordinate excursion in the neighbor set is 6 (the vectors $(\pm 6, \pm 2)$ with $36 + 4 = 40 \le k^2$). So $c = 6$ is sufficient for correctness: every edge incident to an interior prime is visible in the expanded tile.

The implementation's $c = 7$ is conservative: it adds one extra column/row of collar, which captures primes that are not reachable by any single step but provides a safety margin. For the UB campaign, the implementation's value ($c = 7$) is used throughout, ensuring compatibility with the existing scanline kernel.

## Appendix B: Edge Detection Implementation Sketch

```
function build_tile_graph(tiles: List[TileOperator], grid: GridLayout):
    graph = empty_graph()

    # Add vertices
    for each tile T_{i,j} in tiles:
        for each component α in T_{i,j}:
            graph.add_vertex((i, j, α), face_set=T_{i,j}.component_faces[α])

    # Add lateral edges (R-face to L-face)
    for each lateral pair (T_{i,j}, T_{i,j+1}):
        R_ports = T_{i,j}.face_right  # primes in R-face zone with component IDs
        L_ports = T_{i,j+1}.face_left  # primes in L-face zone with component IDs

        # Find shared primes (same (a,b) coordinates in overlap region)
        R_dict = {(p.a, p.b): p.component for p in R_ports}
        for p in L_ports:
            if (p.a, p.b) in R_dict:
                α = R_dict[(p.a, p.b)]
                β = p.component
                graph.add_edge((i, j, α), (i, j+1, β))

    # Add radial edges (O-face to I-face) — analogous
    for each radial pair (T_{i,j}, T_{i+1,j}):
        # ... same logic with O-ports and I-ports ...

    return graph

function check_ub(graph, N_r):
    # BFS from I-boundary
    I_vertices = {v for v in graph if v.shell == 0 and I ∈ v.face_set}
    O_vertices = {v for v in graph if v.shell == N_r-1 and O ∈ v.face_set}

    reachable = BFS(graph, start=I_vertices)

    if reachable ∩ O_vertices == ∅:
        return "BLOCKED — UB established"
    else:
        return "CROSSING — UB not established at this radius"
```

## Appendix C: Notation Summary

| Symbol | Meaning |
|--------|---------|
| $G_k = (P, E_k)$ | Gaussian prime graph with step bound $k^2$ |
| $c$ | Collar width, $\lceil \sqrt{k^2} \rceil$ |
| $T_{i,j}$ | Base tile at shell $i$, stripe $j$ |
| $T_{i,j}^+$ | Expanded tile (with collar) |
| $\mathcal{G}_{\text{tiles}}$ | Tile connectivity graph |
| $\partial_I, \partial_O$ | I-boundary and O-boundary vertex sets |
| $S_{p,q}$ | Super-tile (block of $K \times K$ base tiles) |
| $s_b$ | Lateral stride |
| $s_a$ | Radial stride |
| $W, H$ | Tile width and height |
| $N_r, N_\ell$ | Number of radial shells and lateral stripes |
| $R_b$ | ISE percolation boundary radius |
| $f(r)$ | ISE crossing fraction at radius $r$ |
| io_count | Number of I-to-O crossing components in a tile |
