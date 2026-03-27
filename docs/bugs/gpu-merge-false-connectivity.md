# GPU Boundary Merge False Connectivity Audit

## Summary

The seam union criterion in CUDA matches the Rust composition code. The false positives come from how the GPU path tracks and interprets face bits after cross-tile union.

There are two distinct issues:

1. **Actual GPU merge bug:** the GPU path preserves **tile-local seam face bits** and later treats them as **campaign boundary bits**. A component that merely crosses an internal vertical seam can be labeled `INNER|OUTER` and reported as spanning even when it never touches the campaign's true inner/outer boundary.
2. **Semantic mismatch vs. fat-stripe ground truth:** `fat-stripe-cuda` uses a rectangular `FACE_INNER_BIT && FACE_OUTER_BIT` verdict, while the known-moat `fat-stripe` path uses a **radial threshold** test over merged port coordinates. Even after fixing (1), GPU merge will still not match the `fat-stripe` moat verdict unless the verdict logic is changed.

## Files Audited

- `src/gpu_uf.cu`
- `src/fat_stripe_cuda.cu`
- `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs`
- `tile-probe/crates/moat-kernel/src/compose.rs`
- `tile-probe/crates/moat-kernel/src/tile.rs`
- `tile-probe/crates/fat-stripe/src/orchestrator.rs`

## Merge Criteria Comparison

### CUDA seam merge

`src/fat_stripe_cuda.cu:575-609`

For each seam pair, CUDA compares every port pair on that seam using:

```cpp
da = pa.a - pb.a
db = pa.b - pb.b
dist_sq = da*da + db*db
if (dist_sq <= k_sq) union(...)
```

### Rust composition

- `tile-probe/crates/moat-kernel/src/compose.rs:17-21`
- `tile-probe/crates/moat-kernel/src/compose.rs:99-118`
- `tile-probe/crates/moat-kernel/src/compose.rs:236-255`

Rust does the same thing:

```rust
da = left.a - right.a
db = left.b - right.b
dist_sq = da*da + db*db
if dist_sq <= k_sq { uf.union(...) }
```

### Conclusion

At the reported radii (`~2.8M`, `~80M`), the GPU and Rust seam match rules are the same: same coordinates, same Euclidean metric, same `<= k_sq` threshold. I did **not** find a merge-threshold bug that explains the reported false connectivity.

## Root Cause 1: Internal Seam Faces Are Mistaken for Campaign Boundary Faces

### Affected code

- `src/fat_stripe_cuda.cu:445-458`
- `src/fat_stripe_cuda.cu:461-484`
- `src/fat_stripe_cuda.cu:760-767`

### What the code does now

While building merge nodes, `prepare_boundary_merge_data()` assigns face bits from **every tile-local face port**:

- inner ports add `gm::kFaceInnerBit`
- outer ports add `gm::kFaceOuterBit`
- left ports add `gm::kFaceLeftBit`
- right ports add `gm::kFaceRightBit`

That happens here:

- `src/fat_stripe_cuda.cu:445-458` via `node_for(..., face_bit)`
- `src/fat_stripe_cuda.cu:481-484` via the four `append_face(...)` calls

After the seam unions, host finalization ORs those bits across the merged component and declares spanning if both inner and outer are present:

- `src/fat_stripe_cuda.cu:760-767`

### Why this is wrong

Those bits are **tile-local** face memberships, not **campaign boundary** memberships.

An internal vertical seam always unions:

- an `OUTER` port from the lower tile
- with an `INNER` port from the upper tile

So a component that merely crosses one internal vertical seam can end up with:

- `FACE_OUTER_BIT` from the lower tile's top seam
- `FACE_INNER_BIT` from the upper tile's bottom seam

and then be falsely reported as spanning.

This is exactly what Rust composition does **not** do.

### CPU reference behavior

Rust composition explicitly drops internal seam faces:

- Horizontal compose keeps only exposed `left` and `right` boundaries:
  - `tile-probe/crates/moat-kernel/src/compose.rs:124-177`
- Vertical compose keeps only exposed `inner` and `outer` boundaries:
  - `tile-probe/crates/moat-kernel/src/compose.rs:261-314`

In particular, vertical compose unions `bottom.face_outer` with `top.face_inner` at
`tile-probe/crates/moat-kernel/src/compose.rs:236-255`, but then the result keeps:

- `face_inner` only from `bottom.face_inner`
- `face_outer` only from `top.face_outer`

So internal seam faces are used for connectivity, then discarded for verdict purposes.

### Minimal failure mode

Two tiles stacked vertically are enough:

1. Lower-tile component touches only its local `outer` face.
2. Upper-tile component touches only its local `inner` face.
3. One seam match unions them.

GPU merge result today:

- merged node has `INNER|OUTER`
- finalizer reports spanning

