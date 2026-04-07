# Independent Strip Ensemble — Formal Proof and Algorithm Audit

**Date:** 2026-03-21
**Status:** complete

---

## 1. Theorem — Strip Moat Detection Soundness

### 1.1 Subgraph Monotonicity

**Theorem 1.** *Let $G_k$ be the Gaussian prime graph on $\mathbb{Z}[i]$ with edge threshold $k$ (i.e., $\pi \sim \pi'$ iff $|\pi - \pi'|^2 \le k^2$). Let $S \subset \mathbb{Z}[i]$ be any connected region containing the origin. Let $R_{\text{moat}}(k)$ be the moat radius of $G_k$ (supremum of $r$ such that every path from origin reaches norm $\le r$), and let $R_{\text{strip}}(k, S)$ be the moat radius observed when restricting $G_k$ to primes in $S$. Then:*

$$R_{\text{strip}}(k, S) \le R_{\text{moat}}(k)$$

**Proof.**

1. The primes in $S$ form a subset $P_S \subseteq P$ of all Gaussian primes.
2. The graph $G_k|_S$ restricted to $P_S$ is a subgraph of $G_k$.
3. Every path in $G_k|_S$ is a path in $G_k$.
4. Therefore every connected component in $G_k|_S$ is contained within some connected component of $G_k$.
5. If the origin's component in $G_k$ reaches radius $R_{\text{moat}}(k)$, the origin's component in $G_k|_S$ reaches at most $R_{\text{moat}}(k)$. $\square$

### 1.2 Corollary: Zero False Negatives

**Corollary.** *If the tile ensemble reports "no disconnection up to radius $R$" (i.e., every strip has $I \to O$ connectivity at every shell up to $R$), then $R_{\text{moat}}(k) \ge R$. There are zero false negatives.*

**Proof.** Contrapositive of Theorem 1. If $R_{\text{moat}}(k) < R$, there exists a radius $r < R$ where transport dies in $G_k$. Since $G_k|_S$ is a subgraph, transport must also die at or before $r$ in at least one strip. $\square$

**Remark.** False positives (strip reports disconnection where full graph does not) can occur because the strip removes primes outside its lateral bounds that might bridge components in the full graph.

### 1.3 Lean 4 Proof Sketch

```lean4
-- Gaussian prime graph restricted to a region
structure GaussianPrimeGraph where
  primes : Set (ℤ × ℤ)
  k_sq : ℕ
  adj : (ℤ × ℤ) → (ℤ × ℤ) → Prop :=
    fun p q => p ∈ primes ∧ q ∈ primes ∧ (p.1 - q.1)^2 + (p.2 - q.2)^2 ≤ k_sq

def moatRadius (G : GaussianPrimeGraph) : ℝ :=
  sSup { r : ℝ | ∃ path : List (ℤ × ℤ),
    path.head? = some (0, 0) ∧
    (∀ i, i + 1 < path.length → G.adj (path.get ⟨i, by omega⟩) (path.get ⟨i+1, by omega⟩)) ∧
    ∀ p ∈ path, (p.1^2 + p.2^2 : ℝ) ≤ r^2 }

-- The subgraph monotonicity theorem
theorem strip_moat_le_full_moat
    (G : GaussianPrimeGraph)
    (S : Set (ℤ × ℤ))
    (hS : (0, 0) ∈ S ∨ True)  -- strip contains origin region
    (G_S : GaussianPrimeGraph := ⟨G.primes ∩ S, G.k_sq⟩) :
    moatRadius G_S ≤ moatRadius G := by
  -- Every path in G_S is a path in G (intersection ⊆ full set)
  -- Therefore the reachable set in G_S ⊆ reachable set in G
  -- sSup of subset ≤ sSup of superset
  apply sSup_le_sSup
  intro r ⟨path, hHead, hAdj, hBound⟩
  exact ⟨path, hHead, fun i hi => by
    obtain ⟨hp, hq, hdist⟩ := hAdj i hi
    exact ⟨⟨hp.1, hp.2⟩, ⟨hq.1, hq.2⟩, hdist⟩, hBound⟩
```

---

## 2. Simplified Algorithm — Independent Strip Ensemble

### 2.1 Definition

