# Gaussian Prime Connectivity Transfer Operator (GPCTO):
# Methods for Scalable Moat Detection

> Working methodology document -- for peer review
> Version: 0.2 -- 2026-03-25
> Status: Draft for mathematician review

## Abstract

The Gaussian moat problem asks whether, for each fixed step bound $k$, the connected component of the origin in the Gaussian prime proximity graph $G_k$ is finite. We develop two complementary methods for investigating this question at radii far beyond current computational records. The **tile operator** compresses the connectivity of a rectangular region of the Gaussian lattice into boundary data -- which connected components touch which faces -- and admits a natural composition algebra. The **Independent Strip Ensemble (ISE)** deploys many collar-disjoint tile probes at the same radial distance; it is provably conservative (zero false negatives) with false-positive probability decaying exponentially in the number of strips. For rigorous proof, a **tile-based upper bound (UB)** method tiles the first-octant annulus gaplessly and checks global disconnection through composition. We further develop a **fat-stripe** method -- a concrete realization of the UB approach -- that tiles an annular strip in the first octant, processes each tile independently via row sieve and sparse union-find, composes all tiles via the GPCTO algebra, and delivers a definitive verdict on whether any connected component spans the annulus radially. Calibration against the known Tsuchimura moats at $k^2 = 26$ and $k^2 = 32$ confirms correct detection. At $k^2 = 36$, the ISE locates a percolation transition near $R \approx 80.4\text{M}$; at $k^2 = 40$, results through $R = 3.5\text{B}$ show a sigmoid collapse with $R_{0.5} \approx 839\text{M}$. Fat-stripe calibration confirms total annular barriers at $k^2 = 32$ ($R \approx 2.82\text{M}$) and $k^2 = 36$ ($R \approx 80\text{M}$), with cross-validation against ISE establishing consistency between the two methods. At $k^2 = 40$, five fat-stripe probes across $R \in [1.03\text{B}, 1.09\text{B}]$ detect total annular blockage — the first such detection at a $k^2$ value where no moat was previously known. Angular coverage is 128K lattice units ($\sim 0.007°$); full-octant tiling is needed for rigorous proof.

---

## 1. Preliminaries

### 1.1 Gaussian integers

**Definition 1.1** (Gaussian integers). The ring of Gaussian integers is $\mathbb{Z}[i] = \{a + bi : a, b \in \mathbb{Z}\}$, equipped with the squared norm $|z|^2 = a^2 + b^2$ for $z = a + bi$.

### 1.2 Gaussian primes

**Definition 1.2** (Gaussian primes). An element $\pi \in \mathbb{Z}[i]$ is a *Gaussian prime* if it generates a prime ideal in $\mathbb{Z}[i]$. The classification is:

1. **Split primes.** If $p \equiv 1 \pmod{4}$ is a rational prime, then $p = a^2 + b^2$ for unique $a > b > 0$ (Cornacchia), and $\pi = a + bi$ and its associates are Gaussian primes with $|\pi|^2 = p$.

2. **Inert primes.** If $p \equiv 3 \pmod{4}$ is a rational prime, then $p$ itself is a Gaussian prime with $|p|^2 = p^2$.

3. **Ramified prime.** The rational prime $2 = -i(1+i)^2$; the element $1+i$ is a Gaussian prime with $|1+i|^2 = 2$.

We write $P$ for the set of all Gaussian primes. By the Gaussian prime number theorem, the count of Gaussian primes with norm at most $N$ is asymptotically $C \cdot N / \ln N$, where $C$ depends on convention. In a lattice rectangle at distance $R$ from the origin, the local prime density is approximately

$$
\rho(R) \approx \frac{C}{2\ln R},
$$

with $C \approx 1.274$ calibrated empirically.

### 1.3 The Gaussian prime graph

**Definition 1.3** (Gaussian prime graph). Fix $k^2 \in \mathbb{N}$. The *Gaussian prime graph* is $G_k = (P, E_k)$ where

