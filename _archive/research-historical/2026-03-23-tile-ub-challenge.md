---
date: 2026-03-23
engine: opus
type: adversarial-review
target: 2026-03-23-tile-ub-campaign-plan.md
cross_refs:
  - 2026-03-22-connectivity-transfer-operator.md
  - tile-probe/crates/moat-kernel/src/tile.rs
  - tile-probe/crates/moat-kernel/src/compose.rs
verdict: Accept with revisions
---

# Adversarial Challenge: Tile-Based Upper Bound Campaign Plan

## Summary

The document proposes a method for proving upper bounds on the Gaussian moat radius by decomposing an annular band into overlapping tiles, computing local connectivity, and checking global reachability in the resulting tile connectivity graph. The core mathematical argument is sound in its final form, though the document arrives at correctness through a somewhat tortuous process of self-correction that should be cleaned up. Several practical claims need revision, and there is one critical gap between the theoretical framework and the existing implementation.

---

## Issue 1: The compose.rs implementation does NOT implement the document's edge-detection scheme

**Severity:** Critical
**Location:** Section 1.8 (Practical Edge Detection via Face-Port Matching), Appendix B

**Issue:** The document describes a tile connectivity graph where edges are detected by *shared-prime matching*: find primes with identical $(a, b)$ coordinates in the overlap region and use their component assignments to create edges. This is the correct approach for the gapless tiling with $s_b = W$.

However, the existing implementation in `compose.rs` uses a fundamentally different scheme. The `compose_horizontal` function (lines 88-205) merges two tiles by checking *distance* between face ports:

```rust
for left_port in &left.face_right {
    for right_port in &right.face_left {
        if port_distance_sq(left_port, right_port) <= k_sq {
            uf.union(left_port.component, right_offset + right_port.component);
        }
    }
}
```

This is the ISE composition method: it connects R-face ports of the left tile to L-face ports of the right tile if they are within step distance $\sqrt{k^2}$. This is designed for the ISE stride $s_b = W + 2c$, where the face zones of adjacent tiles do NOT overlap. Two face-port primes from different tiles that are within distance $\sqrt{k^2}$ represent a genuine edge in $G_k$.

For the UB stride $s_b = W$, this scheme is *wrong* in both directions:

1. **It can create false connections.** With $s_b = W$, the R-face zone of $T_{i,j}$ and the L-face zone of $T_{i,j+1}$ overlap. The same prime $\pi$ appears as a face port in *both* tiles, but with *different component IDs* (because each tile runs an independent union-find). The distance check will find $d(\pi, \pi) = 0 \le k^2$ and merge the two component IDs. This is actually correct for $\pi$ itself -- but it also merges any two face ports $p_L \in T_{i,j}$ and $p_R \in T_{i,j+1}$ that happen to be within distance $\sqrt{k^2}$, *even if those two primes are in different components in reality*. The primes might be close in Euclidean space but not connected in $G_k$ within the overlap region.

    Wait -- actually, if $|p_L - p_R|^2 \le k^2$, then they are *by definition* connected by an edge in $G_k$. So the distance check is correct for creating edges. The issue is more subtle: with overlapping face zones, the same prime appears as a port in both tiles. The distance-based merge would union it with itself (harmless) and also union it with nearby primes from the other tile's face zone. But those nearby primes might *also* be in the overlap, so they would appear in both tiles too. The question is whether the resulting merged component structure correctly reflects the true $G_k$ connectivity.

    On closer analysis, the distance-based merge is *sound* for the UB direction: it can only create connections that actually exist in $G_k$ (because $|p-q|^2 \le k^2$ is the definition of an edge). It might *miss* connections that go through intermediate primes in the overlap but are not reflected in face-port-to-face-port distances. But missing connections is the conservative direction for a UB.

    Revising: the existing implementation is sound but potentially *less complete* than the shared-prime matching scheme. The shared-prime scheme captures all connections through the overlap, while the distance scheme only captures direct face-port-to-face-port edges. This means the distance scheme might declare "blocked" more readily than the shared-prime scheme, which would strengthen the UB (at the cost of potentially overstating the bound). For UB purposes this is fine but the discrepancy should be documented.