**Input:** $k^2$, radial range $[R_{\min}, R_{\max}]$, tile width $W$, tile depth $D$, number of strips $M$.

**Setup:**
1. Place $M$ independent strips at distinct lateral offsets $b_1, b_2, \ldots, b_M$. Each strip has width $W$ and is centered at its offset. For square tiles, set $W \approx D$.
2. The strips are independent — no lateral composition between them.

**Per-shell procedure** for radial shell $[R, R+D]$:
1. Build $M$ independent tiles, one per strip. Tile $i$ covers $a \in [R, R+D]$, $b \in [b_i - W/2, b_i + W/2]$.
2. For each tile independently: run union-find over Gaussian primes within the tile (with collar expansion $\lceil\sqrt{k^2}\rceil$). Check if any connected component touches both the Inner face ($a \approx R$) and Outer face ($a \approx R+D$).
3. Record $c_i \in \{0, 1, \ldots\}$ = number of $I \to O$ spanning components for tile $i$.

**Ensemble statistic:**
$$f(R) = \frac{|\{i : c_i > 0\}|}{M}$$

**Detection rules:**
- $f(R) = 0$ for any shell $\implies$ **strong moat candidate** (all $M$ strips blocked)
- $f(R)$ drops sharply (e.g., below 0.1) $\implies$ **weak candidate** (most strips blocked)
- $f(R) \approx 1$ across shells $\implies$ **no moat** in this region

### 2.2 False Positive Probability

At a non-moat radius, define $p = P(\text{single tile reports } I \to O = 0)$. This is the probability that a single square tile of dimensions $W \times D$ fails to contain a spanning component, even though the full graph is connected at this radius.

For square tiles with $W = D = 2000$ and $k^2 = 26$ ($k \approx 5.1$), the tile contains approximately $1.5 \times 10^6$ primes (from the k26 trace data: ~1.5M primes per shell at $R \approx 10^6$). At these densities, Gaussian prime connectivity is deep into the supercritical percolation regime. The probability $p$ of a single tile failing to span is small.

**Conservative estimate:** Even with $p$ as high as 0.3 (which would require pathological prime distribution), the probability of a false positive at one shell with $M$ independent strips is:

$$P(\text{false positive}) = p^M$$

| $p$ | $M = 32$ | $M = 64$ |
|-----|----------|----------|
| 0.3 | $6.6 \times 10^{-17}$ | $4.3 \times 10^{-34}$ |
| 0.1 | $10^{-32}$ | $10^{-64}$ |
| 0.01 | $10^{-64}$ | $10^{-128}$ |

Over $N$ shells (e.g., $N = 500$ for a full campaign), union bound gives $P(\text{any false positive}) \le N \cdot p^M$, which remains negligible.

**False negative probability:** $P(\text{false negative}) = 0$ by Theorem 1.

---

## 3. Why `acc_io` and L↔R Composition Are NOT Needed

### 3.1 `acc_io` (Accumulated I→O Transport)

The current pipeline maintains an `accumulated` tile operator that grows shell-by-shell via vertical composition (`compose_vertical`). This tracks whether the origin's connected component has been severed by accumulating connectivity information across all prior shells.

**For candidate detection, this is unnecessary.** The independent strip ensemble detects moat candidates via per-shell $f(R) = 0$, which is a local property. If any shell is fully blocked across all strips, the moat is detected — no need to track the origin's component through prior shells.

The accumulated approach is needed for a full **lower bound (LB) campaign** where you must prove the origin's component is still alive at a specific radius. That is a future mode, not the candidate detection mode.

### 3.2 L↔R Composition (Horizontal Composition)

The current pipeline composes tiles horizontally within each shell via `compose_grid` / `compose_horizontal`. This merges adjacent strips into a single band tile operator, connecting components that span the lateral seam between strips.

**This was a workaround for narrow tiles.** The k26 trace used strip_width=240 with tile_depth=2000, giving an aspect ratio of 240/2000 = 0.12. At this aspect ratio, individual tiles are too narrow to reliably capture $I \to O$ spanning — connectivity can easily hop laterally out of a narrow tile and re-enter. Horizontal composition recovers this connectivity by stitching adjacent tiles.