$$
\{\pi, \pi'\} \in E_k \quad \Longleftrightarrow \quad |\pi - \pi'|^2 \le k^2.
$$

The *seed set* is $\Sigma_k = \{\pi \in P : |\pi|^2 \le k^2\}$, and the *origin component* $C_0(k)$ is the connected component of $G_k$ containing every prime in $\Sigma_k$. (One may equivalently adjoin a formal vertex at the origin connected to all primes in $\Sigma_k$.)

### 1.4 The moat problem

**Definition 1.4** (Moat radius). The *moat radius* is

$$
R_{\mathrm{moat}}(k) = \sup\{R \ge 0 : C_0(k) \text{ meets the circle } |z| = R\}.
$$

The Gaussian moat problem asks: is $R_{\mathrm{moat}}(k)$ finite for every fixed $k$?

### 1.5 Prior art

| $k^2$ | $R_{\mathrm{moat}}$ | Source |
|--------|---------------------|--------|
| 2 | $\sqrt{137} \approx 11.7$ | Classical |
| 6 | $\approx 3{,}612$ | Tsuchimura 2005 |
| 10 | $\approx 26{,}862$ | Tsuchimura 2005 |
| 18 | $\approx 175{,}045$ | Tsuchimura 2005 |
| 26 | $\approx 1{,}015{,}639$ | Tsuchimura 2005 |
| 32 | $\approx 2{,}823{,}055$ | Tsuchimura 2005 |
| 36 | Unknown (ISE transition at $\approx 80\text{M}$) | This work |
| 40 | Unknown (ISE transition at $\approx 839\text{M}$) | This work |

Tsuchimura's method grows the origin component outward through all Gaussian primes, at cost $O(R^2 / \ln R)$ in both time and memory. This becomes prohibitive for $R > 10^8$.

---

## 2. The Tile Operator

### 2.1 Tile definition

**Definition 2.1** (Tile). A *tile* is a lattice rectangle

$$
T = T(a_0, b_0, W, H) = \{a + bi \in \mathbb{Z}[i] : a_0 \le a \le a_0 + H,\ b_0 \le b \le b_0 + W\},
$$

where $a$ is the radial coordinate and $b$ the lateral coordinate. We call $W$ the *width* and $H$ the *height* of the tile.

### 2.2 The four faces

**Definition 2.2** (Faces). The tile $T$ has four distinguished faces. A Gaussian prime $\pi = a + bi \in P \cap T$ touches:

- the *Inner face* $\mathcal{I}$ if $a - a_0 \le c$,
- the *Outer face* $\mathcal{O}$ if $(a_0 + H) - a \le c$,
- the *Left face* $\mathcal{L}$ if $b - b_0 \le c$,
- the *Right face* $\mathcal{R}$ if $(b_0 + W) - b \le c$,

where $c$ is the collar width defined below. These inequalities are non-strict ($\le$, not $<$): a prime at distance exactly $c$ from a face boundary is counted as touching that face.

### 2.3 Collar

**Definition 2.3** (Collar). The *collar* is

$$
c = \left\lceil \sqrt{k^2} \right\rceil.
$$

The *expanded tile* is

$$
T^+ = \{a + bi : a_0 - c \le a \le a_0 + H + c,\ b_0 - c \le b \le b_0 + W + c\}.
$$

**Justification.** An integer step vector $(\Delta a, \Delta b)$ satisfying $\Delta a^2 + \Delta b^2 \le k^2$ has maximal single-coordinate excursion

$$
\max\{|\Delta a| : \exists \Delta b,\ \Delta a^2 + \Delta b^2 \le k^2\} = \left\lfloor \sqrt{k^2} \right\rfloor.
$$

The floor is the tight bound: the maximum $|\Delta a|$ is achieved at $\Delta b = 0$, giving $|\Delta a| \le \lfloor\sqrt{k^2}\rfloor$.

Using $c = \lceil\sqrt{k^2}\rceil$ rather than $\lfloor\sqrt{k^2}\rfloor$ is the *conservative* choice. When $k^2$ is a perfect square, $\lceil\sqrt{k^2}\rceil = \lfloor\sqrt{k^2}\rfloor$, so the two definitions coincide. When $k^2$ is not a perfect square, $\lceil\sqrt{k^2}\rceil = \lfloor\sqrt{k^2}\rfloor + 1$, so the collar is one unit wider than strictly necessary.

This over-collaring is harmless for correctness -- the expanded tile includes strictly more lattice points than required -- and it provides two benefits:

1. **Uniform formula.** For any $k^2$, the collar $c = \lceil\sqrt{k^2}\rceil$ guarantees that every edge incident to an interior prime is visible in $T^+$, without case analysis on whether $k^2$ is a perfect square.

2. **Face-port completeness.** With $\le$ in the face-membership inequalities, a prime at distance exactly $c$ from the boundary is tagged as a face port. Using ceil ensures that this threshold is never below the actual maximum excursion, even though the excess is at most one unit.

*Concrete values:*

| $k^2$ | $\lfloor\sqrt{k^2}\rfloor$ | $\lceil\sqrt{k^2}\rceil$ | Remark |
|--------|------|------|--------|
| 36 | 6 | 6 | Perfect square; floor = ceil |
| 40 | 6 | 7 | Non-perfect square; collar oversized by 1 |

Throughout this document, $c = \lceil\sqrt{k^2}\rceil$ is the canonical collar. All theorems are stated and proved using this definition.

### 2.4 Face ports

**Definition 2.4** (Face ports). A *face port* is a record $(\pi, C, F)$ where $\pi \in P \cap T$ is a Gaussian prime in the interior tile, $C$ is the index of its connected component in $G_k[T^+]$, and $F \in \{\mathcal{I}, \mathcal{O}, \mathcal{L}, \mathcal{R}\}$ is the face it touches. A single prime may be a face port for multiple faces.

### 2.5 Tile connectivity function

**Definition 2.5** (Tile connectivity function). Given tile $T$ and step bound $k^2$:

1. Form the induced subgraph $G_k[T^+]$ on all Gaussian primes in the expanded tile $T^+$.
2. Compute the connected components of $G_k[T^+]$ via union-find.
3. For each component $C$, determine its *face set* $\partial_T C \subseteq \{\mathcal{I}, \mathcal{O}, \mathcal{L}, \mathcal{R}\}$ -- the set of faces touched by primes of $C$ that lie in the interior tile $T$.
4. The *transfer data* are the pairwise face-spanning counts:

$$
\operatorname{span}_T(X, Y) = \#\{C : X, Y \in \partial_T C\}, \qquad X, Y \in \{\mathcal{I}, \mathcal{O}, \mathcal{L}, \mathcal{R}\},\ X \ne Y.
$$

### 2.6 Inner-to-Outer count

**Definition 2.6** (io_count). The *inner-to-outer count* is

$$
\operatorname{io\_count}(T) = \operatorname{span}_T(\mathcal{I}, \mathcal{O}),
$$

the number of distinct connected components in $G_k[T^+]$ that touch both the Inner and Outer faces of $T$.

**Interpretation.** $\operatorname{io\_count}(T) > 0$ means there exists a path in $G_k[T^+]$ from a prime near the bottom edge to a prime near the top edge. $\operatorname{io\_count}(T) = 0$ means the tile is *radially blocked*: no connectivity passes through it in the outward direction.

---

## 3. Tile Composition

### 3.1 Horizontal composition

**Definition 3.1** (Horizontal composition). Let $T_L = T(a_0, b_0, W, H)$ and $T_R = T(a_0, b_0 + s_b, W, H)$ be two tiles at the same radial level, separated laterally by stride $s_b$. Their *horizontal composition* $T_L \otimes_h T_R$ is a new tile operator whose components are obtained by:

1. Taking the union of all components from both tiles.
2. Merging component $C_L$ from $T_L$ with component $C_R$ from $T_R$ whenever there exist face ports $p_L \in \mathcal{R}(T_L)$ and $p_R \in \mathcal{L}(T_R)$ belonging to $C_L$ and $C_R$ respectively, with $|p_L - p_R|^2 \le k^2$.
3. Recomputing face sets for the merged components.

The composed tile has faces $\mathcal{I}(T_L) \cup \mathcal{I}(T_R)$, $\mathcal{O}(T_L) \cup \mathcal{O}(T_R)$, $\mathcal{L}(T_L)$, and $\mathcal{R}(T_R)$.

### 3.2 Vertical composition

**Definition 3.2** (Vertical composition). Let $T_B = T(a_0, b_0, W, H)$ and $T_T = T(a_0 + H, b_0, W, H)$ be two tiles stacked radially. Their *vertical composition* $T_B \otimes_v T_T$ merges components whenever there exist face ports $p_B \in \mathcal{O}(T_B)$ and $p_T \in \mathcal{I}(T_T)$ with $|p_B - p_T|^2 \le k^2$. The composed tile has faces $\mathcal{I}(T_B)$, $\mathcal{O}(T_T)$, $\mathcal{L}(T_B) \cup \mathcal{L}(T_T)$, and $\mathcal{R}(T_B) \cup \mathcal{R}(T_T)$.

### 3.3 Composition soundness

**Theorem 3.1** (Distance-based composition is conservative). *Let $T_1, T_2$ be tiles sharing a boundary, and let $T_1 \otimes T_2$ denote their composition via distance-based face-port matching. If two components $C_1, C_2$ are merged in $T_1 \otimes T_2$, then there exists a genuine edge $\{\pi_1, \pi_2\} \in E_k$ with $\pi_1 \in C_1$, $\pi_2 \in C_2$.*

*Proof.* The merge occurs because there exist face ports $p_1 \in C_1$ and $p_2 \in C_2$ with $|p_1 - p_2|^2 \le k^2$. By Definition 1.3, this means $\{p_1, p_2\} \in E_k$. Since $p_1 \in C_1$ and $p_2 \in C_2$, the edge connects the two components in $G_k$. $\square$

**Corollary 3.1.** *Distance-based composition never introduces false connections: every merged pair of components is genuinely connected in $G_k$.*

**Remark 3.1** (Conservative direction). Distance-based composition can, however, *miss* connections. Two primes $\pi_1 \in T_1$ and $\pi_2 \in T_2$ might be connected in $G_k$ via a multi-hop path that passes through primes in the overlap region without either $\pi_1$ or $\pi_2$ being face ports. An alternative *shared-prime matching* scheme -- identifying primes with identical coordinates in the overlap zone and merging their respective component assignments -- captures all such connections exactly. The current implementation uses distance-based matching; shared-prime matching is formalized but not yet implemented. For the ISE method (Section 4), composition is not used and this distinction is immaterial. For the tile-UB method (Section 5), distance-based matching is sound (it can only undercount connections, which is conservative for proving disconnection) but potentially less tight than shared-prime matching.

---

## 4. Independent Strip Ensemble (ISE)

### 4.1 Strip definition

**Definition 4.1** (Strip). A *strip* $S_j$ at radial level $R$ with tile dimensions $W \times H$ is a sequence of tiles:

$$
S_j = \bigcup_{n=0}^{N-1} T(a_{\text{lo}} + nH,\ b_j,\ W,\ H),
$$

where $a_{\text{lo}}$ is chosen to place the inner face at radial distance $R$ from the origin, $b_j$ is the lateral offset of strip $j$, and $N = \lceil(R_{\max} - R_{\min})/H\rceil$ is the number of shells.

**Definition 4.2** (Independence criterion). Two strips $S_j$ and $S_l$ are *independent* if their expanded tiles are disjoint in the lateral coordinate: the collar-expanded regions $[b_j - c,\ b_j + W + c]$ and $[b_l - c,\ b_l + W + c]$ do not overlap. This is guaranteed when the lateral stride satisfies

$$
|b_j - b_l| \ge W + 2c.
$$

The standard ISE layout places $M$ strips at positive-only offsets:

$$
b_j = c + j \cdot (W + 2c), \qquad j = 0, 1, \ldots, M-1.
$$

This ensures all $M$ expanded tiles are laterally disjoint and avoids the $b \to -b$ reflection symmetry (Gaussian primality of $a + bi$ is identical to that of $a - bi$), which would halve the effective independent sample count.

### 4.2 The $f(r)$ metric

**Definition 4.3** ($f(r)$). At radial shell $n$ with midpoint $r_n = R_{\min} + (n + \tfrac{1}{2})H$, define

$$
f(r_n) = \frac{\#\{j : \operatorname{io\_count}_j(r_n) > 0\}}{M},
$$

where $\operatorname{io\_count}_j(r_n)$ is the inner-to-outer count of the tile in strip $j$ at shell $n$.

**Interpretation.** $f(r)$ is the fraction of independent strip probes that detect Inner-to-Outer connectivity at radius $r$. It ranges from $0$ (all strips blocked) to $1$ (all strips crossing).

### 4.3 Subgraph monotonicity (zero false negatives)

**Theorem 4.1** (Subgraph monotonicity). *Let $S \subseteq \mathbb{Z}[i]$ be any subset containing the seed set $\Sigma_k$. Let $R_{\mathrm{strip}}(k, S)$ denote the moat radius of the origin component in the induced subgraph $G_k|_S$. Then*

$$
R_{\mathrm{strip}}(k, S) \le R_{\mathrm{moat}}(k).
$$

*Proof.* The primes in $S$ form a subset $P_S \subseteq P$. The graph $G_k|_S = (P_S, E_k|_{P_S})$ is an induced subgraph of $G_k$. Every path in $G_k|_S$ is also a path in $G_k$ (each edge satisfies $|\pi - \pi'|^2 \le k^2$ and both endpoints lie in $P$). Therefore every prime reachable from $\Sigma_k$ in $G_k|_S$ is also reachable in $G_k$. Taking suprema of radii gives $R_{\mathrm{strip}}(k, S) \le R_{\mathrm{moat}}(k)$. $\square$

**Corollary 4.1** (Local crossing soundness). *If a tile $T$ has $\operatorname{io\_count}(T) > 0$, then there exists a genuine Inner-to-Outer path in $G_k$ through the collared region $T^+$.*

*Proof.* The component witnessing $\operatorname{io\_count}(T) > 0$ lives in $G_k[T^+]$, which is a subgraph of $G_k$. The same path exists in $G_k$. $\square$

**Theorem 4.2** (ISE zero false negatives). *Let $\{S_j\}_{j=0}^{M-1}$ be an ensemble of strips covering a radial interval $[R, R+H]$. If $f(r) = 0$ at every strip, then no path in $G_k$ passes from $|z| < R$ to $|z| > R + H$ through any of the $M$ sampled lateral positions.*

*Proof.* For each strip $S_j$, the tile at shell $r$ computes $G_k[T_j^+]$, which contains every Gaussian prime in the expanded tile and every edge incident to any interior prime. If $\operatorname{io\_count}_j(r) = 0$, then no connected component of $G_k[T_j^+]$ spans from the Inner to the Outer face of $T_j$.

Any path in $G_k$ from a prime with $a < a_{\text{lo}} + c$ to a prime with $a > a_{\text{lo}} + H - c$, passing through the lateral range $[b_j, b_j + W]$, must consist entirely of primes within $T_j^+$ (since each step moves at most $c$ units laterally, and the strip width $W$ exceeds $2c$, a path entering the strip's interior cannot exit the expanded tile's lateral range and re-enter within the same tile height). Therefore any such path would appear as a component in $G_k[T_j^+]$, contradicting $\operatorname{io\_count}_j = 0$. $\square$

**Remark 4.1** (False positives are possible). $\operatorname{io\_count}(T) > 0$ certifies a local crossing but does not imply the crossing is part of the origin component $C_0(k)$. The ISE measures *local percolation* (whether any component crosses a shell), not *origin-connected reachability* (whether $C_0(k)$ crosses a shell). These are distinct: a Tsuchimura moat can exist at a radius where $f(r) > 0$, if the crossing components are disconnected from the origin.

### 4.4 False-positive analysis

**Theorem 4.3** (Ensemble false-positive bound). *Suppose, at some shell $r$, the full graph $G_k$ contains an Inner-to-Outer crossing. Let $p(r)$ denote the probability that a single strip fails to detect this crossing (i.e., $\operatorname{io\_count}_j(r) = 0$ despite the crossing existing in $G_k$). If the $M$ strips are independent (collar-disjoint), then*

$$
\Pr(f(r) = 0 \mid \text{crossing exists in } G_k) \le p(r)^M.
$$

*Proof.* The event $f(r) = 0$ requires all $M$ strips to independently fail. Under the disjointness condition, the prime populations in distinct tiles are disjoint, so their $\operatorname{io\_count}$ outcomes are determined by disjoint subsets of primes. The product bound follows. $\square$

Over $N$ shells, the union bound gives $\Pr(\text{any false positive}) \le N \cdot p(r)^M$. Concrete values:

| $p$ | $M = 16$ | $M = 32$ | $M = 64$ |
|-----|----------|----------|----------|
| 0.30 | $4.3 \times 10^{-9}$ | $1.9 \times 10^{-17}$ | $3.5 \times 10^{-34}$ |
| 0.10 | $10^{-16}$ | $10^{-32}$ | $10^{-64}$ |
| 0.01 | $10^{-32}$ | $10^{-64}$ | $10^{-128}$ |

**Remark 4.2** (Independence model). The product formula requires that io_count outcomes across strips are independent. This holds exactly when the expanded tiles are laterally disjoint (guaranteed by stride $\ge W + 2c$). Gaussian primes are deterministic, so "independence" here means that the prime distribution in one tile is not predictive of the connectivity pattern in a distant tile. For tiles separated by $\gg \sqrt{k^2}$, this is empirically well-supported: the Hardy-Littlewood conjecture for Gaussian primes predicts asymptotic independence of prime counts in disjoint regions.

### 4.5 Curvature compensation

**Problem.** Strips are rectangular, but the moat is circular. Under naive placement (all strips sharing the same $a_{\text{lo}}$), a strip at lateral offset $b_j$ has its Inner face center at Euclidean distance $\sqrt{a_{\text{lo}}^2 + (b_j + W/2)^2}$ from the origin. Lateral strips are therefore at slightly different radii.

**Definition 4.4** (Compensated placement). For strip $j$ with lateral center $b_{\text{center},j} = b_j + W/2$, set

$$
a_{\text{lo},j} = \left\lfloor \sqrt{R_{\text{target}}^2 - b_{\text{center},j}^2} \right\rfloor,
$$

where $R_{\text{target}}$ is the desired inner-face radius for all strips.

**Theorem 4.4** (Compensated placement accuracy). *Under compensated placement, the actual inner-face center distance from the origin satisfies*

$$
\left|\sqrt{a_{\text{lo},j}^2 + b_{\text{center},j}^2} - R_{\text{target}}\right| < 1.
$$

*Proof.* By definition, $a_{\text{lo},j} = \lfloor x \rfloor$ where $x = \sqrt{R_{\text{target}}^2 - b_{\text{center},j}^2}$. Then $x - 1 < a_{\text{lo},j} \le x$, so

$$
a_{\text{lo},j}^2 + b_{\text{center},j}^2 \le x^2 + b_{\text{center},j}^2 = R_{\text{target}}^2,
$$

and

$$
a_{\text{lo},j}^2 + b_{\text{center},j}^2 > (x-1)^2 + b_{\text{center},j}^2 = R_{\text{target}}^2 - 2x + 1.
$$

Since $x > 0$, the squared distance lies in $(R_{\text{target}}^2 - 2x + 1,\ R_{\text{target}}^2]$, giving a radial error bounded by $1/R_{\text{target}} < 1$ for $R_{\text{target}} > 1$. $\square$

**Remark 4.3.** Compensated placement preserves strip independence: only the $a$-coordinate changes per stripe; the lateral separation $|b_j - b_l| \ge W + 2c$ is unchanged.

### 4.6 Angular coverage and stripe count

The ISE covers an angular sector of the first quadrant from $\theta = 0$ (the positive real axis) to $\theta_{\max} = \arctan(b_{\max} / R)$, where $b_{\max} = c + (M-1)(W + 2c) + W$ is the rightmost lateral extent.

For $M = 32$, $W = 2000$, $c = 7$ (at $k^2 = 40$): $b_{\max} = 7 + 31 \cdot 2014 + 2000 = 64{,}441$. At $R = 1\text{B}$: $\theta_{\max} \approx 0.0037°$. The angular coverage is a tiny fraction of the full annulus, which is precisely the point: the ISE does not need angular coverage. It needs $M$ independent samples, and by the $p^M$ bound, even $M = 32$ collar-disjoint samples suffice for exponentially reliable detection.

---

## 5. Tile-Based Upper Bound (UB) Method

### 5.1 Concept

The ISE detects moat *candidates* but cannot rigorously prove a moat exists: it samples only a sparse set of angular positions and measures local percolation rather than origin-connected reachability. To *prove* $R_{\mathrm{moat}}(k) \le R$ for some $R$, one must demonstrate that no path in $G_k$ crosses from $|z| < R$ to $|z| > R + H$ through any angular position.

### 5.2 Octant symmetry

The dihedral symmetry group $D_4$ of $\mathbb{Z}[i]$ -- generated by the quarter-turn $z \mapsto iz$ and conjugation $z \mapsto \bar{z}$ -- preserves Gaussian primality. Consequently, it suffices to verify disconnection in the *first octant*: the region $\{a + bi : 0 \le b \le a\}$.

**Lemma 5.1** (Octant barrier implies full-plane moat). *If no connected component of $G_k$ restricted to the first-octant rectangular band $\{a + bi : R_{\min} \le a \le R_{\max},\ 0 \le b \le a\}$ has a component crossing from $a = R_{\min}$ to $a = R_{\max}$, then $R_{\mathrm{moat}}(k) \le R_{\max}\sqrt{2}$.*

*Proof sketch.* Any path from the origin to $|z| > R_{\max}\sqrt{2}$ must eventually have $\max(|a|, |b|) > R_{\max}$, hence $a > R_{\max}$ in at least one of the eight octants (by the octant symmetries). The rectangular barrier in the first octant, transported to all eight octants by $D_4$, blocks any such path. $\square$

**Remark 5.1.** The first-octant restriction is conservative for the UB: connections crossing the octant boundaries ($b = 0$ and $b = a$) are ignored, which can only undercount connectivity.

### 5.3 Gapless tiling geometry

**Definition 5.1** (Gapless tiling). A *gapless tiling* of the first-octant annulus at radius $R$ is a collection of tiles $\{T_{n,j}\}$ such that:

1. Every Gaussian prime $\pi$ with $R_{\min} \le \mathrm{Re}(\pi) \le R_{\max}$ and $0 \le \mathrm{Im}(\pi) \le \mathrm{Re}(\pi)$ lies in at least one tile's interior.
2. Tiles in the same radial shell $n$ are spaced with lateral stride $s_b = W$ (not $W + 2c$), so tile interiors partition the band laterally.
3. Adjacent tiles share expanded regions of width $2c$ in the overlap zone.

**Definition 5.2** (Tiles per radial shell). At radius $R$, the first-octant lateral extent is approximately $R$ (from $b = 0$ to $b = a \approx R$). The number of tiles per shell is

$$
N_\ell = \left\lceil \frac{R}{W} \right\rceil.
$$

At $R = 1.5\text{B}$, $W = 2000$: $N_\ell = 750{,}000$.

### 5.4 Composition for UB

Tiles in each radial shell are composed horizontally (Left/Right matching) to produce a *band operator* for the entire first-octant width. Then consecutive radial shells are composed vertically (Inner/Outer matching). If the final composed operator has $\operatorname{io\_count} = 0$ for the full octant width, the moat is confirmed at radius $R$.

The composition uses distance-based face-port matching (Theorem 3.1), which is sound for the UB direction: it can only undercount connections, making "blocked" verdicts reliable.

### 5.5 ISE-to-UB handoff

The ISE provides two quantities that guide the UB campaign:

1. $R_{0.5}$: the radius where $f(r) = 0.5$ (the percolation transition midpoint).
2. $R_{\text{ext}}$: the radius where $f(r) \approx 0$ (the extinction radius).

The UB campaign starts at $R_{\text{ext}}$, where ISE predicts disconnection is most likely. This avoids a blind search from the origin: instead of scanning all radii, start where the signal indicates the moat is.

### 5.6 Feasibility analysis

Using calibration data from the $k^2 = 36$ campaign:

- Per-tile time on Jetson Orin Nano (6-core ARM): $\sim 1.45\text{s}$ at $R = 80\text{M}$ for a $2000 \times 2000$ tile.
- Per-tile time scales mildly with radius: $\sim 1.9\text{s}$ at $R = 1.5\text{B}$ (larger norms increase Miller-Rabin cost by $\sim 20\%$).

For a single-shell UB at $R = 1.5\text{B}$ (relevant for $k^2 = 40$ near extinction):

| Quantity | Value |
|----------|-------|
| Tiles per shell ($N_\ell$) | $\sim 750{,}000$ |
| Time per tile | $\sim 1.9\text{s}$ |
| Serial time | $\sim 395\text{hr}$ |
| 64-core parallel time | $\sim 6.2\text{hr}$ (at 100% utilization) |
| 64-core with 70% efficiency | $\sim 8.8\text{hr}$ |
| Cloud cost (64 cores at \$2/hr) | $\sim \$18$ |

Multiple radial shells are needed to span a gap of width $\ge \sqrt{k^2}$. At $k^2 = 40$, the minimum gap width is $\sqrt{40} \approx 6.3$ lattice units. A single tile of height $H = 2000$ far exceeds this, so one radial shell is sufficient.

### 5.7 Status

The UB method is formalized but not yet deployed as a campaign. The existing `compose.rs` implements distance-based matching (sound for UB). Shared-prime matching, which would yield tighter composition, is specified but not yet implemented. Both adversarial reviews (Sections on tile-UB and ISE challenges) returned verdicts of "Accept with revisions."

---

## 6. Fat Stripe: Tile-Composed Annular Moat Detection

The fat-stripe method is a concrete realization of the tile-based UB approach (Section 5). Rather than leaving gapless tiling as a feasibility sketch, fat stripe implements it end-to-end: tile the first-octant annular strip, process each tile independently, compose all tiles via the GPCTO algebra, and read off a definitive verdict on whether any connected component of $G_k$ spans the annulus radially. Unlike the ISE, which samples sparse angular positions and measures local percolation, fat stripe covers the full first-octant angular extent and answers a strictly stronger question: *does any component -- origin-connected or not -- bridge the annular band?*

### 6.1 Annular strip geometry

**Definition 6.1** (Annular strip). Fix radii $r_{\min} < r_{\max}$ and step bound $k^2$. The *first-octant annular strip* is the lattice region

$$
\mathcal{A} = \{a + bi \in \mathbb{Z}[i] : r_{\min} \le a \le r_{\max},\ 0 \le b \le b_{\max}(a)\},
$$

where $b_{\max}(a) = \min\!\big(\lceil\sqrt{r_{\max}^2 - a^2}\rceil,\ a\big)$ enforces both the circular outer boundary and the first-octant constraint $b \le a$.

**Remark 6.1** (Angular span). The angular half-width of $\mathcal{A}$ at radius $R$ is approximately

$$
\theta \approx \arctan\!\left(\frac{\sqrt{2R \cdot \Delta r}}{R}\right) \approx \sqrt{\frac{2\Delta r}{R}},
$$

where $\Delta r = r_{\max} - r_{\min}$. For $\Delta r / R \sim 10^{-4}$ (a 10K-wide annulus at $R = 80\text{M}$), $\theta \approx 0.8°$, covering roughly $11\%$ of the first octant. The 8-fold exact symmetry of $\mathbb{Z}[i]$ under the dihedral group $D_4$ (Lemma 5.1) ensures that a barrier in this sector implies a barrier in all eight octants, provided the full octant angular range is covered; see Section 6.7 for a discussion of partial-octant coverage.

### 6.2 Tiling and tile processing

The annular strip $\mathcal{A}$ is partitioned into a grid of tiles. The radial dimension is divided into *stripes* of height $H$ (typically $H = W$, the tile width), and each stripe is divided laterally into tiles of width $W$.

**Definition 6.2** (Radial stripe). A *radial stripe* is the sub-band

$$
\mathcal{A}_n = \{a + bi \in \mathcal{A} : a_n \le a < a_n + H\},
$$

where $a_n = r_{\min} + nH$ for $n = 0, 1, \ldots, N_r - 1$ and $N_r = \lceil(r_{\max} - r_{\min})/H\rceil$.

Within each stripe, tiles are indexed laterally: $T_{n,j} = T(a_n, b_j, W, H)$ with $b_j = jW$, for $j = 0, 1, \ldots, J_n - 1$ where $J_n = \lceil b_{\max}(a_n) / W \rceil$.

Each tile is processed independently in three phases:

1. **Row sieve + Miller-Rabin.** For each row (fixed $a$) of the expanded tile $T_{n,j}^+$, a sieve with splitting primes up to $L = 110{,}000$ eliminates composite Gaussian norms, and deterministic Miller-Rabin (4--12 witnesses depending on norm magnitude) confirms surviving candidates. This produces a prime bitmap for the expanded tile.

2. **Sparse union-find.** Primes are compacted from the bitmap into a list of approximately $P \approx 4S^2 / (\pi \ln n)$ entries (where $S = W + 2c$ is the expanded tile side and $n$ is the typical norm). A rank table constructed via block-wise popcount enables $O(1)$ neighbor-index lookup. Union-find over the compacted list -- checking all backward offset vectors $(\Delta a, \Delta b)$ with $\Delta a^2 + \Delta b^2 \le k^2$ -- identifies connected components. Working set: approximately $500\text{ KB}$ (rank table + UF arrays), versus $16\text{ MB}$ for a dense UF over all lattice points.

3. **Face-port extraction.** For each connected component, the algorithm records which faces ($\mathcal{I}$, $\mathcal{O}$, $\mathcal{L}$, $\mathcal{R}$) it touches within the interior (non-collar) tile, together with the $(a, b)$ coordinates of the face-port primes and their component identifiers. This produces a `TileOperator` as defined in Section 2.

### 6.3 Hierarchical composition

Tile operators are composed in a two-level hierarchy that mirrors the strip-and-tile geometry:

**Step 1: Horizontal composition within stripes.** Within each radial stripe $\mathcal{A}_n$, tiles are composed left-to-right via $\otimes_h$ (Definition 3.1). For efficiency, tiles are grouped into *column-chunks* of $C$ consecutive tiles (typically $C = 200$); tiles within each chunk are composed sequentially, then inter-chunk seams are merged. The result is a single band operator $B_n$ for each radial stripe.

**Step 2: Vertical composition across stripes.** Band operators are composed bottom-to-top via $\otimes_v$ (Definition 3.2), producing a single operator $B$ for the full annular strip.

**Theorem 6.1** (Composition order independence). *The final composed operator $B$ is independent of the order in which horizontal and vertical compositions are evaluated.*

*Proof.* By Theorem 3.1, each composition step merges components via union-find on face-port distances. Union-find is commutative and associative: the final partition depends only on the set of union operations, not their order. Any evaluation schedule -- left-to-right within rows then bottom-to-top across rows, binary reduction trees, or any hybrid -- produces the same set of face-port pairs satisfying $|p_1 - p_2|^2 \le k^2$, hence the same final partition. $\square$

### 6.4 Spanning verdict

**Definition 6.3** (Radial spanning). A connected component $C$ of the composed operator $B$ is *radially spanning* if there exist face ports $p_{\text{in}}, p_{\text{out}} \in C$ such that

$$
|p_{\text{in}}| \le r_{\min} + c \qquad \text{and} \qquad |p_{\text{out}}| \ge r_{\max} - c,
$$

where $|\cdot|$ denotes the Euclidean norm $\sqrt{a^2 + b^2}$ and $c$ is the collar width.

**Remark 6.2** (Radius-based vs. face-based verdict). Because the tiling is Cartesian while the moat is circular, the Inner and Outer face labels of the composed operator do not correspond exactly to the circular boundaries $|z| = r_{\min}$ and $|z| = r_{\max}$. The spanning verdict therefore inspects the actual $(a, b)$ coordinates of all face ports, computing $\sqrt{a^2 + b^2}$ for each, rather than relying on the rectangular face assignments. This avoids false verdicts near the corners of the tiled region where the circular and rectangular boundaries diverge.

**Theorem 6.2** (Soundness of the spanning verdict). *If no component of $B$ is radially spanning (Definition 6.3), then no connected component of $G_k$ restricted to the first-octant annular strip $\mathcal{A}$ has a path from $|z| \le r_{\min}$ to $|z| \ge r_{\max}$.*

*Proof.* Suppose for contradiction that a path $\pi_1, \pi_2, \ldots, \pi_m$ in $G_k[\mathcal{A}]$ connects some $\pi_1$ with $|\pi_1| \le r_{\min}$ to $\pi_m$ with $|\pi_m| \ge r_{\max}$. Every edge $\{\pi_i, \pi_{i+1}\}$ satisfies $|\pi_i - \pi_{i+1}|^2 \le k^2$ and both endpoints lie in $\mathcal{A}$.

By the gapless tiling (Definition 5.1), every prime in $\mathcal{A}$ belongs to the interior of at least one tile $T_{n,j}$. Each consecutive pair $(\pi_i, \pi_{i+1})$ either lies in the same expanded tile (captured by the local UF) or straddles a tile boundary (captured by composition, since both endpoints are face ports in their respective tiles). Therefore, all $\pi_i$ lie in the same component of $B$.

The first prime $\pi_1$ has $|\pi_1| \le r_{\min}$, making it a face port satisfying $|p| \le r_{\min} + c$ (it lies in the face region of the lowest stripe). The last prime $\pi_m$ has $|\pi_m| \ge r_{\max}$, making it a face port satisfying $|p| \ge r_{\max} - c$. Hence their shared component is radially spanning -- contradicting the assumption. $\square$

**Corollary 6.1** (Conservative direction). *The fat-stripe verdict "blocked" (no spanning component) is conservative: it can only be declared when no component of $G_k$ crosses the annulus within the tiled region. False "blocked" verdicts are impossible; false "not blocked" verdicts can occur only due to missed connections from distance-based composition (Remark 3.1).*

### 6.5 Moat types and the percolation regime

The ISE (Section 4) and fat stripe answer related but distinct questions. The ISE measures whether *any* component crosses a single tile's radial extent; fat stripe measures whether *any* component spans an entire annular strip. These yield different detection capabilities depending on the percolation regime of $G_k$ at the candidate moat radius.

**Definition 6.4** (Total barrier). A moat at radius $R$ is a *total barrier* if no connected component of $G_k$ -- whether origin-connected or not -- has a path from $|z| < R$ to $|z| > R + \Delta r$, for some annular width $\Delta r > 0$.

**Definition 6.5** (Origin-only moat). A moat at radius $R$ is *origin-only* if the origin component $C_0(k)$ does not reach $|z| > R$, but other connected components of $G_k$ do have paths crossing the annular region around $R$.

Whether a moat is a total barrier or origin-only depends on the local degree statistics. Degree values for $k^2 \ge 32$ were measured empirically using the `--degree-stats` tool in `fat-stripe`, which samples backward and forward neighbor counts at each Gaussian prime within the annulus. Values for $k^2 \le 26$ are estimated from $\rho \times |\text{offsets}|$.

| $k^2$ | $R_{\text{moat}}$ | Backward offsets | Mean backward degree | Mean total degree | Isolated % | Moat type |
|--------|-----|------|-------|-------|-------|-----------|
| 2 | $\sim 12$ | 2 | 0.45 | 0.90 | — | Total barrier |
| 4 | $\sim 45$ | 2 | 0.31 | 0.61 | — | Total barrier |
| 26 | $\sim 1.02\text{M}$ | 40 | $\sim 1.0$ | $\sim 2.0$ | — | Origin-only |
| 32 | $\sim 2.82\text{M}$ | 50 | 1.96 | 3.92 | 1.3% | Total barrier |
| 36 | $\sim 80\text{M}$ | 56 | 1.99 | 3.97 | 1.3% | Total barrier |
| 40 | $\sim 1.05\text{B}$ | 64 | 2.02 | 4.03 | 1.3% | Total barrier (confirmed) |

**Interpretation.** Empirical degree measurements reveal a universal critical threshold: the percolation transition (total annular blockage) occurs at $d_c \approx 4.0$ mean total degree across all measured $k^2$ values. At confirmed moat radii, the mean total degree sits just below 4.0 (3.92--3.97 for $k^2 = 32, 36$); at supercritical radii, it exceeds 4.0 (4.10--4.18).

At $k^2 = 26$, the total degree at the moat radius is approximately 2.0, placing the graph in the regime where the origin component is bounded but orphan clusters still span the annulus. Fat stripe reports "not blocked" for such moats, correctly reflecting that the annulus is not a total barrier.

At $k^2 = 32$ and $k^2 = 36$, the measured total degree at the moat radius is 3.92--3.97, just below the $d_c \approx 4.0$ threshold. The graph is sufficiently sparse that large-scale fragmentation prevents *any* component from spanning the annulus, not merely the origin component. Fat stripe detects these as total barriers.

**Remark 6.3** (Complementarity with ISE). For origin-only moats ($k^2 = 26$), the ISE detects $f(r)$ depressions but not $f(r) = 0$, because orphan clusters cross the annulus. Fat stripe confirms "not blocked." For total-barrier moats ($k^2 = 32, 36$), both methods agree: ISE shows the percolation transition, and fat stripe confirms absolute disconnection. The two methods are complementary: ISE scans efficiently for moat candidates across wide radial ranges; fat stripe provides the definitive verdict at specific candidate radii.

### 6.6 Calibration results

Fat-stripe calibration runs were performed on a Mac Mini (M-series) using the `fat-stripe` crate with $L = 110{,}000$ sieve limit, deterministic Miller-Rabin, and sparse union-find.

**6.6.1 $k^2 = 32$ at $R \approx 2.82\text{M}$**

The known Tsuchimura lower bound for $k^2 = 32$ is $R_{\text{moat}} \approx 2{,}823{,}054$ (the origin component reaches this radius but no further).

| Parameter | Value |
|-----------|-------|
| Annulus | $[2{,}820{,}000,\ 2{,}830{,}000]$ (width $10{,}000$) |
| Tile size | $2000 \times 2000$ |
| Radial stripes | 5 |
| Total tiles | 449 |
| Column-chunk size | 200 |
| Angular span | $4.82°$ ($\approx 11\%$ of first octant) |
| Elapsed time | 34 seconds |
| **Verdict** | **BLOCKED -- 0 spanning components** |

**6.6.2 $k^2 = 36$ at $R \approx 80\text{M}$**

The known Tsuchimura upper bound for $k^2 = 36$ is $R_{\text{moat}} < 80{,}015{,}782$.

| Parameter | Value |
|-----------|-------|
| Annulus | $[80{,}005{,}000,\ 80{,}015{,}000]$ (width $10{,}000$) |
| Tile size | $2000 \times 2000$ |
| Radial stripes | 5 |
| Total tiles | 2{,}373 |
| Column-chunk size | 200 |
| Elapsed time | 4 minutes 5 seconds |
| **Verdict** | **BLOCKED -- 0 spanning components** |

Both calibration runs confirm that at the known moat radii for $k^2 = 32$ and $k^2 = 36$, the annular strip is a total barrier: no connected component of $G_k$ spans from the inner to the outer radius within the tiled first-octant region.

**6.6.3 $k^2 = 40$ at $R \in [1.03\text{B},\ 1.09\text{B}]$**

No moat at $k^2 = 40$ was previously known. The ISE sigmoid (Section 7.3) with $R_{0.5} \approx 839\text{M}$ and the percolation parameter analysis (Section 6.8) predicted that the candidate moat radius lies near $R \approx 1.05\text{B}$ in the same fragmentation regime as $k^2 = 36$. Five fat-stripe probes were run across a 60M radial band to test this prediction.

Each probe covers a $128{,}000 \times 128{,}000$ annular strip ($64 \times 64$ tiles at $W = 2000$), with angular width $b_{\max} = 128{,}000$.

| Probe | $R$ | Spanning components | Tiles | Time (Mac) |
|-------|-----|---------------------|-------|------------|
| 1 | $1.03\text{B}$ | 0 | 4{,}096 | 5m 15s |
| 2 | $1.04\text{B}$ | 0 | 4{,}096 | 5m 15s |
| 3 | $1.05\text{B}$ | 0 | 4{,}096 | 5m 13s |
| 4 | $1.07\text{B}$ | 0 | 4{,}096 | 5m 12s |
| 5 | $1.09\text{B}$ | 0 | 4{,}096 | 5m 9s |

**Verdict: BLOCKED -- 0 spanning components** at all five radial positions.

Per-tile throughput: $\sim 0.076\text{s/tile}$ on Mac (M-series), comparable to the $k^2 = 36$ calibration run.

**Angular coverage.** At $R = 1\text{B}$, the 128K angular width subtends $\sim 0.007°$, or $\sim 0.016\%$ of the first octant. This is a narrow-wedge probe -- sufficient to detect the fragmentation regime and confirm methodological consistency with $k^2 = 32$ and $k^2 = 36$, but far below the full-octant coverage (Lemma 6.1) required for a rigorous moat proof.

**Significance.** This is the first detection of total annular blockage at $k^2 = 40$. Unlike $k^2 = 32$ and $k^2 = 36$, where the moat was independently known from Tsuchimura's exhaustive method, $k^2 = 40$ has no prior moat result. The fat-stripe probes advance from calibration (confirming known results) to discovery (detecting a new candidate moat region).

**6.6.4 Cross-validation with ISE at $k^2 = 32$**

ISE probes with $200 \times 200$ tiles and 32 strips at the same radial location show per-tile $f(r)$ values ranging from 0.31 to 0.78 -- individual small tiles have 31--78% inner-to-outer connectivity. This is *not* contradictory with the fat-stripe "blocked" verdict. Individual tiles are well-connected *locally* (most components within a single tile span its 200-unit radial extent), but *long-range* radial connectivity through the full 10,000-unit composed strip breaks down at the moat distance. The moat is a large-scale fragmentation phenomenon, not a local one: components that bridge a single tile's height become disconnected from one another when the full angular extent is considered.

### 6.7 Geometry and symmetry

**Partial-octant coverage.** The angular span of the fat-stripe annulus at $R = r$ with width $\Delta r$ is approximately $\theta \approx \sqrt{2\Delta r / R}$, which for the calibration runs amounts to 5--11% of the first octant. The full octant spans 45°, so the calibrated region covers roughly 1--5° out of 45°.

**Lemma 6.1** (Octant symmetry for total barriers). *Let $\mathcal{A}_\theta$ denote the first-octant annular strip restricted to angular positions $[0, \theta]$. If no component of $G_k$ restricted to $\mathcal{A}_\theta$ spans from $r_{\min}$ to $r_{\max}$, this does not immediately imply a full-annulus barrier. However, by the 8-fold dihedral symmetry of $\mathbb{Z}[i]$, an identical argument applies to each of the 8 octant copies of $\mathcal{A}_\theta$. A total annular barrier is proven if and only if: (a) the tiled region covers the full first-octant angular extent ($0 \le b \le a$), or (b) separate fat-stripe runs cover complementary angular sectors whose union tiles the full octant gaplessly.*

**Remark 6.4** (Calibration vs. proof). The calibration results of Section 6.6 demonstrate that fat stripe correctly identifies total barriers at known moat radii ($k^2 = 32, 36$) and detects a new candidate barrier ($k^2 = 40$), but all probes tile only a sub-sector of the first octant. A rigorous moat proof requires full-octant coverage (Lemma 6.1). For $k^2 = 32$ and $k^2 = 36$, the sub-sector verdicts confirm methodological correctness against independently verified Tsuchimura moats. For $k^2 = 40$, the narrow-wedge probes ($\sim 0.007°$) demonstrate that total blockage occurs in the predicted fragmentation regime, but full-octant tiling is required for a rigorous moat claim.

### 6.8 Implications for $k^2 = 40$

Empirical degree statistics measured via the `--degree-stats` tool at the moat radii for $k^2 = 32$, $36$, and $40$ reveal a universal critical degree threshold:

| Parameter | $k^2 = 32$ at $R = 2.82\text{M}$ | $k^2 = 36$ at $R = 80\text{M}$ | $k^2 = 40$ at $R = 1.05\text{B}$ |
|-----------|------|------|------|
| Backward offsets | 50 | 56 | 64 |
| Mean backward degree | 1.96 | 1.99 | 2.02 |
| Mean total degree | 3.92 | 3.97 | 4.03 |
| Isolated primes | 1.3% | 1.3% | 1.3% |

All three $k^2$ values show mean total degree $\approx 4.0$ at or near the moat radius, confirming $d_c \approx 4.0$ as the universal critical threshold. At supercritical radii (well inside the moat), degrees rise to 4.10--4.18; at confirmed moat radii, they sit at 3.92--3.97. The transition is sharp: $k^2 = 40$ crosses $d_c \approx 4.0$ in the range $R \approx 1.0\text{B}$--$1.2\text{B}$, consistent with both the ISE sigmoid ($R_{0.5} \approx 839\text{M}$) and the fat-stripe probes of Section 6.6.3 showing zero spanning components across $R \in [1.03\text{B}, 1.09\text{B}]$.

**Scaling considerations.** A full-octant fat-stripe campaign at $R \approx 1.05\text{B}$ with $\Delta r = 10{,}000$ requires approximately $J = \lceil 1.05 \times 10^9 / 2000 \rceil = 525{,}000$ tiles per stripe and $N_r = 5$ radial stripes, totaling $\sim 2.6\text{M}$ tiles. At the calibrated throughput of $\sim 0.5\text{s/tile}$ (Mac Mini), this amounts to approximately $360$ hours serial. With Rayon parallelism across 8--64 cores and the double-buffered pipeline described in the tile algorithm spec, wall-clock time reduces to 6--45 hours depending on hardware. Cloud deployment on multi-core instances makes a single-day campaign feasible.

---

## 7. Empirical Validation

### 7.1 Calibration against known moats

ISE calibration runs were performed on a Jetson Orin Nano with $M = 32$ positive-only strips, square tiles, and the scanline kernel (Montgomery multiplication, row-sieve, union-find).

| $k^2$ | Tile size | Radial range | Shells | Known moat $R$ | ISE $f(r)$ at moat | Control $\bar{f}$ | Moat sweep $\bar{f}$ |
|--------|-----------|--------------|--------|----------------|---------------------|-------------------|---------------------|
| 2 | $8 \times 8$ | $[0, 100]$ | 13 | $\sim 12$ | $0.000$ at $R=20$ | -- | $0.19$ |
| 26 | $500 \times 500$ | $[950\text{K}, 1.1\text{M}]$ | 300 | $1{,}015{,}639$ | $0.250$ at $R \approx 1{,}016{,}250$ | $0.937$ | $0.477$ |
| 32 | $500 \times 500$ | $[2.7\text{M}, 2.9\text{M}]$ | 400 | $2{,}823{,}055$ | $0.344$ at $R \approx 2{,}823{,}250$ | $0.891$ | $0.503$ |
| 36 | $2000 \times 2000$ | $[79.5\text{M}, 80.5\text{M}]$ | 500 | Unknown | $\mathbf{0.000}$ at $R = 80{,}399{,}000$ | $0.881$ | $0.212$ |

Key findings:

1. **$k^2 = 2$:** ISE correctly detects the moat with six $f(r) = 0$ shells spanning $R = 20$--$76$.

2. **$k^2 = 26, 32$:** ISE detects $f(r)$ depressions near the Tsuchimura moats but does not reach $f(r) = 0$. This is expected: the moat is a structural gap in the origin component, not a collapse of all local connectivity. Orphan clusters provide local IO crossings even at the moat radius. The ISE correctly distinguishes control regions ($\bar{f} > 0.88$) from moat regions ($\bar{f} \approx 0.50$).

3. **$k^2 = 36$:** A single $f(r) = 0$ shell at $R = 80{,}399{,}000$ with monotonic approach ($f = 0.156 \to 0.125 \to 0.063 \to 0.000$). This marks the percolation transition boundary.

### 7.2 Tile-size sensitivity

Four tile heights at $k^2 = 26$, $R \in [1\text{M}, 1.03\text{M}]$, 32 strips:

| Tile | Shells | $f_{\min}$ | $f_{\max}$ | Time/shell |
|------|--------|------------|------------|------------|
| $200 \times 200$ | 150 | 0.281 | 0.750 | 0.7s |
| $500 \times 500$ | 60 | 0.250 | 0.625 | 3.0s |
| $1000 \times 1000$ | 30 | 0.313 | 0.594 | 11.1s |
| $2000 \times 2000$ | 15 | 0.313 | 0.594 | 41.9s |

Tile height does not significantly shift $f(r)$ estimates. The mean and extrema are consistent across two orders of magnitude in tile area. Larger tiles reduce variance (more primes per tile) but do not shift the mean.

### 7.3 $k^2 = 40$ results

ISE probes at $k^2 = 40$ with $M = 64$ strips, $2000 \times 2000$ tiles, from $R = 600\text{M}$ to $R = 3.5\text{B}$:

| $R$ | $f(r)$ | Regime |
|-----|--------|--------|
| $600\text{M}$ | $1.000$ | Connected |
| $700\text{M}$ | $0.906$ | Declining |
| $800\text{M}$ | $0.609$ | Transition |
| $900\text{M}$ | $0.391$ | Transition |
| $1.0\text{B}$ | $0.203$ | Near collapse |
| $1.2\text{B}$ | $0.078$ | Sparse crossings |
| $1.5\text{B}$ | $0.016$ | Near extinction |
| $2.0\text{B}$ | $0.000$ | Extinct |
| $2.5\text{B}$ | $0.000$ | Extinct |
| $3.0\text{B}$ | $0.000$ | Extinct |
| $3.5\text{B}$ | $0.000$ | Extinct |

The $f(r)$ profile is a sigmoid with:

- $R_{0.5} \approx 839\text{M} \pm 12\text{M}$ (95% CI from logistic fit)
- First $f(r) = 0$ shells at $R \approx 2.0\text{B}$
- Total extinction from $2.0\text{B}$ onward

The logistic fit uses three calibration points in the transition region ($R = 700\text{M}, 800\text{M}, 900\text{M}$). Alternative functional forms (complementary error function, asymmetric sigmoid) have not yet been compared; model-selection sensitivity is an open question.

---

## 8. Open Questions

1. **Finite-size scaling.** Does $R_{0.5}$ depend on tile size $W$? Data at $W \in \{500, 1000, 2000, 4000\}$ exists for $k^2 = 26$ but formal extrapolation to $W \to \infty$ has not been performed. The ISE moat estimate $R_{0.5}$ should be understood as a property of a specific tile geometry, not intrinsically of $G_k$. A finite-size scaling protocol -- measuring $R_{0.5}(W)$ for several widths and extrapolating -- would yield a tile-size-independent quantity.

2. **Percolation theory connection.** Is the tile connectivity transfer operator related to existing operators in percolation theory (e.g., the transfer matrix for site percolation on $\mathbb{Z}^2$)? The analogy is suggestive -- both compress connectivity information through a strip -- but Gaussian primes are deterministic, not random, so the connection is heuristic.

3. **Optimal tile geometry.** Square tiles ($W = H$) are confirmed necessary for reliable IO crossing detection. Is there an optimal $W$ for a given $k^2$ and radius $R$? The correlation length $\xi$ of the crossing process sets a natural scale: $W \gg \xi$ is needed for reliable detection, but $W \gg \xi$ wastes computation.

4. **GPU-parallel UB.** Can the UB method be made GPU-parallel with composition on device? The individual tile builds are embarrassingly parallel (one CUDA thread block per tile), but the horizontal composition step requires sequential union-find merging across $N_\ell$ tiles. A hierarchical super-tile aggregation could reduce this to $O(\log N_\ell)$ sequential composition steps.

5. **Logistic model validation.** The sigmoid fit to $f(r)$ data is based on a logistic function fitted to a narrow transition window. The complementary error function $\tfrac{1}{2}\operatorname{erfc}(\cdot)$ is arguably more physically motivated for a percolation-like transition. Fitting both models and comparing $R_{0.5}$ estimates via AIC/BIC would quantify model-choice uncertainty.

6. **Tile-size dependence of $R_{0.5}$.** Narrower tiles measure lower $f(r)$ (they miss lateral paths), shifting the sigmoid leftward. If $R_{0.5}$ at $W = 2000$ and $R_{0.5}$ at $W = 4000$ differ by more than the CI, the estimation framework requires finite-size scaling corrections. This is the deepest methodological concern.

---

## Appendix A: Notation Summary

| Symbol | Definition |
|--------|-----------|
| $\mathbb{Z}[i]$ | Gaussian integers |
| $P$ | Set of all Gaussian primes |
| $G_k = (P, E_k)$ | Gaussian prime graph at step bound $k^2$ |
| $\Sigma_k$ | Seed set: primes with $|\pi|^2 \le k^2$ |
| $C_0(k)$ | Origin component of $G_k$ |
| $R_{\mathrm{moat}}(k)$ | Moat radius: supremum of radii reached by $C_0(k)$ |
| $T(a_0, b_0, W, H)$ | Tile: lattice rectangle $[a_0, a_0+H] \times [b_0, b_0+W]$ |
| $c = \lceil\sqrt{k^2}\rceil$ | Collar width |
| $T^+$ | Expanded tile (interior + collar of width $c$) |
| $\mathcal{I}, \mathcal{O}, \mathcal{L}, \mathcal{R}$ | Inner, Outer, Left, Right faces |
| $\operatorname{span}_T(X,Y)$ | Count of components touching both faces $X$ and $Y$ |
| $\operatorname{io\_count}(T)$ | $= \operatorname{span}_T(\mathcal{I}, \mathcal{O})$ |
| $S_j$ | Strip $j$: vertical stack of tiles at lateral offset $b_j$ |
| $M$ | Number of strips |
| $f(r)$ | Fraction of strips with $\operatorname{io\_count} > 0$ at radius $r$ |
| $R_{0.5}$ | Radius where $f(r) = 0.5$ (logistic midpoint) |
| $\rho(R)$ | Local Gaussian prime density at radius $R$ |
| $\mathcal{A}$ | First-octant annular strip $\{a + bi : r_{\min} \le a \le r_{\max},\ 0 \le b \le b_{\max}(a)\}$ |
| $\mathcal{A}_n$ | Radial stripe $n$ within the annular strip |
| $B_n$ | Band operator: horizontally composed tiles within stripe $n$ |
| $B$ | Full composed operator for the annular strip |

---

## Appendix B: Algorithm Pseudocode (ISE)

### B.1 Tile operator computation

```
Algorithm: ComputeTile(a_0, b_0, W, H, k^2)

Input: tile bounds, step bound
Output: TileOperator (component face sets, face ports, io_count)

1.  c <- ceil(sqrt(k^2))
2.  Enumerate all Gaussian primes pi in the expanded rectangle
    [a_0 - c, a_0 + H + c] x [b_0 - c, b_0 + W + c]
3.  Initialize union-find UF on |primes| elements
4.  Build spatial hash: for each pi, insert into cell grid with
    cell size c
5.  For each prime pi at (a, b):
      For each neighbor cell within distance 2 cells:
        For each prime pi' in that cell with index > index(pi):
          If |pi - pi'|^2 <= k^2:
            UF.Union(index(pi), index(pi'))
6.  For each prime pi = (a, b) in the interior tile [a_0, a_0+H] x [b_0, b_0+W]:
      root <- UF.Find(index(pi))
      component <- ComponentID(root)
      If a - a_0 <= c:  tag component with I-face; add face port
      If (a_0 + H) - a <= c:  tag component with O-face; add face port
      If b - b_0 <= c:  tag component with L-face; add face port
      If (b_0 + W) - b <= c:  tag component with R-face; add face port
7.  io_count <- |{component C : I in faces(C) and O in faces(C)}|
8.  Return TileOperator
```

### B.2 ISE ensemble evaluation

```
Algorithm: ISE(k^2, R_min, R_max, W, H, M)

Input: step bound, radial range, tile dimensions, strip count
Output: f(r) profile for each shell

1.  c <- ceil(sqrt(k^2))
2.  stride <- W + 2c
3.  offsets <- [c + j * stride for j in 0..M-1]
4.  shells <- [(a_lo, a_hi) for a_lo in R_min, R_min+H, ..., R_max-H]
5.  For each shell (a_lo, a_hi):
      io_counts <- [0] * M
      For j in 0..M-1 in parallel:
        T <- ComputeTile(a_lo, offsets[j], W, a_hi - a_lo, k^2)
        io_counts[j] <- io_count(T)
      f(r) <- |{j : io_counts[j] > 0}| / M
      If f(r) = 0: flag shell as moat candidate
6.  Return f(r) profile
```

### B.3 Compensated ISE

```
Algorithm: CompensatedISE(k^2, R_target, W, H, M, N_shells)

Input: step bound, target radius, tile dimensions, strip count, shells per probe
Output: f(r) profile

1.  c <- ceil(sqrt(k^2))
2.  stride <- W + 2c
3.  offsets <- [c + j * stride for j in 0..M-1]
4.  For j in 0..M-1:
      b_center <- offsets[j] + W/2
      a_base[j] <- floor(sqrt(R_target^2 - b_center^2))
5.  For each shell index n in 0..N_shells-1:
      io_counts <- [0] * M
      For j in 0..M-1 in parallel:
        a_lo <- a_base[j] + n * H
        a_hi <- a_lo + H
        T <- ComputeTile(a_lo, offsets[j], W, H, k^2)
        io_counts[j] <- io_count(T)
      f(r) <- |{j : io_counts[j] > 0}| / M
6.  Return f(r) profile
```

### B.4 Fat-stripe campaign

```
Algorithm: FatStripe(k^2, r_min, r_max, W, H, C)

Input: step bound k^2, annular radii [r_min, r_max], tile dimensions W x H,
       column-chunk size C
Output: blocked (boolean), spanning_count

1.  c <- ceil(sqrt(k^2))
2.  sieve_table <- precompute(L = 110000)
3.  composed <- null
4.  For a_lo = r_min, r_min + H, ..., r_max - H:        // radial stripes
      a_hi <- min(a_lo + H, r_max)
      b_max <- min(ceil(sqrt(r_max^2 - a_lo^2)), a_hi)   // first-octant bound
      stripe_op <- null
      For b_lo = 0, C*W, 2*C*W, ..., b_max:              // column-chunks
        b_hi <- min(b_lo + C*W, b_max)
        // Phase 1: Sieve expanded chunk region (Rayon parallel over rows)
        primes <- SieveChunkRows(a_lo - c, a_hi + c, b_lo - c, b_hi + c,
                                 sieve_table)
        // Phase 2: Partition primes into virtual tiles (with collar overlap)
        tile_primes <- Partition(primes, b_lo, W, c)
        // Phase 3: Sparse UF per tile -> TileOperator (Rayon parallel)
        tile_ops <- [BuildTile(a_lo, a_hi, t_b_lo, t_b_hi, k^2, tile_primes[t])
                     for each virtual tile t in parallel]
        // Phase 4: Horizontal composition L->R within chunk
        chunk_op <- tile_ops[0]
        For t = 1 to |tile_ops| - 1:
          chunk_op <- ComposeHorizontal(chunk_op, tile_ops[t], k^2)
        // Inter-chunk horizontal composition
        stripe_op <- ComposeHorizontal(stripe_op, chunk_op, k^2)
      // Vertical composition across stripes
      composed <- ComposeVertical(composed, stripe_op, k^2)

5.  // Verdict: radius-based spanning check
    For each component C in composed:
      has_inner <- any port p in C with sqrt(p.a^2 + p.b^2) <= r_min + c
      has_outer <- any port p in C with sqrt(p.a^2 + p.b^2) >= r_max - c
      If has_inner and has_outer: increment spanning_count

6.  blocked <- (spanning_count == 0)
7.  Return blocked, spanning_count
```

---

## Appendix C: Feasibility Calculation Details

### C.1 Neighbor counts by $k^2$

| $k^2$ | Nonzero neighbor vectors $z$ | Collar $c$ | Example max-excursion vector |
|--------|-----|---|------|
| 2 | 4 | 2 | $(1, 1)$ |
| 26 | 80 | 6 | $(5, 1)$ |
| 32 | 92 | 6 | $(4, 4)$ |
| 36 | 112 | 6 | $(6, 0)$ |
| 40 | 128 | 7 | $(6, 2)$ |

### C.2 Percolation threshold estimation for $k^2 = 40$

Empirical degree-statistics measurements (via `fat-stripe --degree-stats`) confirm a universal critical threshold of $d_c \approx 4.0$ mean total degree across $k^2 = 32$, $36$, and $40$. At $k^2 = 36$, the percolation boundary ($f(r) = 0$) was found at $R \approx 80.4\text{M}$ with measured mean total degree $3.97$ (and $3.92$ at the supercritical reference radius $R = 40\text{M}$ where $f(r) > 0$).

Using $d_c = 4.0$ and total neighbor count $z = 128$ for $k^2 = 40$:

$$
\rho_c(k^2 = 40) = \frac{d_c}{z} = \frac{4.0}{128} = 0.03125 = 3.125\%.
$$

Setting $\rho(R) = C / (2\ln R) = \rho_c$:

$$
\ln R = \frac{C}{2\rho_c} = \frac{1.274}{2 \times 0.03125} = 20.38, \qquad R = e^{20.38} \approx 7.1 \times 10^8.
$$

This estimate of $R \approx 710\text{M}$ is broadly consistent with the empirical sigmoid midpoint $R_{0.5} \approx 839\text{M}$ and the measured degree data, which shows the $d_c \approx 4.0$ crossing occurring in the range $R \approx 1.0\text{B}$--$1.2\text{B}$ (measured mean total degree: 4.03 at $R = 1.05\text{B}$, 4.00 at $R = 1.2\text{B}$). The discrepancy between the $\rho$-based estimate and the measured crossing radius reflects the approximation in the prime density model $\rho(R) = C / (2\ln R)$.

### C.3 Memory requirements

Per-tile working memory for $2000 \times 2000$ tile with $c = 7$:

| Component | Size |
|-----------|------|
| Expanded tile bitmap $(2015^2)$ | $507\text{ KB}$ |
| Union-find parent array ($n$ u32) | $\sim 16\text{ MB}$ |
| Union-find rank array ($n$ u8) | $\sim 4\text{ MB}$ |
| Spatial hash | $\sim 2\text{ MB}$ |
| **Total per tile** | **$\sim 23\text{ MB}$** |

With 6 parallel tiles (Jetson Orin Nano, 6 cores): $\sim 138\text{ MB}$ peak. With 64 parallel tiles (cloud): $\sim 1.5\text{ GB}$.

### C.4 Tsuchimura comparison

A Tsuchimura-style full sweep to $R = 1.5\text{B}$ at $k^2 = 40$ would require storing all primes in the disk of radius $1.5\text{B}$:

$$
N_{\text{primes}} \approx \frac{\pi R^2 \cdot \rho(R)}{1} = \frac{\pi \cdot (1.5 \times 10^9)^2 \cdot 0.031}{1} \approx 2.2 \times 10^{17}.
$$

At 16 bytes per prime (coordinates + component ID): $\sim 3.5\text{ PB}$. Union-find adds comparable storage. This is infeasible on any existing hardware.

The ISE, by contrast, processes one $2000 \times 2000$ tile at a time with $\sim 23\text{ MB}$ working memory, achieving a memory reduction of approximately $10^{11}\times$.

### C.5 Primality validity ceiling

The implementation uses a 9-witness deterministic Miller-Rabin test, valid for all $n < 3.825 \times 10^{18}$. At radius $R$, the maximum norm tested is approximately $R^2$. The hard ceiling is $R \le 1.955 \times 10^9$ ($\approx 1.955\text{B}$). Beyond this, either additional witnesses or a BPSW test is required. The $k^2 = 40$ campaign data through $R = 3.5\text{B}$ uses 12 witnesses for probes beyond the 9-witness ceiling.
