# Vision: Next Phase — Moat Candidate Detection via Multi-Strip Probing

*Logged: 2026-03-20 | Source: Nikita's session vision + R. Jenkins grounding*

## Where We Are (grounding)

1. **UB campaign was measuring the wrong thing.** Ran annular probes (distance ~6) for several days hoping the sqrt(40) moat would be structural — i.e., that connectivity would fail uniformly across an annular ring. Got "survived" results that only prove: IF the component reaches distance D, THEN it passes through D's annulus. Does NOT prove origin-to-D connectivity.

2. **sqrt(36) at R=1B showed moat is locational, not structural.** The moat isn't a clean ring where density drops everywhere — it's a "first bad path" failure (GPT 5.4 Pro framing). Connectivity dies at a specific location, not uniformly. This invalidates the annular probe strategy.

3. **Algorithm reshape:** Cylindric coordinates + connectivity operator → pivoted to rectangular tiles with four-faced boundary operators (I/O/L/R) → hierarchical composition (tiles → supertiles → super-supertiles). User independently reinvented the Renormalization Group construction.

4. **Current state:** Built tile-probe Rust crate (1,260 LOC, 9/9 tests). Rectangular tiles in Cartesian coords, lateral (L↔R) and radial (I↔O) composition, multi-strip pipeline with profiling. Validated on k²=2,4,6 Tsuchimura moats.

## OG Ideas Scorecard

All 10 of the user's original seed ideas mapped into the framework. Nothing discarded — only refined. Biggest shift: "cheap function preserving connectivity" moved from coordinate-transform to graph-compression (EMST). Full mapping documented in `research/2026-03-20-og-ideas.md`.

## Vision: Next Several Days

### Core thesis

Running a group of ~100-1000 strips near the positive a-axis (the "Y axis"), with tiles inside them connected laterally (L↔R) as well as radially (I↔O), is a good **moat candidates detector**.

This is NOT a moat finder. It's Phase 1 of a two-phase campaign:
- **Phase 1:** Multi-strip probe generates candidates (cheap, fast, parallelizable)
- **Phase 2:** Full annular tiling verifies candidates at specific radii (exact, expensive, targeted)

### Validation plan

Challenge the multi-strip probe on known moats:
- **k²=26** — moat location known from Tsuchimura
- **k²=32** — moat at R ≈ 2,823,055
- **k²=36** — moat at R < 80,015,782

**Log everything.** The calibration runs produce empirical data on prime connectivity patterns — this data has value beyond moat detection.

### The Three Worlds of Connectivity Topology

The effectiveness of strip probing depends on which "world" we live in:

#### World 1: Tree-like branching
Connectivity spreads like a branching tree from (0,0) toward the moat. Some branches die, but any reasonably-sized area contains parts of the "Central Finite Curve" (the main connected component heading toward the moat).

**Implication:** Strips work great. Any reasonable-width corridor catches branches. False positive rate drops fast with strip count. Current tile-probe is optimized for this world.

#### World 2: Spiral river
Connectivity is a narrow river that winds in a spiral — aiming to evade strip probes. Goes around and around like a thin trickle until it finally depletes at the moat.

**Implication:** Strips are vulnerable. The trickle can exit the corridor sideways, travel through open space, and re-enter further out. Need either much wider bands or full annular ring closure. The hierarchical RG with ring closure is the only reliable tool.

#### World 3: Hybrid
Something between (1) and (2). Tree-like locally with occasional thin bridges.

**Implication:** Probably where reality lives. Requires adaptive strategy — strips for candidate generation, full tiling for verification at bottleneck radii.

### Theoretical prediction

Rudnick-Waxman says Gaussian prime angles are Poissonian at fine scales. A persistent spiral (World 2) would require angular correlation over millions of lattice units — contradicts the randomness result. If primes are truly angle-random, connectivity should branch like World 1. But "theory says" ≠ ground truth. The calibration runs will tell us empirically which world we're in.

### What the calibration gives us

1. **False positive rate** as f(num_strips, strip_width, k², R)
2. **Connectivity topology** — tree-like vs spiral vs hybrid
3. **Decay signature** — how does transport thin before the moat? Gradual or sudden?
4. **Probe strategy selection** — which world we're in determines the optimal approach for k²=40 at scale

## Pipeline Split (established)

| Domain | Engine | Status |
|--------|--------|--------|
| Prime generation in rectangles | CUDA (future) / Rust Miller-Rabin (current) | tile-probe built |
| Union-find + boundary extraction | Rust/CPU | tile-probe built |
| Shell composition | Rust/CPU | tile-probe built |
| Full annular ring closure | Not yet built | Phase 2 verification |
| Lyapunov extrapolation | Not yet built | Post-calibration |