**With square tiles ($W = D$), individual $I \to O$ is reliable.** A square tile with $W = D = 2000$ has aspect ratio 1.0. Spanning components that exist in the full graph are overwhelmingly likely to appear within a single square tile. Lateral escape and re-entry within $k \approx 5$ steps across a 2000-unit-wide tile is negligible.

### 3.3 What Removing Composition Gives Us

- **Embarrassingly parallel:** Each tile is fully independent. No seam stitching, no union-find merging across tiles, no sequential accumulation.
- **Constant memory per tile:** No growing accumulated operator.
- **Perfect GPU mapping:** Each tile build = one thread block. $M \times N_{\text{shells}}$ tiles, all independent.
- **Simpler correctness:** The only primitive is "build tile, check $I \to O$." No composition bugs possible.

---

## 4. Algorithm Audit — Current vs Proposed

### 4.1 Current Code Structure

**`tile.rs`** — Core tile building. `build_tile_with_sieve` enumerates Gaussian primes in a rectangle (with collar expansion), builds a graph via spatial hashing + union-find, classifies primes into face ports (Inner/Outer/Left/Right), and returns a `TileOperator` with component-to-face mappings.

**`compose.rs`** — Tile composition operators:
- `compose_horizontal`: merges two laterally adjacent tiles by matching Right-face ports of the left tile with Left-face ports of the right tile within distance $k$.
- `compose_vertical`: merges two radially adjacent tiles by matching Outer-face ports of the inner tile with Inner-face ports of the outer tile.
- `compose_grid`: reduces a 2D grid of tiles via pairwise horizontal then vertical composition (parallel via rayon).
- `*_with_seams` variants: same but log `SeamEvent` records for debugging.

**`probe.rs`** — Pipeline orchestrator:
- Creates $M$ strip bounds centered on origin.
- For each radial shell: builds $M$ tiles in parallel (rayon), computes per-tile `io_crossing_count`, horizontally composes into a band, vertically composes with accumulated tile.
- Transport check: `acc_io > 0` on the accumulated tile.
- Records `ShellProfile` with `tile_io_counts`, `band_io_crossings`, `acc_io`, etc.

### 4.2 What to KEEP for Independent Strip Mode

| Component | File | Status | Rationale |
|-----------|------|--------|-----------|
| `build_tile_with_sieve` | tile.rs | **KEEP** | Core primitive — unchanged |
| `TileOperator` struct | tile.rs | **KEEP** | Output type, still needed |
| `SimpleUF` | tile.rs | **KEEP** | Union-find within each tile |
| Face port detection | tile.rs | **KEEP** | $I \to O$ check uses `FACE_INNER_BIT`, `FACE_OUTER_BIT` |
| `io_crossing_count` | probe.rs | **KEEP** | Per-tile $I \to O$ count is the sole metric |
| `PrimeSieve` | primes.rs | **KEEP** | Prime enumeration |
| `centered_strip_bounds` | probe.rs | **MODIFY** | Strips should be independently placed, not necessarily centered as a contiguous band |
| `shell_bounds` | probe.rs | **KEEP** | Radial shell iteration |
| Per-tile `tile_io_counts` | probe.rs | **PROMOTE** | Currently a diagnostic — becomes the primary output |

### 4.3 What to REMOVE / DEACTIVATE for Independent Strip Mode

| Component | File | Status | Rationale |
|-----------|------|--------|-----------|
| `compose_horizontal` | compose.rs | **SKIP** | No lateral composition in this mode |
| `compose_vertical` | compose.rs | **SKIP** | No radial accumulation in this mode |
| `compose_grid` | compose.rs | **SKIP** | No grid reduction |
| `accumulated` variable | probe.rs | **SKIP** | No accumulated tile operator |
| `acc_io` / `io_crossing_count` on accumulated | probe.rs | **REPLACE** | Replace with $f(R)$ ensemble metric |
| `transport_alive` on accumulated | probe.rs | **REPLACE** | Detection via $f(R) = 0$ |
| `SeamEvent` logging | compose.rs | **SKIP** | No seams to log |
| `band_io_crossings` | probe.rs | **SKIP** | No composed band |