2. **Quadratic face-port comparison is expensive with overlap.** The inner loop `for left_port in &left.face_right { for right_port in &right.face_left { ... } }` is $O(|R| \times |L|)$. With ISE stride, face ports are sparse boundary primes. With UB stride and overlap, the R-face and L-face port lists are larger (they include primes from the $2c$-wide overlap band). At density $\sim 3\%$ and $c = 7$, the overlap contains $\sim 14 \times 2000 \times 0.03 \approx 840$ primes per face. The quadratic comparison is $840^2 \approx 700K$ distance checks per tile pair -- tolerable but worth noting.

**Suggested fix:** The document should explicitly state that the shared-prime matching scheme is a *new* edge detection algorithm required for the UB campaign, distinct from the existing distance-based composition in `compose.rs`. The document should note that the existing scheme is sound for UB purposes but implements a different (strictly weaker) edge detection. Implementation of the shared-prime matcher should be listed as a prerequisite for the UB campaign, or the document should prove that the distance-based composition is sufficient.

---

## Issue 2: Theorem 1.3 proof has a dangling path-prefix problem, acknowledged but not cleanly resolved

**Severity:** Major
**Location:** Section 1.4, Theorem 1.3 and its proof

**Issue:** The document proves Theorem 1.3, then immediately flags a gap: "the path PREFIX $p_0, \ldots, p_s$ may wander outside $T_{i,j}^+$." The document then argues that this is covered by the one-sided guarantee (Corollary 4.2 from CTO), noting that the tile graph is a conservative approximation.

This is correct in spirit, but the theorem statement itself is then misleading. Theorem 1.3 says "there exist components $C_L$ in $G_k[T_{i,j}^+]$ and $C_R$ in $G_k[T_{i,j+1}^+]$ and a prime $\pi_M$ in the overlap region such that $\pi_L \in C_L$ and $\pi_M \in C_L$." But this is not necessarily true as stated. The component of $\pi_L$ in $G_k[T_{i,j}^+]$ may not contain $\pi_M$, because $\pi_L$ might only connect to $\pi_M$ via a path that exits $T_{i,j}^+$.

What IS true is: $\pi_M$ and some primes near $\pi_L$ (specifically, the primes on the path segment from $\pi_L$ to $\pi_M$ that stay within $T_{i,j}^+$) are in the same component of $G_k[T_{i,j}^+]$, and those primes are in the R-face zone. But $\pi_L$ itself might be in a *different* component of $G_k[T_{i,j}^+]$.

The correct version of the theorem should state something like: "If a component $C$ in $G_k$ crosses from $T_{i,j}$ to $T_{i,j+1}$, then there exist a prime $\pi_M$ in the overlap, a component $C_L$ in $G_k[T_{i,j}^+]$ touching the R-face, and a component $C_R$ in $G_k[T_{i,j+1}^+]$ touching the L-face, such that $\pi_M \in C_L$ and $\pi_M \in C_R$." This does NOT require $\pi_L \in C_L$ -- the original endpoints might be in separate local components. The chain of edges in $\mathcal{G}_{\text{tiles}}$ still carries the crossing, but through potentially different components than the original $\pi_L$ and $\pi_R$.

The reachability theorem (1.6) ultimately handles this correctly by composing edges across multiple tiles. But Theorem 1.3 as stated is wrong.

**Suggested fix:** Restate Theorem 1.3 to claim only the existence of R-face and L-face touching components that share a prime in the overlap, without claiming those components contain the original endpoints $\pi_L$ and $\pi_R$. Then, in Theorem 1.6, argue that the full path induces a chain of edges in $\mathcal{G}_{\text{tiles}}$ through possibly different components at each tile, and this chain connects $\partial_I$ to $\partial_O$.

---

## Issue 3: Collar definition inconsistency between CTO document and campaign plan

**Severity:** Major
**Location:** Section 1.1, Appendix A

