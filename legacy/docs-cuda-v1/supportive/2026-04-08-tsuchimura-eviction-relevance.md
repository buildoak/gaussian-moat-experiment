---
title: "Tsuchimura Eviction Relevance Analysis"
date: 2026-04-08
engine: gemini
status: complete
---

# Analysis of Tsuchimura's Prime Eviction Optimization

**1. Does our TileOp architecture already subsume this?**
**Yes, at the prime level.** Tsuchimura's eviction keeps the working set of primes bounded. Our architecture completely subsumes this by pushing prime-level union-find into the CUDA kernel (`tile_spec.md` S7). The kernel identifies connected components within a tile, emits a fixed 128-byte TileOp (transfer operator), and immediately discards the raw prime data. The global compositor never sees or stores a raw Gaussian prime.

**2. Could UF entries be evicted during the sweep? What invariant must hold?**
**Yes, but it requires frontier re-rooting.** The compositor sweeps angularly, tower-by-tower (`compositor_spec.md` S6.1). Once tower `j` is matched with `j+1`, tower `j`'s TileOps are never accessed again. However, its Union-Find entries cannot simply be deleted because they may act as *parents* for groups in the active frontier (tower `j+1`) or boundary rings.
* **The Invariant:** No active frontier group or boundary group can point to an evicted node as its UF parent. 
* To evict safely, we would need to proactively re-root all crossing components to the active frontier before dropping the previous tower. This naturally leads to the "Transfer Reduction" architecture (`grid_spec.md` S15 Q3) rather than a global UF.

**3. Could we stream TileOps from CUDA and compose on-the-fly instead of materializing all 9.4 GB first?**
**Yes.** The L/R matching algorithm strictly requires a sliding window of only two adjacent towers (2 * 32 tiles * 128 bytes = 8 KB). If the CUDA kernel streams TileOps (or if we process in small sector batches), the compositor can consume them on-the-fly. We only materialize the 9.4 GB array because of the current batched kernel launch / Phase 0 prefix-sum design. Streaming would entirely eliminate this 9.4 GB memory footprint.

**4. Net verdict: relevant, partially relevant, or subsumed?**
**Partially Relevant (Subsumed for primes, Relevant as a streaming principle).**
The direct prime-level eviction is entirely subsumed by our TileOp abstraction. However, applying the *principle* of eviction to the compositor itself (evicting TileOps and UF entries behind the sliding window) is highly relevant. It highlights that the 9.4 GB TileOp materialization and global UF are unnecessary if we implement a streaming pipeline with transfer reduction.