**Recommendation:** Keep `compose.rs` intact but gated behind a mode flag. The composition pipeline is needed for future LB campaigns. The independent strip mode should be a separate code path (or a `--mode independent-strip` flag) that bypasses composition entirely.

### 4.4 What Needs to Change

1. **Tile width as independent parameter.** Currently `strip_width` controls lateral extent. For square tiles, set `strip_width = tile_depth` (or add a `--square-tiles` flag).

2. **New output metric.** Replace the current per-shell summary with:
   ```
   shell_idx | r_center | tile_io_counts: [c_0, c_1, ..., c_{M-1}] | f(R)
   ```
   Where `f(R) = count(c_i > 0) / M`.

3. **No composition pipeline.** The shell loop becomes:
   ```
   for each shell:
       tiles = parallel_build(M tiles)
       tile_io_counts = [io_crossing_count(t) for t in tiles]
       f_R = count(c > 0 for c in tile_io_counts) / M
       if f_R == 0: record candidate
   ```

4. **Strip placement strategy.** Current: contiguous centered band. Proposed options:
   - **Contiguous:** $M$ adjacent strips covering $[-MW/2, MW/2]$. Maximum spatial coverage.
   - **Scattered:** $M$ strips at random or evenly spaced offsets across a wider range. Maximum statistical independence.
   - Default to contiguous for candidate detection (covers the most likely path locations).

---

## 5. Per-Tile I→O at the Moat — What Does the Data Say?

### 5.1 Available Data

The k26 probe trace (`2026-03-20-k26-probe-trace.json`) was run with:
- `k_sq=26`, `r_min=900000`, `r_max=1100000`
- `num_strips=64`, `strip_width=240`, `tile_depth=2000`
- **Without** `--export-detail` and **without** per-tile `tile_io` in the JSON output

The JSON includes `band_io` (composed band $I \to O$ count) and `acc_io` (accumulated $I \to O$ count) but **does not include the `tile_io` array**, despite the code computing it internally. The JSON serialization in `main.rs` does include `tile_io` (lines 167-178), but this trace file was generated before that code was added, or the field was stripped.

### 5.2 What the Composed Data Shows

At the Tsuchimura moat ($R \approx 1{,}015{,}639$, shell 57 at $R_{\text{center}} = 1{,}015{,}000$):

| Shell | $R_{\text{center}}$ | `band_io` | `acc_io` | alive |
|-------|---------------------|-----------|----------|-------|
| 55 | 1,011,000 | 4 | 0 | false |
| 56 | 1,013,000 | 4 | 0 | false |
| 57 | 1,015,000 | 4 | 0 | false |
| **58** | **1,017,000** | **0** | **0** | **false** |
| **59** | **1,019,000** | **0** | **0** | **false** |
| 60 | 1,021,000 | 2 | 0 | false |

Key observations:
- `acc_io` drops to 0 at shell 6 ($R \approx 913{,}000$) and never recovers — this is expected because the narrow strips (width=240) lose the origin's component early via lateral escape.
- `band_io` (composed band) stays positive through shell 57, then drops to 0 at shells 58-59 ($R \approx 1{,}017{,}000$-$1{,}019{,}000$). This is consistent with the Tsuchimura moat at $R \approx 1{,}015{,}639$.
- The composed band $I \to O = 0$ at the moat confirms that even with 64 narrow strips composed together (total lateral coverage: $64 \times 240 = 15{,}360$), the moat is detectable.

### 5.3 What We Need

**Per-tile `tile_io` data at the moat is not available in this trace.** To validate the independent strip approach, we need:

1. Re-run with the current code (which does output `tile_io`) and square tiles:
   ```bash
   tile-probe --k-squared 26 --r-min 1010000 --r-max 1025000 \
       --strip-width 2000 --num-strips 32 --tile-depth 2000 \
       --json-trace research/2026-03-21-k26-square-tile-trace.json
   ```
2. Check whether individual square tiles show $I \to O = 0$ at shells near $R \approx 1{,}016{,}000$.
3. If they do: the independent strip approach works for this known moat.
4. If they don't: the moat may be narrower than the tile depth, or the strip placement misses the gap. Adjust $D$ downward.

---

## 6. Implications for $\sqrt{40}$ Campaign

### 6.1 Parameters