**Issue:** The CTO document (Section 3.2) defines $c = \lfloor \sqrt{k^2} \rfloor$. The campaign plan (Section 1.1) defines $c = \lceil \sqrt{k^2} \rceil$. The implementation (`tile.rs` line 125) uses `ceil`. Appendix A acknowledges this discrepancy but dismisses it as "the implementation's $c = 7$ is conservative."

For $k^2 = 40$: $\lfloor \sqrt{40} \rfloor = 6$, $\lceil \sqrt{40} \rceil = 7$. The maximum single-coordinate excursion in the step set is indeed 6 (from vectors like $(\pm 6, \pm 2)$). So $c = 6$ is *sufficient* -- every edge incident to an interior prime is visible with a collar of 6.

However, the document uses $c = 7$ throughout. This is conservative (strictly larger collar than necessary), so it does not break soundness. But it does have practical consequences:

1. The face zones are 1 unit wider than necessary, meaning more primes are tagged as face ports. This increases the size of the tile connectivity graph and the cost of face-port matching.
2. The overlap region is $2c = 14$ instead of $2 \times 6 = 12$, which is fine but inconsistent with the CTO document's analysis.
3. The expanded tile is $2014 \times 2014$ instead of $2012 \times 2012$, a negligible difference.

The real issue is that two foundational documents now disagree on the definition of $c$. Since $c$ appears in every theorem, this should be resolved consistently.

**Suggested fix:** Pick one definition and use it everywhere. The conservative $c = \lceil \sqrt{k^2} \rceil$ is safer. Update the CTO document to match, or explicitly define a "document collar" $c_{\text{doc}} = \lfloor \sqrt{k^2} \rfloor$ and an "implementation collar" $c_{\text{impl}} = \lceil \sqrt{k^2} \rceil$ and note that all results hold with either, since $c_{\text{impl}} \ge c_{\text{doc}}$.

---

## Issue 4: Single-shell UB does NOT prove a moat -- it proves a weaker radial barrier

**Severity:** Major
**Location:** Section 3.7, Phase UB-1 (Section 6.3)

**Issue:** The document correctly identifies (in Section 3.6) that a single-shell UB checks whether any component crosses from $a = R_{\min}$ to $a = R_{\max}$, and correctly notes this requires lateral connectivity analysis (not just per-tile io_count). Good.

But the claim "If blocked: $R_{\mathrm{moat}}(40) \le R_b + c$. UB established" in Phase UB-1 needs qualification. The single-shell UB proves that no component of $G_k$ restricted to the *first octant rectangular band* crosses from $a = R_{\min}$ to $a = R_{\max}$.

The moat problem, however, asks about paths in the *full plane*. A path from the origin to infinity can potentially exit the first octant, travel through another octant, and re-enter the first octant beyond $R_{\max}$. The octant-symmetry argument (Section 1.2) says "any prime path from the origin to infinity must cross this octant's annular band." But this is the *annular band* (the true annulus $R_{\min} \le |z| \le R_{\max}$), not the *rectangular band* $R_{\min} \le a \le R_{\max}$. The rectangular band in the first octant covers the region $R_{\min} \le a \le R_{\max}$, $0 \le b \le a$. A prime at position $(a, b)$ with $a^2 + b^2 > R_{\max}^2$ might have $a < R_{\max}$ (if $b$ is large enough), so it can be "beyond" the annulus in norm but "inside" the rectangle in $a$-coordinate.

More concretely: the rectangular band $R_{\min} \le a \le R_{\max}$ in the first octant ($0 \le b \le a$) covers norms from $R_{\min}$ (at $b = 0$) to $R_{\max}\sqrt{2}$ (at $b = a = R_{\max}$). Points at the diagonal edge ($b \approx a$) have norm $a\sqrt{2}$. So the rectangular band does NOT correspond cleanly to an annular band of fixed inner/outer radii.

The correct argument is: the rectangular band at $R_{\min} \le a \le R_{\max}$ blocks all paths that cross the $a = R_{\min}$ line to the $a = R_{\max}$ line in the first octant. By octant symmetry, the *same* rectangular band rotated to each of the 8 octants blocks paths in every octant. The intersection of these 8 barriers forms a complete radial barrier around the origin. Since any path to infinity must eventually have $|z| > R_{\min}\sqrt{2}$ (which requires $\max(|a|, |b|) > R_{\min}$, hence $a > R_{\min}$ in some octant), the barrier is complete.

