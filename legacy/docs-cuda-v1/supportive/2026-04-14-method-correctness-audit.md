---
title: K_SQ=36 Method Correctness Audit
date: 2026-04-14
engine: codex
type: method-audit
status: complete
refs:
  - docs/tile_spec.md
  - docs/grid_spec.md
  - docs/tile_operations.md
  - docs/campaign_spec.md
  - docs/compositor_spec.md
  - tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp
  - tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/grid.cpp
  - tiles-maxxing/campaign-sqrt-36/tiles-compositor/include/grid.h
  - tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp
  - tiles-maxxing/campaign-sqrt-36/tile-cpp/src/face_extract.cpp
  - tiles-maxxing/campaign-sqrt-36/tile-cpp/src/encode.cpp
  - tiles-maxxing/campaign-sqrt-36/tile-cpp/include/constants.h
---

# Mathematical Method Correctness Audit — K_SQ=36

FAIL

## What I Checked

- Canonical specs: `docs/tile_spec.md`, `docs/grid_spec.md`, `docs/tile_operations.md`, `docs/campaign_spec.md`, `docs/compositor_spec.md`.
- K=36 implementation path:
  - `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp`
  - `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/grid.cpp`
  - `tiles-maxxing/campaign-sqrt-36/tiles-compositor/include/grid.h`
  - `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp`
  - `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/tileop_parse.cpp`
  - `tiles-maxxing/campaign-sqrt-36/tile-cpp/src/face_extract.cpp`
  - `tiles-maxxing/campaign-sqrt-36/tile-cpp/src/encode.cpp`
  - `tiles-maxxing/campaign-sqrt-36/tile-cpp/include/constants.h`
- Supporting context: `tiles-maxxing/campaign-sqrt-36/tiles-compositor/docs/supportive/2026-04-12-compositor-logic.md`, `docs/supportive/2026-04-13-k36-campaign-postmortem.md`.

## SOUND

### note — The wedge/octant reduction itself is mathematically salvageable

Severity: `note`

This is an inference from the geometry, not a proof stated in the repo.

Let

```text
C(x, y) = (min(|x|, |y|), max(|x|, |y|)).
```

Then:

- `x^2 + y^2` is preserved, so Gaussian primality is preserved under `C`.
- Axis reflections do not increase distance because `||a| - |b|| <= |a - b|`.
- Swapping one endpoint across the diagonal does not increase distance either:

```text
|(a, b) - (d, c)|^2 - |(a, b) - (c, d)|^2 = 2(a - b)(c - d) <= 0
```

for `a <= b` and `c >= d`.

So any prime path in the plane can be mapped pointwise to a path in the first octant with step lengths weakly decreased. That means multi-octant travel is not mathematically necessary for existence. This supports octant symmetry independently of the repo's current "sub-diagonal live tiles" prose.

Relevant code/spec anchors: `docs/grid_spec.md:99-105`, `docs/compositor_spec.md:49-54`.

### note — Variable-height towers do cover an annular band of radial thickness 8192 in the working wedge

Severity: `note`

The grid construction computes `tiles_per_tower[j] = ceil(32 / cos(theta_j))`, clamped to `[32, 46]`, specifically to keep radial coverage at least `32 * 256 = 8192` at every angle. That part is internally coherent in both spec and code.

Evidence:

- Spec rationale: `docs/grid_spec.md:71-84`, `docs/grid_spec.md:210-231`
- Implementation: `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/grid.cpp:27-45`, `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/grid.cpp:111-123`

### note — The L/R row mapping and h1-based stitching are mathematically correct

Severity: `note`

The code matches current-tower row `r` against previous-tower rows `r - q` and `r - q - 1`, with h1 predicates

- primary: `hl == hr + f`
- secondary: `hl + (S - f) == hr`

where `delta = q*S + f`.

That is the right specialization of the spec's `delta_h` formulas for left-facing matches against the previous tower.

Evidence:

- Geometry/spec: `docs/grid_spec.md:395-443`
- Implementation: `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp:399-491`

### note — Early `SPANNING` termination is sound once a valid spanning root is observed

Severity: `note`

Union-find only adds unions; it never deletes them. The compositor caches inner/outer reachability bits on UF roots, and once a root has both bits the fact cannot be undone by later ingestion.

Evidence:

- Spec: `docs/campaign_spec.md:671-733`
- Implementation: `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp:248-282`

Algorithmically, "SPANNING after 669 towers" means: after ingesting that prefix, a connected component already touched both the inner boundary and the currently exposed outer boundary of the processed region. Processing order does not threaten that monotonicity; later towers can only add more connections.

## UNSOUND

### critical — K_SQ=36 face extraction drops the exact depth-6 layer, so valid length-6 cross-tile edges are lost

Severity: `critical`

For `K_SQ = 36`, the maximum allowed Euclidean step is exactly `6`. A prime at perpendicular depth `6` from a shared face can connect to a prime on the shared boundary itself by a length-6 step. Those points must be exported to the TileOp, or the compositor cannot see the cross-tile edge.

The K=36 constants are:

- `K_SQ = 36`
- `COLLAR = ceil(sqrt(36)) = 6`

from `tiles-maxxing/campaign-sqrt-36/tile-cpp/include/constants.h:23-26`.

But face extraction uses strict inequalities:

- `FACE_I`: `tile_row < COLLAR`
- `FACE_L`: `tile_col < COLLAR`
- `FACE_O`: `tile_row >= TILE_SIDE - COLLAR + 1`
- `FACE_R`: `tile_col >= TILE_SIDE - COLLAR + 1`

from `tiles-maxxing/campaign-sqrt-36/tile-cpp/src/face_extract.cpp:88-102`.

At `COLLAR = 6`, this keeps only depths `0..5`. It excludes depth `6` on every face.

Concrete failure mode:

- Lower tile contains a Gaussian prime at tile row `250` on its O-side band.
- Upper adjacent tile contains a Gaussian prime on the shared boundary row `256`.
- These points can have the same along-face coordinate, so the step length is exactly `6`.
- The lower-tile prime is omitted from the O-face export because row `250` fails `tile_row >= 251`.
- The compositor never sees the cross-tile connection.

This is a direct false-`MOAT` mode. It is specific to perfect-square thresholds like `K_SQ=36`; the same code path happens to work for `K_SQ=40` only because `ceil(sqrt(40)) = 7` and the strict `< 7` still includes depths `0..6`.

The specs say the collar must include every prime within connection range of the face: `docs/tile_spec.md:57-61`. The implementation violates that statement for `K_SQ=36`.

### critical — Within-tower O/I stitching is not mathematically valid for collar-deep face bands

Severity: `critical`

Face extraction exports the entire collar-deep face band, not just the shared boundary row:

- I-face: rows `0..COLLAR-1`
- O-face: rows `S-COLLAR+1..S`

in `tiles-maxxing/campaign-sqrt-36/tile-cpp/src/face_extract.cpp:88-95`.

The compositor then matches O/I faces slotwise, truncated to `min(o_cnt, i_cnt)`:

```cpp
const uint8_t match_cnt = (o_slice.count < i_slice.count) ? o_slice.count : i_slice.count;
for (uint8_t s = 0; s < match_cnt; ++s) {
    unite(global_id(flat_top, go), global_id(flat_bottom, gi));
}
```

from `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp:371-379`.

That slotwise shortcut is only sound if the two port lists are guaranteed to be identical descriptions of the same physical shared-face connectivity. The code does not satisfy that premise, because the two tiles see different non-shared rows of the collar band.

Concrete counterexample:

- On the shared boundary row, there are primes at along-face positions `0` and `8`, so without extra help they are two separate ports.
- In the lower tile only, there is a prime at `(4, boundary-4)`.
- Distances are `sqrt(4^2 + 4^2) < 6` from that bridge prime to both boundary primes, so the lower O-face band merges them into one port.
- In the upper tile, if the symmetric `(4, boundary+4)` point is composite, the upper I-face band still has two ports.

Then `o_cnt = 1`, `i_cnt = 2`, and the compositor silently pairs only slot `0`. The second upper connection is dropped even though it should connect through the same lower-tile component.

This is a direct false-`MOAT` mode.

The repo's prose claims deterministic extraction makes slotwise O/I pairing valid (`docs/tile_spec.md:413-425`, `docs/tile_operations.md:573-576`), but that claim is not true for the implemented band-based face model.

### critical — A non-spanning band `[R, R+8192]` is not equivalent to “there is a moat at radius R”

Severity: `critical`

The mathematical question in the coordinator prompt is:

```text
is there no prime path from the origin to |z| > R with steps <= 6?
```

But the campaign spec defines the executable verdict on a single annular band:

`docs/campaign_spec.md:15-18`

and `docs/tile_spec.md` describes ring expansion separately:

`docs/tile_spec.md:625-640`

That distinction matters. A path can:

- leave the origin,
- reach radii greater than `R`,
- enter the band `[R, R+8192]`,
- fail to cross all the way to `R+8192`.

In that situation the single-band campaign returns `MOAT`, but there is no moat at radius `R` in the original global sense.

So the current campaign verdict is, at best:

```text
"this specific 8192-thick band does not span"
```

not

```text
"no 6-step prime path reaches beyond radius R"
```

unless an additional continuation theorem is proved. I did not find that proof in the repo.

### critical — The current K_SQ=36 campaign implementation can only create false `MOAT`s because overflow/malformed tiles are replaced with empties

Severity: `critical`

This is an implementation soundness failure, not a pure geometry argument, but it directly answers the prompt's "verdict soundness" question for the actual K=36 pipeline.

In campaign mode, overflow or malformed tiles are replaced by the empty-tile sentinel instead of being reprocessed into extended TileOps:

- tower-0 CPU path: `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp:610-621`
- CUDA burst path: `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/campaign.cpp:913-946`

Deleting a tile's boundary connectivity cannot manufacture a false `SPANNING`, but it can sever a real spanning path and create a false `MOAT`.

The K=36 post-mortem already records a 22% overflow rate during the attempted campaign:

- `docs/supportive/2026-04-13-k36-campaign-postmortem.md:84-121`
- reliability assessment: `docs/supportive/2026-04-13-k36-campaign-postmortem.md:163-171`

So, even setting the deeper method issues aside, the present K=36 pipeline does not produce trustworthy `MOAT` verdicts.

## OPEN

### warning — The repo's diagonal-coverage proof and the code disagree

Severity: `warning`

The current specs justify octant handling by saying that all generated sub-diagonal tiles are processed and supply cross-diagonal connectivity:

- `docs/campaign_spec.md:322-335`
- `docs/grid_spec.md:623-629`
- `docs/grid_spec.md:644-656`

But the K=36 compositor still has a dead-tile predicate:

- `tiles-maxxing/campaign-sqrt-36/tiles-compositor/include/grid.h:14-20`

and skips such tiles throughout matching and boundary collection:

- `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp:94-95`
- `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp:351-352`
- `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp:410-412`
- `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp:498-499`
- `tiles-maxxing/campaign-sqrt-36/tiles-compositor/src/compositor.cpp:596-597`

So the implementation is not using the proof strategy described in the specs.

My inference above shows that the octant reduction may still be sound via the canonicalization map `C(x, y) = (min(|x|, |y|), max(|x|, |y|))`. But that is not the argument the repo currently states. As written, the documentation does not justify why "one extra row past the diagonal" should be sufficient, because the documented proof route depends on processing arbitrarily many sub-diagonal tiles.

### warning — Coverage completeness is only proved for the first-octant wedge, not for the global moat question

Severity: `warning`

The live-tile region does cover the first-octant wedge cleanly: `is_tile_dead()` only kills tiles whose top edge is at least one full tile below the diagonal, so any tile intersecting `y >= x` survives:

- `tiles-maxxing/campaign-sqrt-36/tiles-compositor/include/grid.h:14-20`

That is enough for the wedge-reduction argument. It is not enough by itself to close the global "moat at radius R" claim, because that claim still needs the missing band-to-global continuation argument described above.

## Bottom Line

The K_SQ=36 pipeline is not mathematically correct as a decision procedure for

```text
"there exists a moat of width 6 at radius R"
```

in its current form.

The main blockers are:

1. The K=36 face extractor omits the exact depth-6 layer, so real cross-tile edges of length exactly `6` are dropped.
2. O/I stitching by slot index is not valid for the implemented collar-band face model.
3. A single non-spanning `8192`-wide band is weaker than a moat-at-`R` theorem.
4. The executable K=36 campaign currently replaces overflow/malformed tiles with empties, which can only bias results toward false `MOAT`.

The parts that are solid are narrower: the L/R row-shift geometry is right, the radial band tiling is coherent, and early `SPANNING` termination is monotone once a valid spanning root exists.
