---
title: "Octant Stitching Hypotheses"
date: 2026-04-11
engine: codex
type: analysis
status: complete
---

Let tile `(j,r)` have `x0 = jS`, `y0 = base_y[j] + rS`, `S = 256`, so its square is `[x0,x0+S] x [y0,y0+S]` under the shared-boundary convention.

## 1. Diagonal geometry

The diagonal `a=b` intersects tile `(j,r)` iff `[x0,x0+S] ∩ [y0,y0+S] != empty`.

Equivalently:

- fully above diagonal: `y0 >= x0 + S`
- straddling: `y0 < x0 + S` and `y0 + S > x0`
- dead/below: `y0 + S <= x0`

This matches `grid_spec` S7. Towers continue past the diagonal, so the last few towers contain straddling and dead tiles.

Reflection `sigma(x,y)=(y,x)` sends the square to the exact reflected square `[y0,y0+S] x [x0,x0+S]`.

Original square: `x in [x0,x0+S], y in [y0,y0+S]`
Reflected square: `x in [y0,y0+S], y in [x0,x0+S]`

Crucial point: this reflected square is usually not a tower tile, because tower x-coordinates are multiples of `S`, but `y0 = base_y[j] + rS` is generally not.

Face mapping under `sigma` is exact for the reflected square.

- `I <-> L`
- `O <-> R`
- `L <-> I`
- `R <-> O`

If a point has local coordinate `h` on a face, reflection preserves that `h`.

On `O`, the point `(x0+h, y0+S)` reflects to the reflected `R` face.

Its reflected local coordinate is still `h`.

So the axis swap preserves `h`; the hard part is not `h`, but the absence of a real reflected tile in the discrete tower grid.

## 2. Hypotheses

### A. Explicit mirror via virtual reflected seam objects

Compose the octant normally. For each straddling tile, build a virtual reflected square carrying mirrored ports, then union it back to real tiles by standard face matching.

Problem: current `TileOp` stores `h1` only for `L/R`, not for `I/O`.

Since `sigma` maps `O/I` to `R/L`, a post-hoc mirror built only from `TileOp` bytes cannot recover enough geometry.

This works only if seam data is emitted separately during K5, or if raw face primes are retained.

### B. Diagonal as a special tower boundary

Treat `a=b` as a fifth boundary.

For each straddling tile, extract diagonal-visible ports and match them to diagonal-visible ports of reflected neighbors by a dedicated predicate.

This is conceptually clean, but it is not reducible to existing `match_lr` on current `TileOp`.

The seam is not aligned with tile faces and again needs `O/I` geometric data not stored now.

### C. Full-annulus virtual unfolding

Duplicate the octant logically, apply `sigma` to all boundary tiles, then run a general neighbor search over real and virtual cells.

This is complete in principle, but it is the most machinery-heavy version of A.

It still needs virtual reflected face data unavailable from current `TileOp`.

### D. Two-octant or quadrant composition

Do not stitch at `a=b`.

Tile both adjacent octants explicitly, or the whole first quadrant, and use only ordinary `I/O/L/R` composition.

This loses some symmetry savings.

It is the simplest route to a proof of completeness with the current encoding.

## 3. Correctness

Any one-octant method that ignores cross-diagonal edges can miss paths that zigzag across `a=b` multiple times.

That is the catastrophic false-moat direction.

Diagonal primes (`a=a`) are fixed by `sigma`.

They must be represented once in the final UF.

A naive mirror can double-count them unless seam unions identify the point with itself.

Proof sketch for D: the union graph built from two explicit octants contains every prime vertex.

It also contains every step-`<=sqrt(k)` edge in that angular region.

Any full-path segment crossing `a=b` lies entirely inside this composed domain, so UF connectivity is exact.

No special deformation or symmetry argument is needed.

## 4. Recommendation

Most robust: D, explicit two-octant or quadrant composition.

It is straightforward.

It is provably complete.

If one-octant memory reduction is mandatory, the simplest correct seam is a modified A/B hybrid.

Emit an auxiliary diagonal-port record during K5 for straddling tiles, including enough `h1` to mirror `O/I` into `R/L`.

Then run a dedicated seam pass.

I would not recommend a seam implemented only from current `TileOp` bytes.

It is underspecified and likely incomplete.