This argument is correct but not in the document. The document uses the vague phrase "by the eightfold symmetry of $\mathbb{Z}[i]$, connectivity in the full plane is equivalent to connectivity in the first octant" without proving it rigorously for the rectangular-band setup.

**Suggested fix:** Add a lemma proving that a rectangular barrier in the first octant implies a full-plane moat. The key claim is: if no component crosses from $a = R_{\min}$ to $a = R_{\max}$ in the region $0 \le b \le a$, then (by the 8-fold symmetry) no component crosses this barrier in *any* octant, and therefore no component can have both $\max(|a|, |b|) < R_{\min}$ and $\max(|a|, |b|) > R_{\max}$. Conclude $R_{\mathrm{moat}} \le R_{\max}\sqrt{2}$ (the maximum norm at the boundary), not $R_{\min} + c$.

Actually, the bound $R_{\mathrm{moat}} \le R_{\min} + c$ is the *norm* bound only if the band is an *annular* band. For a *rectangular* band, the correct bound on moat radius (in terms of norm $|z|$) is $R_{\mathrm{moat}} \le R_{\max}\sqrt{2}$, which is weaker than claimed. The document should clarify which notion of "radius" is being used: $a$-coordinate or Euclidean norm.

---

## Issue 5: Tsuchimura memory estimate is wrong by orders of magnitude

**Severity:** Major
**Location:** Section 5.3

**Issue:** The document estimates:

$$N_{\text{primes}} \approx \frac{H \times R}{1} \times \frac{C}{2 \ln R} \approx \frac{2000 \times 10^9 \times 1.274}{2 \times 20.7} \approx 6.2 \times 10^{10}$$

Several problems:

1. The density formula $\rho = C / (2 \ln R)$ gives the *fraction* of lattice points that are prime. With area $= H \times R_{\max} = 2000 \times 10^9$, the number of lattice points is $2 \times 10^{12}$, and at density $\sim 1.274 / (2 \times 20.7) \approx 0.031$, the prime count is $\sim 6.2 \times 10^{10}$. This calculation appears correct.

2. But then "at 16 bytes per prime (coordinates + component ID), this requires $\sim 1$ TB of RAM" -- $6.2 \times 10^{10} \times 16 = 9.9 \times 10^{11}$ bytes $\approx 1$ TB. This is correct.

3. "Plus the adjacency structure (each prime has $\sim 128$ neighbors): $\sim 100$ TB." Each prime has $\sim 128$ neighbors *in the neighbor set*, but the actual number of *prime* neighbors is $128 \times \rho \approx 128 \times 0.031 \approx 4$. At 8 bytes per neighbor index, the adjacency cost is $6.2 \times 10^{10} \times 4 \times 8 \approx 2$ TB. The estimate of 100 TB is wrong -- it used $128$ (the full neighbor count) instead of $\sim 4$ (the actual number of *prime* neighbors). The correct adjacency cost is $\sim 2$ TB, not 100 TB.

4. Also, Tsuchimura would not store an explicit adjacency list. Union-find operates by scanning neighbors on-the-fly (exactly as the scanline kernel does). So the memory cost is dominated by the prime storage ($\sim 1$ TB) plus the union-find arrays ($\sim 500$ GB for parent + rank at 8+1 bytes per element). Total $\sim 1.5$ TB.

The conclusion is still "infeasible" for most machines, but the 100 TB figure is inflated by a factor of $\sim 50$.

**Suggested fix:** Correct the adjacency estimate. State that Tsuchimura requires $\sim 1.5$ TB for primes + UF data, with no explicit adjacency storage needed. This is infeasible on typical cloud instances (128 GB RAM) but within reach of high-memory machines (e.g., AWS x1.32xlarge with 2 TB RAM). The tile-based method's advantage is real but not as extreme as 100 TB vs 2 GB.

---

## Issue 6: Cost estimate arithmetic is internally inconsistent

**Severity:** Minor
**Location:** Section 4.4, Phase UB-1 cost

