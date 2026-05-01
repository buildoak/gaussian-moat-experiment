---
title: "Combined Audit Report for tile_spec.md and grid_spec.md"
date: 2026-04-08
engine: codex
model: gpt-5.4
type: audit
status: complete
scope: tile_spec.md + grid_spec.md
---

FAIL

## Findings

### CRITICAL 1: `grid_spec.md` contradicts the TileOp layout and compositor inputs on `face_mask`, so the specified compositor cannot be implemented as written

**Problematic text**

From `docs/grid_spec.md`:

> "The grid_spec does NOT modify the TileOp format. It consumes TileOps exactly as tile_spec.md v4 defines them: 128 bytes, face-grouped SoA, with a separate face_mask array."

> "The compositor receives: `tile_ops[0..N)`: array of N TileOps (128 bytes each). `face_masks[0..N)`: array of N face_mask bytes."

> "### S10.2 Face Mask Array `face_masks[j * 32 + r]    // 1 byte each`"

From `docs/tile_spec.md`:

> "The face_mask is embedded inside the TileOp at byte 96, readable without parsing port data."

> "face_mask is embedded at byte 96 of each TileOp — no separate array needed."

**What is wrong**

These are mutually exclusive storage models. The grid spec says it consumes tile_spec v4 "exactly as defined" but then repeatedly requires a separate `face_masks` array that tile_spec v4 explicitly removed. This is not a wording nit: the compositor API, memory budget, and scan paths are different depending on whether `face_mask` is embedded or detached. As written, the two specs describe incompatible data plumbing.

**Suggested fix**

Pick one representation and delete the other everywhere. If tile_spec v4 is authoritative, update `grid_spec.md` so compositor inputs are only `tile_ops`, every `face_masks[...]` read becomes `tile_ops[t].bytes[96]` or an equivalent accessor, and the memory budget removes the separate 73 MB array.

### CRITICAL 2: Octant stitching is not specified rigorously enough to implement, and the fallback "single-octant" argument is explicitly unproven

**Problematic text**

From `docs/grid_spec.md`:

> "**Status: UNPROVEN.** This claim is plausible by symmetry ... **Recommendation: do not use Option B without a proof.**"

> "Detailed stitching geometry — which faces share which primes, the exact delta_h formula under reflection — is deferred to the implementation specification."

> "1. Identify the reflected tile: (j', r') such that `tile_x(j', r') = tile_y(j, r)` and `tile_y(j', r') = tile_x(j, r)`."

**What is wrong**

The spec rejects the proof-free single-octant shortcut, but the recommended replacement is still incomplete on the core mathematical object that makes it correct: the exact reflected-neighbor mapping and matching rule along `y = x`. Worse, the proposed reflected-tile equation generally has no solution in this tower grid, because `tile_x(j', r') = j' * S` must equal `base_y[j] + r*S`, which is usually not a multiple of `S`. So the report’s own "reflected tile" construction usually does not exist. This blocks any correct implementation of diagonal stitching and leaves the main correctness gap unresolved.

**Suggested fix**

Either provide a full seam specification with an explicit discrete mapping from each straddling tile/face segment to the mirrored tile/face segment plus an exact reflected `delta_h`, or abandon single-octant composition and specify a two-octant or full-sector composition that avoids diagonal stitching entirely.

### CRITICAL 3: The active-port UF memory estimate is off by a factor of about 4-5, so the feasibility claims in `grid_spec.md` are not supported

**Problematic text**

From `docs/grid_spec.md`:

> "tiles average ~7 active ports across all faces (tile_spec S4.5 validated data: 30-36 ports per tile before face distribution, ~7-9 per face)."

> "Active-only: ~513M entries * 4 bytes = 2.05 GB (at 7 ports/tile average)"

From `docs/tile_spec.md`:

> "Ports per tile 30-36" (after pruning table in S4.5)

**What is wrong**

The cited tile_spec data says roughly 30-36 surviving ports per tile, not 7. The parenthetical in `grid_spec.md` is internally inconsistent: "30-36 ports per tile" and "~7-9 per face" imply about 28-36 ports per tile, not 7 total. Using the tile_spec numbers, `73.4M * (30..36)` gives about `2.2B..2.6B` active ports, so a 4-byte parent array alone is about `8.8..10.6 GB`, before ranks, offsets, or other UF metadata. The stated `~2.3 GB` UF budget is therefore not justified, and the overall memory feasibility argument is materially wrong.

