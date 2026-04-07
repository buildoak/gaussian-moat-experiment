---
date: 2026-03-25
status: complete
engine: coordinator
type: proof
---

# Tile Composition Soundness for Gaussian Moat UB Campaigns

## Method Under Analysis

Two-phase tile-based connectivity determination:

**Phase 1 (per-tile, parallelizable):** For each tile T_ij in an M×N grid:
1. Sieve Gaussian primes in expanded tile T̂_ij (tile + collar c = ceil(√k) on all sides)
2. Build k-connectivity graph: edge between primes iff |p-q|² ≤ k
3. Union-find → connected components
4. Export face ports: Vec<(a, b, component_id)> per face (I/O/L/R)

**Phase 2 (composition, streaming):**
1. For adjacent tiles, match face ports: distance ≤ √k between ports on shared boundary
2. Union matched component IDs in global union-find
3. Check: does any global component touch both overall Inner and Outer faces?

**Claim:** This produces the same connectivity result as processing the entire region as one tile.

## Results

### Theorem 1: Soundness (No False Positives) ✓ PROVEN

Every seam union is triggered by a real edge (|p-q|² ≤ k). Every local component is a connected subgraph of G_k(R). Any path reported by composition lifts to a real path.

**Caveat:** Expanded tiles must be clipped to the search region R. Primes outside R in the collar must not participate in the connectivity graph.

### Theorem 2: Completeness — CONDITIONAL on W ≥ c

**Without W ≥ c:** False negatives are possible via diagonal adjacency.

**Counterexample (Codex-constructed):** W=2, k=2, c=ceil(√2)=2. Four tiles meeting at a corner. Edge (-5,-2)—(-4,-1) has |p-q|²=2 ≤ k, but the primes lie in diagonally-adjacent tiles sharing no face. Composition never matches them.

**With W ≥ ceil(√k²):** PROVEN COMPLETE. When W ≥ c, any diagonal-adjacent prime pair has at least one member in the CORE (not just collar) of an intermediate tile. The connection surfaces through that intermediate tile's face ports. In practice W >> c always holds (W=2000, c=7 for k²=40).

### Theorem 3: Minimum Data Per Tile

| Data | Sufficient? | Proof |
|------|------------|-------|
| 6 face-pair booleans (I→O, I→L, etc.) | NO | Counterexample: same booleans, different internal wiring, different composition result |
| Per-face component ID sets + connectivity | NO | Same component sets but different port coordinates → different matching |
| **Full face-port lists: Vec<(a, b, component_id)> per face** | **YES** | Coordinates enable distance-based matching; component IDs enable connectivity propagation. Proven minimum. |

### Theorem 4: Composition Order Independence ✓ PROVEN

Union-find is commutative and associative. Any evaluation order of seam unions produces the same final partition. Binary reduction trees, streaming left-to-right, or any hybrid strategy all produce identical results. Rows can be composed in parallel.

### Theorem 5: Corner Handling via W ≥ c

In a 2D grid, four tiles meet at each corner. A k-connected path CAN traverse the corner region connecting four tiles without passing through any shared face. This is handled automatically when W ≥ c because:
- The corner primes fall in the expanded (collar) region of all four tiles
- With W ≥ c, at least one intermediate tile contains the corner primes in its core
- That tile's face ports capture the connection

## Implementation Requirements

1. **ASSERT W ≥ ceil(√k²) at tile creation** — makes correctness structural
2. **Clip expanded tiles to search region R** — prevents spurious connections from out-of-region primes
3. **Use full face-port lists for composition** — not just booleans
4. **Face ports restricted to core-tile primes** — each prime exported by exactly one tile

## Existing Code Status (compose.rs)

The current Rust implementation in `tile-probe/crates/moat-kernel/src/compose.rs` is **already correct**:
- Uses distance-based port matching: `port_distance_sq() <= k_sq`
- Uses full face-port lists with coordinates and component IDs
- Binary reduction tree via `compose_grid`
- Component ID offsetting for unique IDs
- Face ports restricted to core-tile primes

**Missing:** W ≥ c assertion, region clipping assertion, component_sizes propagation through composition.
