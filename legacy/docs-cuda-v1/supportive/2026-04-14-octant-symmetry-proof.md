---
date: 2026-04-14
type: proof
status: verified
refs:
  - docs/grid_spec.md
  - docs/supportive/2026-04-14-method-correctness-audit.md
  - docs/supportive/2026-04-14-method-correctness-audit-gemini.md
  - docs/supportive/2026-04-14-tsuchimura-sweep-results.md
  - docs/supportive/2026-04-11-octant-stitching-codex-hypothesis.md
---

# Octant Symmetry Proof for Gaussian Moat Detection

## Theorem (Octant Reduction Soundness)

Let K be a positive integer. Define the *step bound* as sqrt(K). A
**Gaussian prime walk of step length at most sqrt(K)** is a finite
sequence z_0, z_1, ..., z_n of Gaussian primes such that
|z_{i+1} - z_i| <= sqrt(K) for all i.

Define the **canonicalization map** C: Z[i] -> Z[i] by

    C(x + iy) = min(|x|, |y|) + i * max(|x|, |y|)

so that C maps any Gaussian integer to the closed first-octant wedge
{a + ib : 0 <= a <= b}.

**Claim.** If a Gaussian prime walk of step length at most sqrt(K)
connects two points A and B in Z[i], then there exists a Gaussian prime
walk of step length at most sqrt(K) connecting C(A) and C(B) that lies
entirely within the image of C (the first-octant wedge {a + ib : 0 <= a <= b}).

**Consequence for moat detection.** If no first-octant Gaussian prime walk
of step length at most sqrt(K) crosses an annular band
{z in Z[i] : R <= |z| <= R + W}, then no such walk crosses it in the full
plane either. Searching the first-octant wedge is sufficient.

---

## Definitions

**Gaussian integers.** Z[i] = {a + bi : a, b in Z}. We identify a + bi
with the lattice point (a, b) in Z^2. The **norm** of z = a + bi is
N(z) = a^2 + b^2.

**Gaussian primes.** A Gaussian integer z is a Gaussian prime if and only
if it is a prime element of Z[i] (up to units). The primality of z depends
only on N(z) and whether z lies on an axis. Crucially, z is a Gaussian
prime if and only if N(z) satisfies one of:
- N(z) = p where p is a rational prime with p = 1 (mod 4), or p = 2;
- z = (1+i)*unit or z lies on an axis and |z| is a rational prime
  with |z| = 3 (mod 4).

In all cases, Gaussian primality depends only on |a| and |b| (and their
arrangement on axes), not on their signs or ordering.

**Euclidean distance.** For z = (a, b) and w = (c, d) in Z^2, the
squared Euclidean distance is |z - w|^2 = (a - c)^2 + (b - d)^2.

**Annular band.** For real numbers R and W > 0, the annular band is
B(R, W) = {z in Z[i] : R^2 <= N(z) <= (R + W)^2}.

**First-octant wedge.** Oct_1 = {(a, b) in Z^2 : 0 <= a <= b}.

---

## The Canonicalization Map

Define C: Z^2 -> Oct_1 in two stages:

1. **Absolute-value fold.** F(a, b) = (|a|, |b|). This maps Z^2 to the
   closed first quadrant Q_1 = {(a, b) : a >= 0, b >= 0}.

2. **Diagonal sort.** S(a, b) = (min(a, b), max(a, b)). For (a, b) in Q_1,
   this maps to Oct_1.

Then C = S o F, i.e., C(a, b) = (min(|a|, |b|), max(|a|, |b|)).

**Remark on well-definedness.** C is defined on all of Z^2, and its image
lies in Oct_1. It is surjective onto Oct_1 (every point in Oct_1 is its
own canonical image). It is not injective: up to 8 points may map to a
single canonical representative.

---

## Lemma 1 (Norm Invariance)

**Statement.** For all z = (a, b) in Z^2:

    N(C(z)) = N(z).

**Proof.** N(z) = a^2 + b^2. Since |a|^2 = a^2 and |b|^2 = b^2, the
absolute-value fold preserves N. Since min(a,b)^2 + max(a,b)^2 = a^2 + b^2
for any a, b in R, the diagonal sort also preserves N. Therefore
N(C(z)) = min(|a|,|b|)^2 + max(|a|,|b|)^2 = a^2 + b^2 = N(z). QED.

---

