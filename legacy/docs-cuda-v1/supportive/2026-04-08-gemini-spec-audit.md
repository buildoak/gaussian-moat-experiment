---
title: "Spec Audit: TileOp and Grid Architecture (v4/v1)"
date: 2026-04-08
engine: gemini
model: gemini-3.1-pro-preview
type: audit
status: complete
scope: tile_spec.md + grid_spec.md
---

# Spec Audit: TileOp and Grid Architecture (v4/v1)

This report details the findings from an adversarial audit of `tile_spec.md` (v4) and `grid_spec.md` (v1), focusing on correctness, consistency, completeness, rigor, and implementability. 

### 1. Asymmetric Layout Breaks Octant Stitching
**Quote:** `grid_spec.md` S8.3: *"Face O (top, fixed y) <-> Face R (right, fixed x)... Match ports using h1 equality with the computed delta_h (tile_spec S5.1)."*
**Quote:** `tile_spec.md` S4.1: *"I/O faces do not store h1 because the tower grid architecture guarantees delta_h = 0 for all I/O shared faces..."*

**Problem:** Octant stitching (Option A) requires matching the O-face (top) of a tile near the diagonal with the R-face (right) of its reflected counterpart. This cross-axis mapping mandates offset-based `h1` matching. However, the v4 asymmetric `TileOp` layout explicitly removed `h1` values from I/O faces to save 32 bytes. Without `O.h1` stored in the `TileOp`, the compositor has no geometric data to perform the required matching across the diagonal seam.

**Severity:** CRITICAL (blocks implementation or affects correctness)

**Suggested Fix:** Either revert to a symmetric `TileOp` layout (or one that includes at least `O.h1`), or perform octant stitching using raw lattice primes directly inside the CUDA kernel before the `TileOp` is emitted, bypassing the need for compositor-level O-face geometric matching.

### 2. Incomplete Spanning Check on Ragged Boundaries
**Quote:** `grid_spec.md` S9.7: *"Inner boundary: row 0, I-face... Check against all outer boundary ports... let t_outer = tile_index(j', 31)... if tile_ops[t_outer].face(O).groups[s']"*

**Problem:** Due to the downward curvature of the inner arc (`delta[j] >= 0`), adjacent towers are vertically offset. This exposes portions of the L-faces on row 0 (inner boundary) and portions of the R-faces on row 31 (outer boundary) to the outside of the annulus. A connected path of primes could bypass the I/O faces entirely, entering through an exposed L-face step on row 0 or exiting through an exposed R-face step on row 31. By only checking I-faces and O-faces for spanning, the compositor will miss these paths, reporting false moats.

**Severity:** CRITICAL (blocks implementation or affects correctness)

**Suggested Fix:** Expand the spanning check (Phase 4) to include any L-face port on row 0 that falls within its exposed `d` units (where `d = delta[j-1]`), and any R-face port on row 31 that falls within its exposed `d` units (where `d = delta[j]`). These must be added to the `inner_roots` hash set and checked on the outer boundary.

### 3. Grid Spec Uses Impossible Layout and Separate Face Masks Array
**Quote:** `grid_spec.md` S14.1: *"The tile_spec v4 defines a face-grouped SoA layout: Bytes 0-31: Face I (16 groups + 16 h1)... Bytes 96-127: Face R (16 groups + 16 h1)"*
**Quote:** `grid_spec.md` S10.2: *"face_masks[j * 32 + r] // 1 byte each... Same tower-major ordering as tile_ops. Total: N bytes ~ 73 MB"*

**Problem:** `grid_spec.md` completely missed the v4 update in `tile_spec.md`. It mistakenly describes a 128-byte symmetric layout (which mathematically leaves zero bytes for metadata) and relies on a separate 73 MB `face_masks` array. `tile_spec.md` v4 explicitly uses an asymmetric layout (bytes 0-63 groups, 64-95 L/R h1) to embed `face_mask` at byte 96 and `port_counts` at 97-100 to save memory. 

**Severity:** HIGH (should fix before impl)

**Suggested Fix:** Update `grid_spec.md` to reflect the true groups-first asymmetric layout from `tile_spec.md` v4. Remove the separate `face_masks` array from the grid spec memory budget entirely, and update all `face_masks[a]` array accesses in the compositor pseudocode to read the embedded `tile_ops[a].bytes[96]`.

### 4. Useless Face-Pair Pruning Bits in Embedded Metadata
**Quote:** `tile_spec.md` S4.3: *"face-pair pruning (primary runtime benefit): bits 0-5 let the compositor skip matching loops for disconnected face pairs. ... skip matching on I x R port pairs."*

**Problem:** `tile_spec.md` dedicates 6 bits of its precious embedded metadata to pairwise face connectivity (IO, IL, IR, etc.). However, the actual composition algorithm (Phase 1, `tile_spec.md` S6.1 and `grid_spec.md` S9.3) uses an O(P) Union-Find approach that loops over all active ports by group ID. There are no O(N^2) pairwise face matching loops in the compositor, making these 6 bits entirely useless for the defined algorithm.

**Severity:** MEDIUM (documentation gap)

**Suggested Fix:** Either remove the face-pair pruning bits from the `face_mask` definition (freeing them for future use), or explicitly document how they apply to the proposed "Transfer Reduction" optimization (Q3 in `grid_spec.md`), which is the only architecture where they would provide a runtime benefit.

### 5. Wrapping vs. Signed Arithmetic Discrepancy
**Quote:** `grid_spec.md` S9.5: *"The comparison a.h1[sa] == b.h1[sb] + delta_h must be computed in signed arithmetic (i16 or wider) to handle the negative offset."*
**Quote:** `tile_spec.md` S6.2: *"if a_h1[sa] == b_h1[sb].wrapping_add(delta_h) {"*

**Problem:** The two specs provide conflicting instructions on how to handle the L/R face offset arithmetic. `grid_spec.md` dictates upcasting to signed `i16` integers, while `tile_spec.md` uses idiomatic `u8` wrapping addition. Because `a.h1` and `b.h1` are bounded within `[0, 255]`, `b_h1.wrapping_add(delta_h as u8)` perfectly handles negative offsets via modulo 256 arithmetic without requiring a wider signed cast. 

**Severity:** LOW (style/clarity)

**Suggested Fix:** Standardize on `wrapping_add` (`u8` math) across both specs, as it is efficient, safe, and avoids the casting overhead. Update the narrative in `grid_spec.md` S9.5 to reflect the modulus-based approach defined in `tile_spec.md`.