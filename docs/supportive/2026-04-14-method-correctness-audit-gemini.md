---
title: "Mathematical Method Correctness Audit: Gaussian Moat K_SQ=36"
date: 2026-04-14
type: method-audit
engine: gemini
status: complete
---

# Mathematical Method Correctness Audit: Gaussian Moat K_SQ=36

`FAIL` on methodology. The compositor matching algorithms are mathematically incompatible with the `face_extract` domain logic, guaranteeing both false disconnections (false MOATs) and hallucinated paths (false SPANNING).

## SOUND

### 1. Coverage Completeness & Octant Symmetry (Q1, Q6)
**PASS:** The 1-row sub-diagonal margin is strictly mathematically sufficient. 

By Gaussian integer symmetry, the squared distance between any prime $z = (x_1, y_1)$ in the first octant ($x_1 \ge y_1$) and the mirror reflection $w' = (y_2, x_2)$ of a prime $w = (x_2, y_2)$ in the second octant ($x_2 < y_2$) satisfies:
$$D(z, w)^2 - D(z, w')^2 = 2(x_1 - y_1)(y_2 - x_2) \ge 0$$
Since primality is invariant under reflection ($N(w) = N(w')$), any valid step venturing into the second octant can be reflected into a valid step staying entirely within the first octant. Thus, 0 extra rows would technically be sufficient to prove connectivity. The `MARGIN = 2*S` bounding in `grid.cpp`, filtered to 1 live row by `is_tile_dead` in `grid.h`, provides redundant but perfectly safe cross-diagonal coverage.

- **Evidence:** `tiles-compositor/include/grid.h:14`
- **Severity:** Note

### 2. Collar Sufficiency (Q2)
**PASS:** The collar bound is unconditionally safe. 

`COLLAR = ceil(sqrt(36)) = 6` correctly bounds the maximum orthogonal component of any valid step. The 7-deep expanded domain ensures every cross-tile connection is internally visible to at least one tile's local Union-Find phase.

- **Evidence:** `tile-cpp/include/constants.h:18`
- **Severity:** Note

## UNSOUND

### 3. I/O Face Matching & Positional Shortcut (Q3, Q4, Q7, Q8)
**FAIL:** Positional matching on I/O faces causes exponential cross-wiring of completely unrelated physical components.

The spec asserts that I/O faces contain only the "same shared boundary row" of primes, justifying positional matching (`unite(o_groups[s], i_groups[s])`). However, `face_extract.cpp` extracts primes up to `COLLAR` depth. Because primes outside the *proper* domain (`tile_row < 0` or `tile_row > TILE_SIDE`) are skipped, Tile A extracts `Face O` primes in `[Y+250, Y+256]`, while Tile B extracts `Face I` primes in `[Y+256, Y+262]`. 

These sets are disjoint except at the boundary `Y+256`. Tile A will extract ports anchored at primes that Tile B does not see. This shifts all subsequent array indices. The compositor's positional `std::min(o_slice.count, i_slice.count)` loop blindly pairs `port[s]` from Tile A with `port[s]` from Tile B, physically teleporting paths across the annulus. This explains the absurd early termination anomaly where SPANNING was hallucinated at just 0.48% (669/138131) of the octant.

- **Evidence:** 
  - `tile-cpp/src/face_extract.cpp:81` (halo prime skip)
  - `tile-cpp/src/face_extract.cpp:89-94` (7-row face depth)
  - `tiles-compositor/src/compositor.cpp:302` (index-based matching)
- **Severity:** Critical

### 4. L/R Face Matching via Anchor Prime (Q3, Q4, Q7)
**FAIL:** `h1` coordinate matching silently drops valid cross-tile connections.

L/R matching unites ports if `A.h1 == B.h1 + delta_h`, where `h1` is the minimum-$h$ prime in the port. If a connection bridges the boundary, Tile A may group it with an earlier prime $P_1$ (since $P_1$ is exclusively in Tile A's proper domain), setting `h1` to $P_1$'s coordinate. Tile B, unable to extract $P_1$, groups the connection with a later prime $Q$, setting `h1` to $Q$'s coordinate. 

Because the `h1` anchors mismatch, the compositor's strict equality check evaluates to false and permanently discards the connection. `h1` matching is mathematically incompatible with domain-exclusive extraction.

- **Evidence:** `tiles-compositor/src/compositor.cpp:371` (`if (hl == hr + static_cast<uint16_t>(f))`)
- **Severity:** Critical

### 5. Annular Band Methodology (Q5)
**FAIL:** Sequential non-overlapping W-width bands can silently step over a wavy moat.

The pipeline evaluates non-overlapping W-width bands ($W=8192$) for spanning paths. If a moat gap oscillates radially by more than 8192 units, it can straddle the seam between `[R, R+W]` and `[R+W, R+2W]`. A path will successfully span band 1 (where the moat has swung outward) and span band 2 (where the moat has swung inward). The pipeline will falsely report SPANNING for both bands, completely missing the moat.

- **Evidence:** `docs/grid_spec.md` S12.3 (admits straddling flaw but defers sliding window to Phase 2)
- **Severity:** Warning

## OPEN
None. The logic defects are fully isolated.