CPU compose result:

- internal seam faces are discarded
- merged component is **not** spanning unless it also touches the campaign's real bottom/top boundary

### Proposed fix

Keep seam ports for union, but stop treating every local face as a campaign boundary face.

Code-level change:

1. Build tile-neighbor knowledge first (you already have `tile_lookup` in `src/fat_stripe_cuda.cu:492-526`).
2. For each tile, compute whether each face is **exposed**:
   - `inner` exposed iff there is no tile at `(a_lo - side, b_lo)`
   - `outer` exposed iff there is no tile at `(a_lo + side, b_lo)`
   - `left` exposed iff there is no tile at `(a_lo, b_lo - side)`
   - `right` exposed iff there is no tile at `(a_lo, b_lo + side)`
3. Create merge nodes exactly as today, but OR face bits into `node_face_bits` **only for exposed faces**.
4. Internal seam faces must still be emitted into `inner_ports` / `outer_ports` / `left_ports` / `right_ports` for seam matching; they just must not contribute boundary bits.

Practical refactor:

- Change `node_for(component_id, face_bit)` into `node_for(component_id)`.
- Make `append_face(...)` optionally OR a `boundary_bit` only when that face is exposed on the campaign boundary.

### Fix scope

**CUDA-only.**

This is enough to make GPU boundary merge match the rectangular-face semantics of the Rust compose path in `fat-stripe-cuda`.

## Root Cause 2: GPU/Fat-Stripe-CUDA Verdict Uses Rectangular Faces, Not the Radial Moat Test

### Affected code

- `src/fat_stripe_cuda.cu:763-767`
- `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:256-264`
- `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:361-365`
- Reference ground truth: `tile-probe/crates/fat-stripe/src/orchestrator.rs:100-195`

### What the code does now

Both GPU merge summary and the non-GPU-merge `fat-stripe-cuda` path declare spanning when a component has both rectangular face bits:

- `INNER`
- `OUTER`

### Why this diverges from known-moat ground truth

The `fat-stripe` campaign code explicitly says rectangular face bits are not sufficient for annular probes:

- `tile-probe/crates/fat-stripe/src/orchestrator.rs:100-106`

It instead checks whether a merged component has:

- at least one port with `r <= r_inner + collar`
- and at least one port with `r >= r_outer - collar`

using actual `(a, b)` coordinates:

- `tile-probe/crates/fat-stripe/src/orchestrator.rs:163-194`

So if your "CPU UF ground truth" is the `fat-stripe` moat campaign, then `fat-stripe-cuda` is currently solving a different verdict problem.

### Proposed fix

If the intended result is the same moat verdict as `fat-stripe`, then the GPU merge path must compute the same radial test after merging.

Code-level options:

1. After host-side `parent` flattening in `run_gpu_boundary_merge()`, walk all merged face ports and accumulate per-component:
   - `has_inner_radial`
   - `has_outer_radial`
2. Use the same thresholds as `fat-stripe`:
   - on-axis: based on `r_min`, `r_max`, and `ceil(sqrt(k_sq))`
   - off-axis: based on rectangular corner radii, same as `fat-stripe`
3. Report spanning only from that radial test.

Required plumbing:

- `run_gpu_boundary_merge()` currently only receives `PreparedMergeData` and `k_sq`.
- To reproduce `fat-stripe` semantics it also needs the campaign geometry / thresholds (`r_min`, `r_max`, `b_min`, `b_max`, or precomputed `r_inner_sq`, `r_outer_sq`).

### Fix scope

**Rust + CUDA interface changes** if you want GPU merge to match `fat-stripe` moat verdicts.

If you only want GPU merge to match the current rectangular `fat-stripe-cuda` compose path, then fix (1) above is sufficient and no Rust-side change is required.

## Not the Root Cause

### `merge_seams_kernel` threshold logic

Not the cause of the reported failures. CUDA and Rust use the same seam-distance rule.

### `merge_flatten_kernel`

Not the cause of the reported false connectivity.

- Device flatten runs three passes at `src/fat_stripe_cuda.cu:723-726`
- Then host code does a full `host_find()` normalization at `src/fat_stripe_cuda.cu:736-745`

So incomplete device flatten cannot explain a stable false spanning component in the final host summary.

## Bottom Line

The concrete GPU bug is **not** in the seam distance comparison. It is in the **face-bit semantics**:

- GPU merge unions internal seam endpoints correctly,
- but then it incorrectly treats the seam endpoints' local `INNER` / `OUTER` labels as global campaign-boundary labels.

That is enough to manufacture fake spanning components.

If your comparison target is the annular `fat-stripe` moat verdict, there is also a second mismatch: the GPU path still uses a rectangular face-bit verdict instead of the radial threshold test.
