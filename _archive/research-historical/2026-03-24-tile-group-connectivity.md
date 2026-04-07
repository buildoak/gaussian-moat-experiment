---
date: 2026-03-24
status: planning
engine: coordinator
---

# Tile Group Connectivity: Intermediate Step Between ISE and UB Campaign

## Core Insight

Current ISE measures connectivity per individual tile (2000×2000). The full UB campaign uses millions of tiles stitched together. There's a valuable middle ground: **small groups of tiles** (low thousands) with cross-tile union-find.

## The Idea

1. **Optimal tile sizing**: We've already proved ISE is sensitive to tile size. Instead of fighting this, exploit it — find the most CUDA/memory-efficient tile size for each step size k.

2. **Group connectivity**: Stitch 4 adjacent stripes together into a 2D tile group. Each tile processes independently (sieve + graph + UF), then boundary primes connect across tile edges. The group acts as an analogue of a single larger tile — but cheaper, because:
   - Each small tile is independently sieved (fits in memory)
   - Only boundary primes need cross-tile matching
   - The 2D group captures cross-face connectivity patterns that single tiles miss

   **Cross-face connectivity patterns** — the key measurement:
   - Element enters left face → exits top
   - Element enters bottom → exits left
   - These diagonal/turning paths are invisible to single-tile left-right traversability
   - They carry structural information about how the graph routes around obstacles (moat fragments)

   **Analogue of bigger tiles, but cheaper**: A 4-stripe group of 2000×2000 tiles gives the connectivity information of a ~4000×4000 region, but each tile sieve is 4× less memory than a single 4000×4000 tile (sieve cost scales superlinearly with area due to norm range).

3. **Why this works better than single-tile ISE**:
   - Single tile: connectivity is binary (path crosses or doesn't) — one bit per tile
   - Tile group: connectivity is structural — component sizes, merge patterns, boundary statistics across the group give a much richer signal
   - A moat that spans 3 tiles but not 1 would be invisible to single-tile ISE but visible to group connectivity

4. **Why this is much cheaper than full UB**:
   - Full UB campaign: millions of tiles, full annular coverage, enormous CUDA optimization needed
   - Tile groups: low thousands of tiles, arranged in radial strips, runs on Jetson with current code
   - Orders of magnitude less compute for a qualitatively different measurement

5. **This is a stepping stone to UB**: The cross-tile union-find logic is needed anyway for the full UB campaign. Building it now as a lightweight "group ISE" mode gives us:
   - The infrastructure for tile stitching (reusable for UB)
   - A new operator for moat detection (immediate value)
   - A test bed for optimizing tile sizes and memory patterns

## Tile Size Optimization

The tile size that reliably detects zero-connectivity at known moats could be calibrated:
- Run small groups at k²=26 (moat at ~1M) with varying tile sizes → find smallest tile that shows zero group-connectivity
- Repeat for k²=32, k²=36
- The optimal tile size likely scales with k (step radius) — this would give us a principled tile size for k²=40

## Relationship to Multi-Metric ISE

This is complementary to per-tile graph metrics (susceptibility χ, largest component S, C1/C2 ratio, etc.). Per-tile metrics extract more from individual tiles. Group connectivity extracts signal from tile-to-tile interactions. Both can be calibrated on known moats independently.

## Implementation Path

1. Per-tile boundary prime extraction (which primes are within √k of tile edges)
2. Cross-tile edge computation (which boundary primes from adjacent tiles connect)
3. Group union-find (merge component IDs across tiles)
4. Group-level metrics: does the group span? How many cross-tile merges? Component distribution across the group?

## Scale Estimates

For a 4-stripe tile group at R=1.1B, tile size 2000×2000:
- Group: 4 adjacent stripes → 2D arrangement, ~4000×4000 effective coverage
- Each tile sieve: 4× cheaper than a single 4000×4000 tile (norm range scales superlinearly)
- Multiple groups at different radial angles → sparse but representative coverage
- Each tile: ~15K primes, ~8 min on Jetson currently (but most time is sieving, not graph)
- With pre-sieved data, graph-only pass would be seconds per tile
- Total: feasible in hours, not days