**Issue:** The document claims $\$8.60$ for the single-shell UB (Section 4.4) and then in Section 6.4 lists Phase UB-1 at $\$8.60$. But the abstract says "$9 for single-shell UB." These are approximately consistent but the abstract should match the body.

More importantly, the timing estimate deserves scrutiny. At 1.8s per tile with 64 cores:

$$\frac{550{,}000 \times 1.8}{64} = 15{,}469 \text{ seconds} \approx 4.3 \text{ hours}$$

This assumes 100% utilization (perfect load balancing, no overhead). With the document's own estimate of 70% effective parallelism (Section 6.6, item 4), the actual time is:

$$\frac{4.3}{0.7} = 6.1 \text{ hours}$$

At $\$2$/hr: $\$12.20$, not $\$8.60$. The document should use its own load-imbalance factor consistently.

**Suggested fix:** Apply the 70% efficiency factor to the cost estimates, or remove the 70% claim.

---

## Issue 7: The face-port data structure stores primes, not just booleans -- confirmed sufficient

**Severity:** Nit (resolved positively)
**Location:** Section 1.8, tile.rs

**Issue:** The challenge protocol asked whether `face_ports` stores enough information for inter-tile matching. Examining `tile.rs`:

```rust
pub struct FacePort {
    pub a: i64,
    pub b: i64,
    pub component: usize,
}
```

Each face port records the exact coordinates $(a, b)$ and the component ID. This is exactly what the shared-prime matching scheme (Section 1.8) requires: match on $(a, b)$, link component IDs. The data structure is sufficient.

However, as noted in Issue 1, the existing *composition* code (`compose.rs`) does not use shared-prime matching. It uses distance-based merging. The data structure supports the proposed algorithm, but the algorithm is not yet implemented.

**Suggested fix:** None needed for the data structure. Implementation of shared-prime matching is covered by Issue 1.

---

## Issue 8: Can a path skip a tile entirely?

**Severity:** Minor
**Location:** Section 1.4, Theorem 1.6 proof

**Issue:** With $s_b = W$ (gapless tiling), the interiors partition the band. Every lattice point in the band is in exactly one tile's interior. A path in $G_k$ that traverses the band must pass through lattice points in consecutive tiles' interiors (or through the same tile). It cannot "skip" from tile $T_{i,j}$ to $T_{i,j+2}$ without visiting $T_{i,j+1}$'s interior, because the interiors are contiguous and each step is at most $c < W$ in the lateral direction.

More precisely: if a prime $\pi_1$ is in $T_{i,j}$'s interior (so $b(\pi_1) \le b_0 + jW + W$) and $\pi_2$ is in $T_{i,j+2}$'s interior (so $b(\pi_2) \ge b_0 + (j+2)W$), then $|b(\pi_2) - b(\pi_1)| \ge W = 2000 > c = 7$. So they cannot be connected by a single step. Any path between them must pass through a prime with $b$-coordinate in $[b_0 + (j+1)W, b_0 + (j+1)W + W]$, which is in $T_{i,j+1}$'s interior.

This confirms that tile-skipping is impossible with gapless tiling, and the proof of Theorem 1.6 is correct on this point.

**Suggested fix:** The document's proof relies on this implicitly. Making it explicit (one sentence: "since $W > c$, each step moves by at most one tile laterally") would strengthen the argument.

---

## Issue 9: Super-tile aggregation can create false connections -- correctly identified but underanalyzed

**Severity:** Minor
**Location:** Section 3.3, Proposition 3.2

**Issue:** Proposition 3.2 claims the super-tile face-port structure is "a sound coarsening of the base tile graph." The proof says "composition of sound approximations is sound." This is true for the UB direction: the super-tile connectivity can only be a *superset* of the true connectivity, never a subset. So if the super-tile graph says "blocked," the base-tile graph (and hence $G_k$) is also blocked.