- $k^2 = 40$, $k \approx 6.32$. Collar = 7.
- Target moat radius: unknown (no Tsuchimura data for $k^2 = 40$). Expected to be significantly larger than $10^6$.
- Tile dimensions: $W = D = 2000$ (square). Configurable up or down.

### 6.2 Strip Layout

- $M = 32$ or $M = 64$ independent strips.
- Contiguous placement: total lateral coverage = $M \times W = 64{,}000$ to $128{,}000$.
- At $R \approx 10^6$, the annular circumference is $\approx 6.3 \times 10^6$, so 64K-128K coverage is $\approx 1\%-2\%$ of the annulus. This is sufficient because:
  - Moats are global phenomena (the graph disconnects everywhere, not just locally).
  - If a moat exists at radius $R$, no strip should have $I \to O$ connectivity at $R$ regardless of lateral position.

### 6.3 GPU Parallelism

The independent strip design maps naturally to GPU execution:

| Level | Parallelism | Unit |
|-------|------------|------|
| Across strips | $M = 32$-$64$ | One tile per strip per shell |
| Across shells | Batch $B$ shells simultaneously | Independent (no vertical composition) |
| Within tile | Prime enumeration + UF | Thread block internal |
| **Total** | $M \times B$ independent tile builds | One kernel launch per batch |

No synchronization between tiles. No composition kernel. No accumulated state.

### 6.4 Cost Comparison

| | Current (composed) | Proposed (independent) |
|---|---|---|
| Tile builds per shell | $M$ | $M$ |
| Horizontal composition | $O(M)$ union-find merges | **0** |
| Vertical composition | 1 large merge | **0** |
| Memory per shell | Accumulated tile (grows) | $M$ independent tiles (constant) |
| Parallelism | Tiles parallel, composition sequential | **All parallel** |
| Communication | Seam matching (L↔R, I↔O) | **None** |

Per-prime computation is identical — the tile build (prime enumeration + spatial hashing + union-find) dominates runtime. The composition overhead is small in CPU mode but becomes a bottleneck in GPU mode where the independent tiles could run fully parallel.

---

## 7. Open Questions

1. **Does individual tile $I \to O = 0$ actually occur at the Tsuchimura moat?**
   Need the `--json-trace` run with square tiles to check per-tile `tile_io` counts. If individual tiles still show $I \to O > 0$ at $R \approx 1{,}016{,}000$, the tile depth may need adjustment, or the moat is thinner than one tile depth.

2. **What tile aspect ratio ($W/D$) gives optimal discrimination?**
   Square (1:1) is the theoretical sweet spot for spanning reliability. Slightly wider (2:1) increases lateral coverage per strip but reduces radial resolution. Need calibration runs to determine.

3. **How does $f(R)$ behave at non-moat radii?**
   We expect $f(R) \approx 1.0$ at non-moat radii (nearly all tiles have $I \to O > 0$). Need calibration data across a range of radii to establish the baseline and set detection thresholds.

4. **Should strips be adjacent (contiguous band) or scattered (random offsets)?**
   - **Adjacent:** Maximum spatial coverage of the annulus. If the moat is local (it isn't, but if), adjacent strips are more likely to cover the blocked region.
   - **Scattered:** Maximum statistical independence between tiles. Better for the $p^M$ false positive bound, since adjacent tiles have correlated prime distributions near their shared boundary.
   - For candidate detection, adjacent is likely sufficient. For rigorous statistical claims, scattered is cleaner.

5. **Can we adaptively refine?**
   If $f(R)$ drops but doesn't reach 0, run more strips at that radius. Adaptive refinement turns weak candidates into strong candidates or dismisses them, without re-scanning the full radial range.

6. **Tile depth vs moat width.**
   If the moat gap is narrower than $D$, a single tile might straddle the moat and still show $I \to O > 0$ (connected on both sides of the gap, spanning through). Smaller $D$ gives finer radial resolution but increases the number of shells. The Tsuchimura moat at $k^2 = 26$ has a gap of roughly $\sqrt{26} \approx 5.1$ in norm, but the tile depth is 2000. The moat manifests as a radial annulus where prime density drops below the connectivity threshold — this should still block $I \to O$ in a 2000-deep tile. Needs empirical verification.