## Lemma 2 (Primality Preservation)

**Statement.** If z is a Gaussian prime, then C(z) is a Gaussian prime.

**Proof.** By Lemma 1, N(C(z)) = N(z). Write z = (a, b) and
C(z) = (a', b') where a' = min(|a|, |b|) and b' = max(|a|, |b|).

**Case 1: z is off-axis** (both a != 0 and b != 0). Then a' >= 1, so
C(z) is also off-axis. An off-axis Gaussian integer w is prime iff N(w) is
a rational prime. Since N(C(z)) = N(z) and N(z) is a rational prime
(because z is an off-axis Gaussian prime), C(z) is also an off-axis
Gaussian prime.

**Case 2: z is on an axis** (a = 0 or b = 0, but not both). Then
|a| = 0 or |b| = 0, so min(|a|, |b|) = 0, meaning C(z) = (0, b') lies
on the b-axis. An on-axis Gaussian integer (0, m) is prime iff |m| is a
rational prime congruent to 3 (mod 4). Since b' = max(|a|, |b|) and
exactly one of |a|, |b| is zero, we have b' equals the nonzero one.
The original z had the same absolute value on its nonzero coordinate,
so b' = |a| or b' = |b| equals the same rational prime p = 3 (mod 4).
Hence C(z) = (0, p) is a Gaussian prime.

**Case 3: z = 0.** Zero is not prime, so this case is vacuous. QED.

---

## Lemma 3 (Axis Reflection Does Not Increase Distance)

**Statement.** For all a, c in Z:

    ||a| - |c|| <= |a - c|.

**Proof.** This is the reverse triangle inequality for the absolute value
function on R. Squaring both sides: (|a| - |c|)^2 <= (a - c)^2. Expand:
a^2 - 2|a||c| + c^2 <= a^2 - 2ac + c^2. This simplifies to
ac <= |a||c| = |ac|, which holds for all real numbers. QED.

---

## Lemma 4 (Diagonal Sort Does Not Increase Distance in Q_1)

**Statement.** For (a, b) and (c, d) in Q_1 (the first quadrant), let
(a', b') = S(a, b) = (min(a,b), max(a,b)) and
(c', d') = S(c, d) = (min(c,d), max(c,d)). Then:

    |(a', b') - (c', d')|^2 <= |(a, b) - (c, d)|^2.

**Proof.** We proceed by cases.

**Case A: No swap needed for either point.** If a <= b and c <= d, then
(a', b') = (a, b) and (c', d') = (c, d). The distance is unchanged.

**Case B: Both points swap.** If a >= b and c >= d, then (a', b') = (b, a)
and (c', d') = (d, c). Then |(b, a) - (d, c)|^2 = (b-d)^2 + (a-c)^2 =
(a-c)^2 + (b-d)^2 = |(a,b) - (c,d)|^2. The distance is unchanged.

**Case C: Exactly one point swaps.** Without loss of generality, suppose
a <= b (no swap) and c >= d (swap occurs), so (a', b') = (a, b) and
(c', d') = (d, c). We compute:

    |(a, b) - (c, d)|^2 = (a - c)^2 + (b - d)^2

    |(a, b) - (d, c)|^2 = (a - d)^2 + (b - c)^2

The difference is:

    D = |(a, b) - (d, c)|^2 - |(a, b) - (c, d)|^2
      = [(a - d)^2 + (b - c)^2] - [(a - c)^2 + (b - d)^2]
      = [a^2 - 2ad + d^2 + b^2 - 2bc + c^2]
        - [a^2 - 2ac + c^2 + b^2 - 2bd + d^2]
      = -2ad + 2ac - 2bc + 2bd
      = 2a(c - d) + 2b(d - c)
      = 2(a - b)(c - d).

Since a <= b, we have (a - b) <= 0. Since c >= d, we have (c - d) >= 0.
Therefore D = 2(a - b)(c - d) <= 0.

This gives |(a, b) - (d, c)|^2 <= |(a, b) - (c, d)|^2, i.e., the
distance after the diagonal sort is no larger than before.

**Boundary cases.** When a = b or c = d, D = 0, so the distance is
exactly preserved. QED.

---

## Lemma 5 (Canonicalization Does Not Increase Distance)

**Statement (Key Lemma).** For all z, w in Z^2:

    |C(z) - C(w)|^2 <= |z - w|^2.

Equivalently, |C(z) - C(w)| <= |z - w|.

**Proof.** Write z = (a, b) and w = (c, d).

**Step 1: Absolute-value fold.**
Let F(z) = (|a|, |b|) and F(w) = (|c|, |d|). By Lemma 3 applied
coordinate-wise:

    |F(z) - F(w)|^2 = (|a| - |c|)^2 + (|b| - |d|)^2
                    <= (a - c)^2 + (b - d)^2
                    = |z - w|^2.

**Step 2: Diagonal sort.**
Now F(z) and F(w) are in Q_1. By Lemma 4:

    |S(F(z)) - S(F(w))|^2 <= |F(z) - F(w)|^2.

**Combining:** |C(z) - C(w)|^2 = |S(F(z)) - S(F(w))|^2 <= |F(z) - F(w)|^2
<= |z - w|^2. QED.

---

## Lemma 6 (Canonicalization Maps Z[i] to Z[i])

**Statement.** C maps Z^2 to Z^2. Specifically, C(Z^2) = Oct_1 intersect Z^2.

**Proof.** If (a, b) in Z^2, then |a|, |b| are nonnegative integers, and
min(|a|, |b|), max(|a|, |b|) are nonnegative integers with the first no
larger than the second. So C(a, b) in Oct_1 intersect Z^2.

Conversely, every (a, b) in Oct_1 intersect Z^2 satisfies C(a, b) = (a, b),
so Oct_1 intersect Z^2 is contained in the image. QED.

---

## Main Theorem

**Statement.** Let P = (z_0, z_1, ..., z_n) be a Gaussian prime walk
in Z[i] with step length at most sqrt(K):

- Each z_i is a Gaussian prime.
- |z_{i+1} - z_i| <= sqrt(K) for all 0 <= i < n.

Then the sequence C(P) = (C(z_0), C(z_1), ..., C(z_n)) satisfies:

1. Each C(z_i) is a Gaussian prime in Oct_1.
2. |C(z_{i+1}) - C(z_i)| <= sqrt(K) for all 0 <= i < n.
3. The walk starts at C(z_0) = C(A) and ends at C(z_n) = C(B).

In particular, if there exists a Gaussian prime walk from A to B with step
bound sqrt(K) in the full plane, there exists one from C(A) to C(B) with
the same step bound that lies entirely in Oct_1.

**Proof.**

(1) By Lemma 2, each C(z_i) is a Gaussian prime. By Lemma 6, each
C(z_i) lies in Oct_1.

(2) By Lemma 5, |C(z_{i+1}) - C(z_i)| <= |z_{i+1} - z_i| <= sqrt(K).

(3) The endpoints are C(z_0) = C(A) and C(z_n) = C(B) by construction.

**Remark on multi-hop composition.** The proof applies C independently to
each vertex. This is valid because each step's distance bound is checked
independently (Lemma 5 applies pairwise, not to the path as a whole). The
canonical path may visit fewer distinct points if C collapses two original
primes to the same canonical image, but this can only shorten the path
(merge consecutive identical vertices), never break it. QED.

---

## Corollary 1: First-Octant Grid Sufficiency

**Statement.** To determine whether a Gaussian prime walk of step length
at most sqrt(K) crosses the annular band B(R, W), it suffices to check
whether any such walk crosses B(R, W) intersect Oct_1.

**Proof.** The annular band is defined by norm bounds: z in B(R, W) iff
R^2 <= N(z) <= (R + W)^2. By Lemma 1, N(C(z)) = N(z), so z in B(R, W)
if and only if C(z) in B(R, W).

Suppose a walk P = (z_0, ..., z_n) crosses B(R, W) in the full plane,
meaning z_0 lies inside the inner boundary (N(z_0) <= R^2) and z_n lies
outside the outer boundary (N(z_n) >= (R + W)^2). Then C(z_0) satisfies
N(C(z_0)) = N(z_0) <= R^2 and C(z_n) satisfies N(C(z_n)) = N(z_n) >=
(R + W)^2. By the Main Theorem, the canonical walk C(P) crosses B(R, W)
in Oct_1.

Contrapositive: if no walk crosses B(R, W) in Oct_1, then no walk
crosses B(R, W) in the full plane. The first-octant grid is sufficient. QED.

---

## Corollary 2: Sub-Diagonal Tiles Are Unnecessary for Soundness

**Statement.** The canonicalization argument shows that first-octant-only
coverage is sufficient for moat detection, independently of whether
sub-diagonal tiles are processed.

More precisely: The compositor need only process tiles whose lattice
domain intersects Oct_1 = {(a, b) : 0 <= a <= b}. Processing sub-diagonal
tiles (tiles entirely within {a > b > 0}) provides redundant coverage.
The `is_tile_dead()` predicate, which skips tiles whose domain lies
entirely below the diagonal, does not compromise soundness.

**Proof.** The Main Theorem shows that any full-plane walk maps to a walk
in Oct_1. The Oct_1 walk visits only points (a, b) with a <= b. Therefore,
to detect all walks that cross the annular band, it suffices to have
complete prime data and correct connectivity within Oct_1.

A tile is "dead" if its entire lattice domain satisfies a > b (i.e., it
lies strictly below the diagonal). No point in Oct_1 falls in such a
tile's domain. Therefore, removing dead tiles does not affect any vertex
or edge of any canonical walk.

**Important caveat.** Tiles that *straddle* the diagonal (containing
points on both sides of a = b) MUST be processed, because they contain
Oct_1 points. The `is_tile_dead()` predicate correctly preserves straddling
tiles: a tile (j, r) with domain [x_0, x_0 + S] x [y_0, y_0 + S] is
killed only if y_0 + S <= x_0, i.e., the tile's entire domain satisfies
b <= a. Any tile with at least one point where b >= a survives. QED.

---

## Corollary 3: Diagonal Margin of Zero Rows Suffices

**Statement.** The diagonal-sort distance inequality (Lemma 4) is strict
enough that zero extra rows of tiles past the y = x diagonal are needed
for soundness. The MARGIN parameter in the grid generation (currently
MARGIN = 2 * S = 512) provides redundant coverage.

**Proof.** The canonicalization argument works entirely within Oct_1. It
does not require any second-octant data. A "cross-diagonal" step in the
original walk (from z in Oct_1 to w outside Oct_1, or vice versa) maps
under C to a step that is no longer than the original and lands in Oct_1.
No tile in the second octant is needed to reconstruct the canonical walk.

Therefore, extending tower generation past the diagonal provides additional
(redundant) cross-diagonal connectivity, but the moat/spanning verdict is
sound even without it. The margin is a belt-and-suspenders safety measure,
not a mathematical necessity. QED.

---

## Edge Cases

### E1. Points on the diagonal (a = b)

If z = (a, a) for a >= 0, then C(z) = (a, a) = z. The map is the identity
on the diagonal. No special handling is required. The diagonal is part of
Oct_1.

If a < 0, then C(a, a) = (|a|, |a|), which is on the diagonal in Oct_1.

### E2. Points on an axis (a = 0 or b = 0)

If z = (0, b), then C(z) = (0, |b|). The map sends all axis points to the
nonneg b-axis, which is the boundary of Oct_1. This is handled correctly by
Lemma 2 (Case 2).

If z = (a, 0), then C(z) = (0, |a|). Note that the a-axis maps to the
b-axis under C. This is consistent with the 8-fold symmetry of Z[i].

### E3. Degenerate steps (canonical step shorter than original)

Lemma 5 shows |C(z) - C(w)| <= |z - w|. Strict inequality can occur,
e.g., z = (1, 3) and w = (-2, 5): |z - w|^2 = 9 + 4 = 13, but
C(z) = (1, 3) and C(w) = (2, 5), so |C(z) - C(w)|^2 = 1 + 4 = 5 < 13.

This is harmless. A shorter step is still a valid step (it remains within
the step bound). The concern would be if shorter steps somehow *created*
new invalid connections, but that is impossible: we are proving existence
of a canonical walk from C(A) to C(B), not uniqueness. The canonical walk
has the same number of hops (or fewer, if collisions occur) and each hop
is within the step bound.

### E4. Collision: C maps distinct primes to the same point

If z != w but C(z) = C(w), then the canonical walk visits the same
vertex twice in succession. Collapsing these consecutive duplicates
produces a shorter walk with strictly fewer hops that still connects
C(A) to C(B). This cannot break connectivity.

### E5. Can the canonical walk visit a non-prime?

No. By Lemma 2, C maps Gaussian primes to Gaussian primes. Every vertex
of the canonical walk is C(z_i) for some Gaussian prime z_i, hence is
itself a Gaussian prime.

### E6. Can the canonical walk have a step exceeding sqrt(K)?

No. By Lemma 5, each canonical step is at most as long as the
corresponding original step, which is at most sqrt(K).

### E7. Does canonicalization interact badly with the annular band?

No. By Corollary 1, the annular band is defined by norm bounds, and
C preserves norms. A walk that enters, stays in, or exits the annular
band in the full plane does exactly the same (possibly with shorter steps)
in the canonical version.

### E8. Does C preserve Z[i] lattice structure?

Yes. By Lemma 6, C maps integer lattice points to integer lattice points.
No irrational or non-integer coordinates are introduced.

---

## Scope and Limitations

This proof establishes that **first-octant coverage is sufficient for
the connectivity question**. It does NOT address other correctness
concerns in the pipeline:

1. **Face extraction correctness.** The collar depth, face-prime extraction,
   and TileOp encoding must be correct for the compositor to see all
   cross-tile edges. This is a separate concern (see the K_SQ=36 face
   extraction audit for known issues).

2. **I/O and L/R matching correctness.** The compositor's matching
   algorithms must correctly reconstruct cross-tile connectivity from
   TileOps. This is a separate concern from octant reduction.

3. **Annular band vs. global moat.** This proof shows that the first
   octant is sufficient *for the annular band question*. Whether a
   non-spanning annular band implies a global moat (no path from origin
   to infinity) requires additional argumentation about the annular band
   methodology, which is outside the scope of this document.

4. **Overflow/malformed tile handling.** Replacing overflow tiles with
   empties can sever connectivity and create false MOATs. This is an
   implementation concern orthogonal to the symmetry argument.

---

## Experimental Verification

### Diagonal-Fix Experiment

The grid generation was modified to extend towers past y = x (commit
cf2d0e6), adding sub-diagonal tiles that the compositor previously skipped
via `is_tile_dead()`. A sweep of 12 representative R values was run both
before and after the fix:

| R | Pre-fix verdict | Post-fix verdict | Changed? |
|---|-----------------|------------------|----------|
| 50,000,000 | SPANNING | SPANNING | No |
| 60,000,000 | SPANNING | SPANNING | No |
| 61,000,000 | MOAT | MOAT | No |
| 62,000,000 | SPANNING | SPANNING | No |
| 63,000,000 | MOAT | MOAT | No |
| 65,000,000 | MOAT | MOAT | No |
| 70,000,000 | MOAT | MOAT | No |
| 75,000,000 | MOAT | MOAT | No |
| 78,000,000 | MOAT | MOAT | No |
| 80,000,000 | MOAT | MOAT | No |
| 80,015,000 | MOAT | MOAT | No |
| 81,000,000 | MOAT | MOAT | No |

**Zero verdict changes across all 12 R values.** This is consistent with
the theoretical prediction: since C maps any full-plane walk to a walk in
Oct_1 with equal-or-shorter steps, cross-diagonal connectivity is redundant
for the spanning/moat verdict.

### Interpretation

The experimental result does not *prove* soundness (an experiment cannot
prove a universal statement), but it provides strong corroboration:

- The R values span the entire transition zone (50M--81M) where SPANNING
  and MOAT verdicts alternate.
- If cross-diagonal paths were needed for any spanning verdict, removing
  sub-diagonal tiles would convert at least one SPANNING to MOAT.
- No such conversion occurred, consistent with the theorem.

---

## Summary of Proof Structure

```
Lemma 1 (Norm invariance)
    |
    v
Lemma 2 (Primality preservation)          Lemma 3 (Axis reflection)
    |                                           |
    |                                           v
    |                                     Lemma 4 (Diagonal sort)
    |                                           |
    |                                           v
    |                                     Lemma 5 (Key: C doesn't
    |                                              increase distance)
    |                                           |
    v                                           v
Main Theorem (Canonical walk exists in Oct_1)
    |
    +---> Corollary 1 (Annular band sufficiency)
    +---> Corollary 2 (Sub-diagonal tiles unnecessary)
    +---> Corollary 3 (Zero diagonal margin suffices)
```

The proof is self-contained. No appeal is made to the specific tile
geometry, compositor matching logic, or implementation details. The
result depends only on the algebraic structure of Z[i] and the
Euclidean metric.