But wait -- is this actually true? Let me trace it carefully. The base-tile graph $\mathcal{G}_{\text{tiles}}$ is a conservative *underapproximation* of $G_k$ (it misses connections). The super-tile aggregation takes $\mathcal{G}_{\text{tiles}}$ and runs reachability within a $K \times K$ block. This produces face-port summaries that are an *exact* computation on $\mathcal{G}_{\text{tiles}}$ within the block. The super-tile graph is then built by matching super-tile face ports.

If two base-tile components are in the same super-tile reachability class, they get merged into one super-tile component. This can only *increase* connectivity (merge components that are reachable from each other within the block). At the super-tile level, matching face ports between super-tiles uses the coarsened components, which may merge base-tile components from different positions. This merging can create connections between super-tile boundary components that share a face but are not actually connected through the block interior -- wait, no, the reachability computation is exact within the block. If two boundary components are in the same reachability class, there IS a path in $\mathcal{G}_{\text{tiles}}$ within the block.

So the super-tile graph is an exact computation on $\mathcal{G}_{\text{tiles}}$ at a coarser resolution. It loses nothing compared to checking $\mathcal{G}_{\text{tiles}}$ directly. The only loss is already in $\mathcal{G}_{\text{tiles}}$ itself (compared to $G_k$).

This means the document's Proposition 3.2 is actually stronger than stated: the super-tile aggregation preserves the UB property *exactly* (no additional loss), not just soundly.

**Suggested fix:** Strengthen Proposition 3.2 to state that super-tile aggregation is lossless relative to the base-tile graph. The hierarchy does not weaken the UB.

---

## Issue 10: First-octant boundary conditions are non-trivial

**Severity:** Minor
**Location:** Section 6.6, item 5

**Issue:** The document correctly identifies boundary effects at $b = 0$ and $b = a$ (the octant boundaries) and dismisses them as "they can only make blocking harder, not easier." This is the right intuition: the octant boundary acts as a reflecting wall, and reflections can only create *more* connections (primes near $b = 0$ are connected to their reflections $b \to -b$, which in the full graph creates additional paths).

However, the tile-based UB works in the first octant only. At $b = 0$, the leftmost tile's L-face has no neighboring tile. The tile graph simply has no left neighbor for $j = 0$. But in the full $G_k$, primes near $b = 0$ can connect to primes at $b < 0$ (in the adjacent octant), which by symmetry is the same as connecting to primes at $|b|$. This means the full graph has connections across $b = 0$ that the first-octant tile graph misses.

For the UB direction, this is conservative: the tile graph misses connections, making blocking easier to achieve. The UB remains valid. But the document should make this explicit: "The first-octant restriction can only *undercount* connections, which is conservative for the UB."

Similarly, at the diagonal $b = a$, the tile near the diagonal edge has no neighbor in the $b > a$ direction. In the full graph, the quarter-turn symmetry $z \to iz$ maps $(a, b)$ to $(-b, a)$, which for $b \approx a$ connects the tile to its rotated image. Again, the tile graph misses these connections, which is conservative.

**Suggested fix:** Add a brief argument (2-3 sentences) making the conservativity of the octant restriction explicit.

---

## Issue 11: "Gapless tiling costs 0.7% more tiles" -- verified correct

**Severity:** Nit (confirmed)
**Location:** Section 2.2

**Issue:** With ISE stride $s_b = W + 2c = 2014$, tile density is $1/2014$ per unit. With UB stride $s_b = W = 2000$, tile density is $1/2000$. The ratio is $2014/2000 = 1.007$, or 0.7% more tiles. Each tile has the same expanded size ($W + 2c = 2014$ wide). The overlap of 14 columns means those primes are sieved and union-found in two tiles, but the extra work is $14/2014 \approx 0.7\%$ of each tile's work. Both claims are correct.

**Suggested fix:** None needed.

---

## Issue 12: The "streaming aggregation" memory analysis in Section 3.5 is confused

**Severity:** Minor
**Location:** Section 3.5

**Issue:** The document first estimates "10,000 tiles, each with ~20 MB of working data: 200 GB -- too large." Then it proposes processing the $K$ radial shells sequentially, reducing to "100 tiles simultaneously: 2 GB."

