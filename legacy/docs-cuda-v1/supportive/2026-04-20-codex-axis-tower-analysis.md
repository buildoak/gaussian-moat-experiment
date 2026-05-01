---
title: Axis Tower Reflection Closure Analysis
date: 2026-04-20
engine: codex
type: analysis
status: complete
refs: [methodology/lemmas_v2/tile-operator-definition-v-claude.md, methodology/lemmas_v2/campaign-blueprint.md, AGENTS.md]
---

## Question

Under the snapped grid with `S = 256`, and specifically considering an offset `(0, 0)` axis-adjacent tower, does the first tower need reflection-aware union-find edges for primes whose full-annulus neighbors lie across the reflection axis, or is octant-local UF sufficient?

## Short Answer

Octant-local UF is sufficient, provided K4 constructs the local graph over `V(G_full|_R)` only and includes all ordinary within-octant edges. Explicit reflection-neighbor edges in K4 or side-face stitching in the compositor are not required by the canonical proof.

## Source Findings

The canonical math document defines the active octant as:

```text
R := { (x, y) in Z^2 : x >= 0, y >= x,
       R_inner^2 <= x^2 + y^2 <= R_outer^2 }
```

So the checked-in canonical octant is `arg z in [pi/4, pi/2]`, not `[0, pi/4]`. The prompt's lower octant is the `sigma_diag` reflection of the canonical one. The analysis is unchanged by this convention swap, but "Y-axis-adjacent first tower" corresponds to the canonical upper octant, not to the prompt's lower octant.

Theorem 11 proves the tile operator verdict for `G_full|_R`, i.e. the Gaussian-prime graph restricted to the octant. Lemmas 6 and 7 provide tile and edge coverage for edges whose endpoints are in `R`. Lemma 10 ensures any geo-boundary prime component that can leave its host tile is represented by a face port, so flag-driven compositor UF is complete for the octant graph.

Theorem 12 then proves:

```text
I_R !~ O_R over G_full|_R
iff
I_A !~ O_A over G_full|_A
```

It does this by folding any full-annulus path into `R`. If a path step crosses from `R` into an adjacent reflected octant, the off-octant endpoint is reflected back into `R`. The Monotone Reflection Lemma proves the reflected endpoint is no farther from the previous in-octant endpoint, so the folded step is still a valid `<= sqrt(K)` graph edge.

This is exactly the mechanism that handles "connected via reflection" cases. For an edge `{u, v}` where `u in R` and `v` lies just across `x = 0`, Theorem 12 replaces `v` by `sigma_y(v) in R`; since `||u - sigma_y(v)|| <= ||u - v|| <= sqrt(K)`, the ordinary octant-local graph contains the edge `{u, sigma_y(v)}`. Likewise, a neighbor across `y = x` is replaced by `sigma_diag(v)`.

Therefore, K4 does not need to generate or union off-octant reflected-neighbor edges. It only needs to generate all normal edges among primes in the octant-restricted tile graph.

## Direct Answers

1. **Yes, Theorem 12 is compatible with octant-local UF only.** The proof explicitly says no runtime reflection code is needed; the blueprint repeats this in Section 9.

2. **The reflected-neighbor case is handled by path folding.** Any full-annulus edge that exits into an adjacent octant folds to an equal-or-shorter edge whose endpoint is the reflected prime inside `R`. That folded edge is an ordinary K4 edge in `G_full|_R`, covered by Lemma 7 and then by Theorem 11's port-graph equivalence.

3. **No concrete K4/compositor modification is needed.** If reflection-aware behavior were ever intentionally implemented, the mathematically consistent version would be to canonicalize off-octant candidates back into `R` before unioning, or to stitch side-exposed ports to reflected ports. That is not the blueprint path and would need careful duplicate/axis handling. The current spec says side-exposed faces are not stitched.

4. **The canonical octant is `[pi/4, pi/2]`.** The prompt's `{0 <= theta <= pi/4}` is the diagonal reflection of the canonical octant. Symmetric concerns at `y = x` are handled the same way as axis concerns, using `sigma_diag` in Theorem 12. The blueprint's `face_R` at `i = i_max` corresponds to the diagonal side exposure.

5. **The side-exposed-faces rule is related but not identical.** "Do not stitch side-exposed faces" is the compositor corollary of Theorem 12: full-annulus paths crossing side faces fold back into `R`. The K4 in-tile concern is lower-level: the local tile graph must be the octant-restricted graph and must contain all ordinary edges between primes in `R`.

6. **Boundary subtleties remain worth documenting.**
   - The prompt/AGENTS excerpt says offset `(0, 0)`, but the checked-in blueprint recommends `(1, 1)` to sidestep BACKLOG B1. Under `(0, 0)`, closed intervals can make column `-1` appear active via shared `x = 0` boundary points. B1 says this is harmless in practice but the active-tile definition needs tightening if `(0, 0)` is restored.
   - K4 should explicitly apply the octant predicate when enumerating halo primes. Including off-octant halo primes in local UF would no longer be computing `G_full|_R` as used by Theorem 11.
   - Primes exactly on `x = 0` or `y = x` are in `R`; they are fixed by one reflection and may have multiple valid fold choices. Theorem 12 says to pick deterministically, but this is proof-level only if no runtime folding is implemented.
   - Closed tile intervals are essential. Axis and diagonal boundary primes must not be lost through half-open ownership. Duplicate incidence across adjacent tiles is expected and resolved by strict positional port stitching on real shared faces.
   - Tower-closing near `y = x` is a separate structural issue. The math doc says reflection closure handles side-exposed `face_R`, while I4/corner closure handles port-graph diagonal-orphan concerns; these should not be conflated.
   - Tower top and bottom faces are inner/outer exposed through flag-driven geo tests, not through side-reflection logic.

## Conclusion

For the first axis-adjacent tower, do not add reflection-aware UF edges. The safe implementation contract is:

1. K4 builds local UF over primes in `R` only, including axis/diagonal boundary primes.
2. K4 unions every ordinary pair of octant primes at squared distance `<= K`.
3. The compositor stitches only actual shared tile faces inside the octant grid.
4. Side-exposed `face_L`/`face_R` are left unstitched, relying on Theorem 12 for full-annulus equivalence.

The main actionable documentation gap is the `(0,0)` vs `(1,1)` offset mismatch and the need to state explicitly that K4 halo enumeration is octant-clipped.