**Suggested fix**

Recompute all UF and total-memory numbers from the tile_spec S4.5 post-pruning counts. If the real budget is too high, the spec needs another compression strategy, chunked composition, or a different state representation; the current "active-only" argument is insufficient.

### HIGH 4: The naive `global_id` scheme overflows `u32`, but the spec repeatedly types IDs and parents as `u32`

**Problematic text**

From `docs/grid_spec.md`:

> "`global_id(t, f, s) = t * 64 + f * 16 + s`"

> "UF array size: 73.4M * 64 = 4.70B entries"

> "At 4 bytes per entry (u32 parent pointer): 18.8 GB."

> "`let mut leaders: [Option<u32>; 256] = [None; 256]`"

**What is wrong**

`4.70B` exceeds `u32::MAX` (`4,294,967,295`), so the spec’s own naive `global_id` space does not fit in the `u32` type used in pseudocode. Even though S9.8 later introduces active-only allocation, the document still presents the fixed-slot `global_id` formula, uses `u32` in the algorithms, and never states when IDs are widened or remapped. That is a concrete arithmetic inconsistency a systems programmer cannot safely guess around.

**Suggested fix**

State one canonical ID scheme. Either make all port IDs and UF parents `u64`, or define the active-only remapped ID space as the only valid `global_id` used by later phases and update all pseudocode and memory arithmetic accordingly.

### HIGH 5: The L/R matching arithmetic is inconsistent across the two specs; one version uses wrapping `u8` math where the grid spec requires signed arithmetic

**Problematic text**

From `docs/tile_spec.md`:

> "For staggered or multi-resolution grids, `delta_h != 0` (see S5.3)." 

> "if `a_h1[sa] == b_h1[sb].wrapping_add(delta_h)`"

From `docs/grid_spec.md`:

> "For the primary neighbor (`delta_h = -f`) ..."

> "The comparison ... must be computed in signed arithmetic (i16 or wider) to handle the negative offset."

**What is wrong**

For the primary L/R neighbor, `delta_h` is negative by construction (`-f`). The tile spec’s sample code uses `wrapping_add` on `u8` `h1` values, which is not the same predicate as signed equality unless every caller carefully converts the signed delta into a modular byte and also separately enforces overlap semantics. The grid spec later corrects this to signed `i16`, which means the two documents disagree on the arithmetic that decides whether two ports are the same physical port. That is a correctness-sensitive mismatch, not an optimization detail.

**Suggested fix**

Define the matching predicate once, in signed arithmetic, and remove the wrapping-`u8` example. A safe canonical form is `(a_h1 as i16) == (b_h1 as i16) + delta_h`, with `delta_h` explicitly typed as signed and derived per S5.

### HIGH 6: The recommended non-overlapping band scan admits a false-negative moat case in the unsafe direction

**Problematic text**

From `docs/grid_spec.md`:

> "**Edge case:** a moat that is narrower than W might straddle a band boundary. If the moat gap falls exactly at the seam between two bands, each band individually spans ..."

> "**Phase 1 approach (recommended):** use non-overlapping bands."

**What is wrong**

The document explicitly identifies a case where non-overlapping bands miss a real moat, then still recommends that method as the default. That is an unsafe false-negative for the stated search objective. "Phase 2 refinement" does not rescue the correctness of the published algorithm; it only notes a possible future fix.

**Suggested fix**

Make overlapping windows part of the base algorithm, not an optional refinement, or prove a minimum moat radial width that rules the seam case out. Without one of those, the current recommendation is not correct.

### HIGH 7: `grid_spec.md` still relies on symmetric TileOps with I/O `h1` in several places, which breaks the v4 asymmetric layout story

**Problematic text**

From `docs/grid_spec.md`:

> "The tile_spec v4 defines a face-grouped SoA layout: `Bytes  0-31: Face I (16 groups + 16 h1)` ..."