But in Section 3.7, the document pivots to the single-shell UB approach, which requires only $N_r = 1$ shell of $N_\ell = 550{,}000$ tiles. The streaming aggregation of Section 3.5 is for the multi-shell super-tile approach, which is effectively abandoned in favor of the simpler single-shell approach. The super-tile machinery (Part 3) is thus largely unused.

This is not an error, but it is confusing. The document develops an elaborate hierarchical scheme (Part 3), then realizes in Section 3.7 that the single-shell approach is sufficient for the UB. Part 3 ends up being theoretical infrastructure that is never deployed.

**Suggested fix:** Either (a) restructure the document to present the single-shell UB first and relegate super-tile aggregation to an appendix for future multi-shell campaigns, or (b) make clear at the start of Part 3 that this machinery is developed for the multi-shell case (Phase UB-2) and is not needed for the initial single-shell campaign.

---

## Issue 13: The document self-corrects in real time, leaving false claims in the text

**Severity:** Minor
**Location:** Multiple (Theorem 1.3 proof, Section 3.6, Corollary 1.9)

**Issue:** The document has several "Wait --" moments where the author catches an error mid-argument:

- After the proof of Theorem 1.3: "Wait -- the argument above needs a refinement."
- In Section 3.6: "Wait, that's the wrong question."
- In Corollary 1.9: "But wait -- we defined edges via shared primes, not mere face adjacency."

This is intellectually honest and shows careful self-review, but it leaves the document in a state where the reader encounters wrong claims before seeing them corrected. For a research document that may be cited or re-read, the false starts should be removed and the correct arguments presented cleanly.

**Suggested fix:** Revise the document to remove the false starts and present the corrected arguments directly. The "Wait" observations can be preserved as remarks ("One might initially think X, but in fact Y because Z") if they are pedagogically valuable.

---

## Issue 14: Collar computation uses floating point

**Severity:** Nit
**Location:** tile.rs line 125

**Issue:** The implementation computes `collar = (k_sq as f64).sqrt().ceil() as i64`. For large $k^2$, this could have floating-point precision issues. For example, if $k^2 = 2^{53} + 1$ (a pathologically large value), the `f64` representation would lose precision. However, for all practical values ($k^2 \le 100$), this is perfectly safe. The maximum practical $k^2$ discussed is 40, where $\sqrt{40}$ is representable to many digits of precision in `f64`.

**Suggested fix:** None needed for the current campaign. For future-proofing, consider an integer square root function.

---

## Overall Assessment

### Strengths

1. The core mathematical argument (Theorems 1.6 and 1.8) is correct: the tile connectivity graph is a conservative approximation, and "blocked in $\mathcal{G}_{\text{tiles}}$" implies "blocked in $G_k$."
2. The analysis of ISE stride vs. UB stride (Section 1.4) is thorough and correctly identifies why $s_b = W$ is necessary.
3. The cost estimates are roughly in the right ballpark and show the computation is feasible.
4. The face-port data structure in the implementation stores sufficient information for the proposed algorithm.
5. The document honestly identifies its own gaps and corrects them.

### Weaknesses

1. The existing composition code (`compose.rs`) implements distance-based merging, not the shared-prime matching described in the document. This is a critical implementation gap.
2. Several theorems are stated incorrectly before being corrected inline, making the document hard to read as a reference.
3. The relationship between the rectangular band and the true annular moat needs a tighter argument.
4. The Tsuchimura memory comparison is inflated by ~50x.
5. Cost estimates do not consistently apply the document's own 70% efficiency factor.

---

## Verdict: Accept with revisions

The mathematical framework is sound. The key insight -- that gapless tiling with $s_b = W$ makes inter-tile connectivity sound for a UB argument -- is correct and well-argued (after self-correction). The practical cost estimates are feasible. The main required revisions are:

1. **Critical:** Reconcile the document's shared-prime matching algorithm with the existing distance-based composition code, or prove the existing code is sufficient.
2. **Major:** Clean up the self-correcting proofs into final-form statements.
3. **Major:** Add a rigorous octant-to-full-plane lemma for the rectangular band.
4. **Major:** Correct the Tsuchimura memory estimate.
5. **Minor:** Apply the 70% efficiency factor consistently to cost estimates.
