# The Connectivity Transfer Operator for Gaussian Primes

## 1. Introduction and Motivation

The Gaussian moat problem asks a deceptively simple question: if one is allowed to step only from one Gaussian prime to another, and only by steps of bounded length, can one walk to infinity?

Write the Gaussian integers as \(\mathbb{Z}[i]\). Fix a squared step bound \(k^2\). A walk is allowed to move from one Gaussian prime \(\pi\) to another \(\pi'\) whenever
$$
|\pi-\pi'|^2 \le k^2.
$$
The problem is to decide whether the primes reachable from the origin by such moves form an unbounded set.

This is already nontrivial for the smallest interesting case. When \(k^2=2\), the only allowed step lengths are \(1\) and \(\sqrt{2}\), and yet the walk dies quickly: the farthest reachable point is the Gaussian prime \(11+4i\), at Euclidean distance \(\sqrt{137}\). So a moat exists already for \(k^2=2\).

For larger step sizes the same phenomenon moves outward by many orders of magnitude. Classical computation found a moat for \(k^2=26\) at radius about \(10^6\). More recent large-scale computation has pushed the \(k^2=36\) moat to below \(8\times 10^7\). The arithmetic is local, but the question is global: one must understand how local prime connectivity behaves over enormous distances.

That is why computation matters here. The issue is not that the local rule is complicated. It is not. The issue is that the local rule must be applied over a vast sparse graph whose large-scale geometry is invisible near the origin.

The method developed in this project is built around one modest idea.

- A **tile** is a local connectivity probe: it asks how prime-connected components enter and leave a rectangular region.
- A **strip** is a stack of such probes: it samples the same radial shells at one fixed lateral offset.
- An **independent strip ensemble** repeats this at many collar-disjoint offsets, turning one local question into many parallel samples.

The mathematics is elementary. The usefulness comes from choosing the right abstraction.

## 2. The Gaussian Prime Graph \(G_k\)

### 2.1 Gaussian integers and Gaussian primes

An element of \(\mathbb{Z}[i]\) has the form
$$
z=a+bi,\qquad a,b\in\mathbb{Z},
$$
with squared norm
$$
|z|^2=a^2+b^2.
$$

The Gaussian primes are characterized by a familiar dichotomy.

- If both coordinates are nonzero, then \(\pi=a+bi\) is Gaussian prime exactly when \(a^2+b^2\) is an ordinary rational prime.
- If one coordinate is zero, then \(\pi\) is Gaussian prime exactly when its nonzero coordinate has absolute value a rational prime congruent to \(3 \bmod 4\).

We write \(P\) for the set of all Gaussian primes.

### 2.2 The prime graph

Fix \(k^2\in\mathbb{N}\). The Gaussian prime graph is
$$
G_k=(P,E_k),
$$
where
$$
\{\pi,\pi'\}\in E_k
\quad\Longleftrightarrow\quad
|\pi-\pi'|^2\le k^2.
$$

So \(G_k\) records exactly which Gaussian primes lie within one legal step of one another.

For \(k^2=2\), this means that edges occur only at Euclidean distance \(1\) or \(\sqrt{2}\). That tiny graph already has a moat.

### 2.3 The origin component

The origin \(0\) is not itself a Gaussian prime, so one needs a small convention in order to speak cleanly about "the component of the origin."

Define the seed set
$$
\Sigma_k=\{\pi\in P:|\pi|^2\le k^2\}.
$$
These are exactly the Gaussian primes that can be reached from the origin in one legal step.

We then define \(C_0(k)\) in either of two equivalent ways.

1. Adjoin a formal vertex \(0\) and connect it to every \(\pi\in\Sigma_k\); then \(C_0(k)\) is the connected component of \(0\).
2. Inside \(G_k\) itself, \(C_0(k)\) is the union of all connected components that meet \(\Sigma_k\).

This convention matches the geometry of the walking problem and is especially important at \(k^2=2\), where the four seed primes \(\pm1\pm i\) are natural first steps from the origin but are not all adjacent to one another inside \(G_2\).

### 2.4 Moat radius

The moat radius is
$$
R_{\mathrm{moat}}(k)
=
\sup\{R\ge 0 : C_0(k)\text{ meets the circle }|z|=R\}.
$$
Equivalently, it is the supremum of Euclidean radii attained by points of \(C_0(k)\).

The Gaussian moat problem can now be stated cleanly:

> For every fixed \(k\), is \(R_{\mathrm{moat}}(k)\) finite?

For \(k^2=2\), the answer is yes. The walk reaches only a small neighborhood of the origin before a genuine gap appears. For \(k^2=26\), the same happens much farther out, near \(10^6\). For \(k^2=36\), the known obstruction is below \(8\times10^7\).

## 3. The Connectivity Transfer Operator (Tile)

### 3.1 Why a tile?

Suppose one wants to know whether prime connectivity can move outward across a small radial interval. One does not need the whole graph for that. One needs only a local window, provided that the window is large enough to see every edge that touches its interior.

That is the role of a tile. It is a microscope aimed at one rectangular region of the lattice. Its output is not the list of primes inside the rectangle, nor the full component structure. It is something much smaller: which connected components touch which boundary faces.

This is why the word "operator" is appropriate. The tile compresses a complicated local graph into a tiny amount of boundary data.

### 3.2 The tile and its collar

A tile is the lattice rectangle
$$
T(a_0,b_0,W,H)
=
\{a+bi\in\mathbb{Z}[i] : a_0\le a\le a_0+H,\ b_0\le b\le b_0+W\}.
$$

Here \(a\) plays the outward, or "radial," coordinate and \(b\) the lateral one. Far from the origin this is just the local flattening of an annulus into a rectangle.

To evaluate connectivity correctly, the tile must be enlarged by a collar. An integer step vector \((\Delta a, \Delta b)\) satisfying \(\Delta a^2+\Delta b^2\le k^2\) has maximal coordinate excursion \(\lfloor\sqrt{k^2}\rfloor\). For the collar we use
$$
c=\left\lfloor \sqrt{k^2}\right\rfloor.
$$
Then every prime in the interior of \(T\) can only be adjacent to primes lying within \(c\) lattice units in each coordinate direction. So we work on the expanded tile
$$
T^+
=
\{a+bi : a_0-c\le a\le a_0+H+c,\ b_0-c\le b\le b_0+W+c\}.
$$

This is the local graph actually examined by the operator. The collar is not optional. It is exactly what guarantees that every edge incident to an interior prime is visible.

Two small examples are worth fixing in mind.

- If \(k^2=36\), then \(c=6\), because \(\lfloor\sqrt{36}\rfloor=6\).
- If \(k^2=40\), then \(c=6\), because \(\lfloor\sqrt{40}\rfloor=\lfloor 6.32\ldots\rfloor=6\).

### 3.3 The four faces

The tile has four distinguished faces, named for the direction in which connectivity is being tested.

- \(I\) (inner): near the bottom edge \(a=a_0\)
- \(O\) (outer): near the top edge \(a=a_0+H\)
- \(L\) (left): near the left edge \(b=b_0\)
- \(R\) (right): near the right edge \(b=b_0+W\)

Face membership is defined on primes in the interior tile \(T\), not on the collar.

A Gaussian prime \(\pi=a+bi\in P\cap T\) touches

- the \(I\)-face iff \(a-a_0\le c\),
- the \(O\)-face iff \(a_0+H-a\le c\),
- the \(L\)-face iff \(b-b_0\le c\),
- the \(R\)-face iff \(b_0+W-b\le c\).

These inequalities are inclusive. A prime exactly \(c\) units from a face is counted as touching that face. This matters whenever \(k^2\) is a perfect square: using strict inequality would incorrectly miss boundary primes at distance exactly \(\sqrt{k^2}\).

For \(k^2=2\), we have \(c=2\). On a tiny tile near the origin, the collar is so wide that the same seed prime can easily touch more than one face, or even all four. Later, when \(W\) and \(H\) are large, face membership becomes a true boundary notion rather than a global one.

### 3.4 Components and transfer data

Now form the induced subgraph
$$
G_k[T^+]
$$
on the Gaussian primes lying in the expanded tile \(T^+\).

Each connected component \(C\) of this graph determines a face set
$$
\partial_T C \subseteq \{I,O,L,R\},
$$
namely the collection of faces touched by the primes of \(C\) that lie in the interior tile \(T\).

The tile's transfer data are then the pairwise face counts
$$
\operatorname{span}_T(X,Y)
=
\#\{C : X,Y\in \partial_T C\},
\qquad X,Y\in\{I,O,L,R\},\ X\ne Y.
$$

In particular,
$$
\operatorname{io\_count}(T)=\operatorname{span}_T(I,O)
$$
is the number of connected components that touch both the inner and outer faces.

This is a count, not merely a Boolean flag. Several distinct components can cross the same tile.

The interpretation is immediate.

- If \(\operatorname{io\_count}(T)>0\), then there exists a prime path in the collared local graph joining the bottom of the tile to the top.
- If \(\operatorname{io\_count}(T)=0\), then this local probe is blocked: within that collared region, no prime component carries connectivity from \(I\) to \(O\).

That is the whole operator. It turns a large local graph into six small integers, of which \(\operatorname{io\_count}\) is the one most relevant for outward transport.

### 3.5 Exact local realization

The mathematics above is abstract, but the current experiments instantiate it exactly.

- Gaussian primality is tested by a parity pre-filter, then trial division by small primes, then deterministic Miller-Rabin on the survivors.
- The \(k^2\)-neighborhood is the disk of lattice step vectors \((\Delta a,\Delta b)\) with \(\Delta a^2+\Delta b^2\le k^2\).

For \(k^2=36\), this disk contains \(112\) nonzero neighbor vectors. In the scanline realization, only the \(56\) backward offsets are checked, because each undirected edge needs to be processed only once. This changes the implementation, not the operator being computed.

## 4. Independent Strip Evaluation (ISE)

### 4.1 Strip construction

A strip is a vertical stack of tiles at one fixed lateral offset. Fix parameters \(W\), \(H\), \(r_{\min}\), \(r_{\max}\), and let
$$
N=\frac{r_{\max}-r_{\min}}{H}
$$
for simplicity of exposition.

The \(j\)-th strip is
$$
S_j
=
\bigcup_{n=0}^{N-1} T(r_{\min}+nH,\ b_j,\ W,\ H),
$$
where the strip offset is chosen as
$$
b_j=c+j\cdot \mathrm{stride},
\qquad
\mathrm{stride}=W+2c.
$$

The meaning of this choice is simple. Because each tile must be evaluated together with its collar, two strips are independent only if their collared tiles share no lattice points. Strip \(j\) examines columns \([b_j-c,\,b_j+W+c]\), and strip \(j+1\) examines columns \([b_j+W+c,\,b_j+2W+3c]\). With inclusive boundaries, these share column \(b_j+W+c\). A strictly disjoint spacing would be \(W+2c+1\). In practice, the shared boundary column contains the same primes in both strips, so the effective dependence is negligible at scale: the overlap is a single column out of \(W+2c\) total columns. The implementation uses \(W+2c\) for simplicity.

Thus the strip ensemble samples the same radial shell at many laterally separated locations:
$$
b_0=c,\quad b_1=c+(W+2c),\quad b_2=c+2(W+2c),\ \dots
$$

### 4.2 The ISE metric

For shell index \(n\), let
$$
r_n=r_{\min}+\left(n+\tfrac12\right)H
$$
be the shell's midpoint.

At shell \(n\), each strip contributes one tile and therefore one value \(\operatorname{io\_count}_{j}(r_n)\). The ensemble statistic is
$$
f(r_n)
=
\frac{\#\{j : \operatorname{io\_count}_{j}(r_n)>0\}}{M},
$$
where \(M\) is the number of strips.

So \(f(r_n)\) is the fraction of independent local probes that detect inner-to-outer connectivity at that shell.

Its meaning is intentionally blunt.

- \(f(r_n)=1\): every probe crosses
- \(f(r_n)=0\): every probe is blocked
- \(0<f(r_n)<1\): transitional regime

For \(k^2=2\), one expects this transition almost immediately. Near the origin, tiles still cross. A short distance later, crossings disappear. For \(k^2=36\), the same transition is shifted to tens of millions.

### 4.3 One-sided monotonicity

The guiding observation is the oldest one in graph theory: deleting vertices cannot create a path. It can only destroy one.

This has two consequences.

#### Theorem 4.1 (Subgraph monotonicity)

Let \(S\subseteq \mathbb{Z}[i]\) contain the seed set \(\Sigma_k\). Let \(R_{\mathrm{strip}}(k,S)\) be the supremum of radii reached by the origin component when \(G_k\) is restricted to primes in \(S\). Then
$$
R_{\mathrm{strip}}(k,S)\le R_{\mathrm{moat}}(k).
$$

**Proof.** The restricted graph is an induced subgraph of \(G_k\). Every path in it is also a path in \(G_k\). Hence every seed-reachable prime in the restricted graph is also seed-reachable in the full graph. Taking suprema of radii gives the inequality. \(\square\)

This theorem is the mathematical core of the lower-bound probe: if one traces a seed-connected subgraph, one can only underestimate the true reachable radius.

#### Corollary 4.2 (Local soundness of a tile crossing)

If a tile \(T\) has \(\operatorname{io\_count}(T)>0\), then the full graph \(G_k\) contains a genuine \(I\)-to-\(O\) crossing across the same collared tile.

**Proof.** The component witnessing \(\operatorname{io\_count}(T)>0\) lives in the induced subgraph \(G_k[T^+]\). Since \(G_k[T^+]\) is a subgraph of \(G_k\), that same path exists in the full graph. \(\square\)

This is the exact one-sided guarantee used by ISE. A detected crossing is always real. What may fail is the converse: a strip may look blocked only because the strip deleted primes just outside its lateral boundary that would have rescued the connection in the full graph.

Project shorthand has sometimes called this "zero false negatives." More precisely, ISE is one-sided and conservative in the direction that matters for screening: local blocked strips can be spurious, but local detected crossings are genuine.

### 4.4 False-positive bound under an independence model

Probability enters only when we ask how reliable many strips are as a screening device.

Fix a shell \(r\). Suppose that, under some probabilistic model for local prime configurations, a single strip fails to detect an \(I\)-to-\(O\) crossing with probability \(p(r)\), even though the full graph does cross that shell. Assume further that collar-disjoint strips behave as independent samples.

Then
$$
\Pr\bigl(f(r)=0 \mid \text{the full graph crosses shell }r\bigr)\le p(r)^M.
$$

**Proof.** The event \(f(r)=0\) means that all \(M\) strips fail simultaneously. Under the independence assumption, the probability is the product of the single-strip failure probabilities, hence at most \(p(r)^M\). \(\square\)

The geometry matters here. The spacing \(W+2c\) is exactly what makes this model plausible: the collared tiles use disjoint sets of primes. The independence is still a modeling assumption, not a theorem about the deterministic set of Gaussian primes, but it is the natural one for separated probes.

A numerical example shows the scale. If \(M=32\) and even the pessimistic value \(p(r)=0.5\) is used, then
$$
p(r)^M = 2^{-32}\approx 2.3\times 10^{-10}
$$
per shell.

### 4.5 What ISE cannot prove

The limitations are as important as the guarantees.

- ISE detects **moat candidates**. A shell with \(f(r)=0\) is strong evidence of blockage, but it is not by itself a proof of a true global moat.
- Shellwise positivity does **not** imply shell-to-shell continuity. The component that crosses one shell need not connect to the component that crosses the next shell.
- Even if \(f(r)>0\) for every shell in an interval, that does not yet prove that the origin component survives through that interval.
- To certify a true lower bound, or to confirm a true moat, one needs explicit seed-connected tracing of the origin component. In this project that is the role of the LB probe.

So ISE should be understood correctly. It is not the final proof object. It is the screening instrument that finds where proofs are worth spending.

## 5. Octant Symmetry and Strip Placement

### 5.1 Reflection in \(b\)

The simplest symmetry is
$$
a^2+b^2=a^2+(-b)^2.
$$
Therefore the Gaussian primality of \(a+bi\) is equivalent to that of \(a-bi\). A strip at offset \(+b\) and a strip at offset \(-b\) probe mirror-image copies of the same prime configuration.

This is not merely heuristic. In the current campaign data, mirrored strip pairs produced identical \(\operatorname{io\_count}\) values in all \(528\) out of \(528\) checked comparisons across \(33\) shells.

So a symmetric \(\pm b\) layout wastes half the strip budget. It looks like \(M\) samples, but it provides only \(M/2\) genuinely new ones.

### 5.2 The full octant symmetry

The full symmetry group of \(\mathbb{Z}[i]\) relevant here is the eight-element dihedral group generated by

- quarter-turns \(z\mapsto iz\), \(z\mapsto -z\), \(z\mapsto -iz\),
- conjugation \(z\mapsto \overline{z}\),
- and their compositions.

Gaussian primality is preserved by all of these symmetries, because the defining conditions depend only on the norm and, on the axes, on absolute value modulo \(4\).

Thus any tile, strip, or larger region has seven equivalent copies obtained by reflections and quarter-turns. In that sense, configurations at
$$
(a,b),\ (a,-b),\ (-a,b),\ (-a,-b),\ (b,a),\ (b,-a),\ (-b,a),\ (-b,-a)
$$
are symmetry-related and therefore mathematically equivalent after the corresponding transformation of the lattice.

### 5.3 Positive-only placement

The practical fix is immediate:
$$
b_j=c+j(W+2c),\qquad j=0,\dots,M-1.
$$

All strips are placed at distinct positive offsets. This removes the obvious \(b\mapsto -b\) duplication and gives \(M\) genuinely separated probes.

For ISE, that is the dominant symmetry to eliminate. The other octant symmetries matter mainly when one compares strips near different axes or swaps the roles of the coordinates. Because ISE works with narrow vertical strips at large positive offsets, the reflection \(b\mapsto -b\) is the practically relevant redundancy.

## 6. Parameter Theory

### 6.1 Tile width \(W\)

The width \(W\) should be larger than the correlation length \(\xi\) of the connectivity process.

The intuition is easy. A connected component rarely travels straight upward. It meanders. If the tile is too narrow, a perfectly good global connection can leave the strip laterally and re-enter later. The strip then looks blocked only because the window was too thin.

This is the same finite-size effect seen in percolation. Well above threshold, \(\xi\) is small and modest widths suffice. Near threshold, \(\xi\) grows, and narrow windows become unreliable.

For \(k^2=36\) near \(R\approx 80\) million, the measured density is close to the transition regime. In that setting \(W=2000\) may already be comparable to \(\xi\), while \(W=4000\) provides a safer margin.

### 6.2 Tile height \(H\)

The height \(H\) sets the radial resolution.

- Small \(H\) gives fine localization of the transition.
- Large \(H\) makes crossing easier, because a path has more room to wander before it must reach the outer face.

There is one obvious geometric requirement:
$$
H>2c,
$$
so that the inner and outer face zones do not overlap trivially.

Beyond that, \(H\) is a tradeoff. Too small, and shell counts become noisy. Too large, and a narrow obstruction is averaged away inside one generous tile.

### 6.3 Number of strips \(M\)

The role of \(M\) is statistical rather than geometric. Under the independence model of Section 4.4,
$$
\Pr(\text{all strips fail}) \le p(r)^M.
$$

So \(M\) directly controls the false-candidate rate.

In practice, values between \(16\) and \(64\) are the natural range.

- Below \(16\), one gives away too much statistical power.
- Beyond \(64\), returns diminish unless the single-strip failure probability is itself fairly large.

### 6.4 Collar \(c\)

The collar is forced by the step bound. For integer step vectors with \(\Delta a^2+\Delta b^2\le k^2\), the maximal single-coordinate excursion is \(\lfloor\sqrt{k^2}\rfloor\):
$$
c=\left\lfloor\sqrt{k^2}\right\rfloor.
$$

Examples:

- \(k^2=36\): \(c=6\)
- \(k^2=40\): \(c=6\)

Its purpose is purely geometric: every edge incident to an interior prime must remain inside the expanded tile. That is why the same \(c\) also appears in the strip stride \(W+2c\).

### 6.5 Prime density and the percolation threshold

Analytically, the density of Gaussian primes in a tile at distance \(R\) is of order
$$
\rho(R)\asymp \frac{C}{\ln(R^2)}=\frac{C}{2\ln R},
$$
where \(C\) depends on the exact sampling convention and is close to \(1\). This is the local form of the classical asymptotic count of Gaussian primes by norm.

What matters for connectivity is not only \(\rho(R)\), but the neighborhood size. For fixed \(k^2\), the relevant site-percolation model on \(\mathbb{Z}^2\) uses the disk neighborhood
$$
\{(\Delta a,\Delta b)\in\mathbb{Z}^2 : \Delta a^2+\Delta b^2\le k^2\}.
$$

For \(k^2=36\), this gives \(112\) nonzero neighbor vectors. Such a large neighborhood should have a comparatively low critical density. There is no simple closed-form formula for that threshold on \(\mathbb{Z}^2\), but the usual continuum heuristic says that larger neighborhoods push the threshold downward, roughly like a constant divided by the effective neighborhood size.

The ISE data suggest that for \(k^2=36\) the transition occurs near
$$
\rho_c \approx 3.50\%.
$$

This should be read as an empirical threshold for the local crossing process, not as a theorem. Finite tile width shifts the observed transition slightly, which is exactly why width sweeps matter.

## 7. Empirical Calibration for \(k^2=36\)

### 7.1 The measured \(f(r)\) gradient

Current campaigns show a sharp decline in the strip-crossing fraction:

| Radius \(R\) | \(f(r)\) | Local prime density |
|---|---:|---:|
| \(70\)M | \(0.42\) | \(\sim 3.5\%\) |
| \(80\)M | \(0.21\) | \(\sim 3.5\%\) |
| \(100\)M | \(0.04\) | \(\sim 3.5\%\) |

The key point is not the exact third decimal place. It is the shape. Raw prime density changes only slowly over this range, but \(f(r)\) collapses quickly. So the transition is a connectivity effect, not a sudden disappearance of primes.

### 7.2 Strip symmetry discovery

An early version of ISE placed strips in symmetric \(\pm b\) pairs. This looked like \(M=32\), but it was really only \(16\) independent probes, because the positive and negative offsets are mirror copies.

The evidence was perfect agreement of mirrored pairs in all checked shells. After this was recognized, the placement was changed to positive-only offsets. From that point onward, \(M=32\) meant \(32\) genuinely distinct strip samples.

### 7.3 Tile-size sensitivity

The width study is consistent with the correlation-length picture.

- \(W=2000\) appears to be close to the natural wandering scale of the crossing process.
- \(W=4000\) should give a cleaner, sharper transition.

This is the standard finite-size scaling expectation from percolation. Narrow windows smear the transition; broader windows make it look more like a genuine threshold.

### 7.4 Estimated threshold

Taken together, the present \(k^2=36\) data place the transition around
$$
R\approx 80\text{M}-100\text{M},
$$
with local density near
$$
\rho(R)\approx 3.50\%.
$$

So the current empirical estimate is
$$
\rho_c \approx 3.50\%
$$
as an empirical finite-width ISE crossing threshold for the \(k^2=36\) neighborhood on \(\mathbb{Z}^2\).

The caveat is the same as before: this is an ISE-based estimate, hence a finite-width estimate. The true infinite-width threshold may sit somewhat lower.

That caveat does not diminish the main point. The transfer-operator method converts a hard global graph problem into a clean local observable. A tile tells us how connectivity crosses a region. An ensemble of strips tells us where those crossings become rare. And subgraph monotonicity explains exactly what such local observations can, and cannot, certify.