> "Under the current tile_spec layout, Face I and Face O h1 values are stored ..."

From `docs/tile_spec.md`:

> "I/O faces do not store h1 ..."

> "This asymmetry saves 32 bytes (no I/O h1) ..."

**What is wrong**

Section S14 of `grid_spec.md` describes the old symmetric layout as if it were tile_spec v4, and then uses those nonexistent I/O `h1` fields to justify octant-stitching options. That directly contradicts the current TileOp definition. It also means the answer to the requested cross-check is "no": the asymmetric `TileOp` layout does not flow cleanly through all compositor phases as currently documented, because the grid spec still assumes I/O `h1` exists in its layout analysis and stitching discussion.

**Suggested fix**

Rewrite S14 to match the actual v4 layout or delete it. Any stitching design must be expressed using only the fields that really exist in v4, or it must explicitly propose a new TileOp version instead of retroactively assuming one.

### MEDIUM 8: `face_mask == 0x00` is used inconsistently as "dead tile" versus "live tile with no surviving ports"

**Problematic text**

From `docs/tile_spec.md`:

> "`0x00`: dead tile — no ports at all, safe to skip entirely"

> "The CUDA kernel sets bit 6 whenever any port survives pruning, so `0x00` means no ports exist at all."

From `docs/grid_spec.md`:

> "A tile (j, r) is dead if its entire area lies below the line `y = x` ..."

> "Alive tile | A tile with `face_mask != 0x00` (has ports, is in the working octant)."

**What is wrong**

These definitions conflate geometry with connectivity. A tile can be geometrically in the working octant yet still have no surviving ports, in which case tile_spec says its `face_mask` is `0x00`. Grid spec then labels `face_mask != 0x00` as the definition of "alive tile," which is false under that case. The compositor can still safely skip zero-port tiles, but the terminology and predicates are inconsistent, and that matters because the document uses "dead" both for sub-diagonal tiles and for portless tiles.

**Suggested fix**

Separate the concepts explicitly: `geometrically_dead` for tiles excluded by `y <= x`, and `portless` for emitted all-zero TileOps regardless of geometry. Then state which checks use geometry and which use `face_mask`.

### MEDIUM 9: Cache-line and byte-range claims for L/R matching are wrong under the actual asymmetric layout

**Problematic text**

From `docs/grid_spec.md`:

> "Data accessed per match: 32 bytes from tile a (R-face: 16 groups + 16 h1) ... both from cache line 1 of the TileOp, bytes 64-127."

From `docs/tile_spec.md`:

> "Face R groups: bytes 48-63"

> "Face R h1: bytes 80-95"

**What is wrong**

Under the actual v4 layout, R-face groups live in cache line 0 and R-face `h1` lives in cache line 1; they are not a single 32-byte chunk in line 1. This does not change the mathematical correctness of matching, but it does make the compositor performance discussion and any low-level read plan inaccurate.

**Suggested fix**

Update the access description to reflect the real byte ranges: L/R matching touches group bytes in line 0 and `h1` bytes in line 1. Keep the performance claims aligned with the actual layout.

## What I Checked

- Read `docs/tile_spec.md` end to end, focusing on TileOp layout, matching semantics, pruning, overflow handling, and compositor pseudocode.
- Read `docs/grid_spec.md` end to end, focusing on tower geometry, delta arithmetic, dead-tile rules, octant stitching, compositor phases, and memory accounting.
- Cross-checked the specific items requested:
  - The asymmetric TileOp layout does not flow cleanly through all compositor paths as documented, because `grid_spec.md` still assumes symmetric I/O `h1` storage in S14 and in its stitching discussion.
  - Tower delta arithmetic is locally consistent between `grid_spec.md` S5 and the intended `tile_spec.md` L/R logic, but the published matching code disagrees on signed-vs-wrapping arithmetic.
  - The embedded `face_mask` at byte 96 does not flow through all compositor code paths as documented, because `grid_spec.md` still specifies a separate `face_masks` array and budgets memory for it.
  - The face mapping under reflection (`I <-> L`, `O <-> R`) is geometrically correct, but the actual octant-stitching algorithm is incomplete and the proposed reflected-tile lookup is generally not realizable on this tower grid.
