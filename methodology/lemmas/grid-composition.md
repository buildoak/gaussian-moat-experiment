# Grid Composition — Annular Scanning and Spanning Verdict

> How tiles are arranged and stitched to probe an annular ring for connectivity transfer.

This document describes the application of TileOp and stitch to the Gaussian moat problem. The operator and stitch operation are defined in [tile-operator-definition.md](tile-operator-definition.md); their soundness and completeness are proved in [tile-operator-completeness.md](tile-operator-completeness.md).

---

## Annular Scanning

The target: probe an annular ring at radius R in Z[i] for connectivity transfer from inner boundary to outer boundary. Eight-fold symmetry of the Gaussian primes reduces the search to a single working octant: {(x, y) : x ≥ 0, y ≥ x}.

**Towers** are vertical columns of tiles, indexed j = 0, 1, 2, ... Tower j has:
- Base x-coordinate: base_x = j · S
- Base y-coordinate: base_y = ⌊√(R² − (j·S)²)⌋, clamped for monotonicity (arc-following)
- Height: varies to guarantee uniform radial coverage W across all angles

Towers follow the arc of radius R. Each tower's base sits on the circle; tiles stack radially outward. Tower height increases from ~32 tiles near the y-axis to ~46 tiles near the diagonal (y = x), compensating for the changing angle between radial direction and tower axis.

---

## Stitching the Grid

The compositor ingests towers left-to-right. For each tower:

1. **Vertical stitching within the tower:** stitch(O_lower, I_upper) for each adjacent tile pair, bottom to top.
2. **Horizontal stitching with the previous tower:** stitch(R_prev, L_curr) for each tile row, matching across tower boundaries.
3. **Spanning check** after each tower ingestion.

---

## Spanning Verdict

After all towers are stitched, the global UF is checked: if any root appears in both the inner boundary set (Face I of the innermost tile row) and the outer boundary set (Face O of the outermost tile row) — connectivity transfers. No moat of width √K exists in this annulus.

The process terminates either at the first SPANNING detection (early exit) or after the last tower (MOAT verdict).

---

**Depends on:** [tile-operator-definition.md](tile-operator-definition.md), [tile-operator-completeness.md](tile-operator-completeness.md)
