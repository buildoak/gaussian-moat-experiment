---
date: 2026-03-26
engine: codex
status: complete
---
# Fat-Stripe-CUDA Code Audit

## Executive Summary
The new CUDA tile pipeline is mostly faithful to the existing single-tile reference. The sieve/Miller-Rabin path in `src/row_sieve.cuh` and `src/tile_kernel.cuh` matches the Gaussian-prime definition, the batch kernel in `src/batch_dispatch.cuh` maps blocks to `(tile,row)` correctly, and `src/face_extract.cuh` is a near-direct refactor of `classify_components()` from `src/tile_main.cu`. On its own terms, the local tile math looks sound.

The main correctness failures are higher in the stack. The Rust `fat-stripe-cuda` orchestrator does not match the CPU reference campaign semantics in `tile-probe/crates/fat-stripe/src/orchestrator.rs`: it tiles a plain rectangle instead of the annular/first-octant geometry, it never clips partial edge tiles, it uses the inclusive CUDA `side` convention instead of the CPU orchestrator's `a_hi - 1` / `b_hi - 1` convention, and it reduces the final verdict to `FACE_INNER_BIT && FACE_OUTER_BIT` instead of the CPU reference's radial spanning test over actual `(a,b)` coordinates. Those differences can change the final `blocked` verdict even when the CUDA tile outputs themselves are correct.

The binary protocol is internally consistent on current little-endian targets, but it is still a raw native-endian struct dump with incomplete validation on the Rust side. Test coverage also overstates confidence: the only end-to-end differential test silently becomes a no-op when `FAT_STRIPE_CUDA_BIN` is unset, which was the case in the local test run.

## Verdict Table
| Axis | Verdict | Key Finding |
|------|---------|-------------|
| 1. Mathematical correctness | FAIL | Tile-local sieve/UF logic is sound, but the Rust campaign verdict is not the required radial spanning test and origin semantics diverge near the origin. |
| 2. Binary protocol integrity | CONCERN | C++ and Rust layouts match today, but the stream is raw native-endian data and Rust skips several header/value validations. |
| 3. Batch dispatch correctness | PASS | Block-to-`(tile,row)` mapping, bitmap slicing, and last-batch handling are correct. |
| 4. Bridge correctness | CONCERN | `bridge.rs` reconstructs `TileOperator` mechanically, but it trusts face buckets blindly and does not validate geometry from coordinates. |
| 5. Composition pipeline | FAIL | The CUDA orchestrator does not mirror CPU fat-stripe geometry or tile-bound semantics, so composition can target the wrong region. |
| 6. Edge cases & safety | CONCERN | Target-scale arithmetic is safe, but diagonal/edge tiles are not clipped and explicit OOM degrades poorly. |
| 7. Code quality | CONCERN | Dead config surface, optional differential coverage, and missing validations reduce confidence. |

## Top 5 Risks
1. The final `blocked`/`unblocked` result can be wrong because `fat-stripe-cuda/src/orchestrator.rs` uses a rectangular `FACE_INNER_BIT && FACE_OUTER_BIT` test instead of the CPU reference's radial spanning check over actual port radii.
2. Tile bounds are off by one relative to the CPU fat-stripe orchestrator, and the new orchestrator never clips partial edge tiles, so campaigns can compose the wrong geometry.
3. On-axis campaigns no longer follow the CPU reference annular/first-octant tiling; the new code enumerates a full rectangle and can include tiles the reference never processes.
4. The protocol/bridge path trusts raw native-endian structs and face buckets with limited validation, so malformed or future-format streams can be partially accepted and miscomposed.
5. The only end-to-end CUDA-vs-CPU differential test is skipped when `FAT_STRIPE_CUDA_BIN` is unset, so a green test run does not prove parity.

## Detailed Findings

### 1. Mathematical Correctness
The tile-local Gaussian-prime pipeline is correct.

- Axis points are handled separately through `is_axis_gaussian_prime()`, which checks `|coord| % 4 == 3` and `is_prime(|coord|)` in `src/tile_kernel.cuh:34-57`. Non-axis points use `gaussian_norm_u64(a,b)` and `is_prime(a^2+b^2)` in the same function, which is the correct mathematical definition.
- The row sieve in `src/row_sieve.cuh:76-130` correctly precomputes `sqrt(-1) mod p` for `p ≡ 1 (mod 4)` and separately tracks `p ≡ 3 (mod 4)` primes. The kernel then marks the two residue classes `b ≡ ±a*r (mod p)` for `p ≡ 1 (mod 4)` and the `a ≡ 0, b ≡ 0 (mod p)` case for `p ≡ 3 (mod 4)` in `src/row_sieve.cuh:200-223`. False positives are filtered by the exact predicate in `src/row_sieve.cuh:251-266`.
- The parity prefilter in `src/row_sieve.cuh:190-198` would incorrectly exclude `(±1,±1)` if left alone, but the "rescue small norms" path in `src/row_sieve.cuh:227-248` explicitly clears marked points whose norm is actually prime. The final exact test in `src/row_sieve.cuh:251-266` prevents false acceptance.

The union-find and face extraction logic are faithful to the existing single-tile reference.

- `src/face_extract.cuh:32-58` precomputes the same backward offsets as `src/tile_main.cu:188-203`: all offsets with `da < 0` or `da == 0 && db < 0` whose squared distance is `<= k_sq`. That is the correct half-neighborhood for a row-major one-pass component build.
- The UF pass in `src/face_extract.cuh:121-145` is the same algorithm as `src/tile_main.cu:266-290`, and face classification still uses inclusive `<= collar` checks on all four faces in `src/face_extract.cuh:177-192`, matching `tile-probe/crates/moat-kernel/src/tile.rs:420-446`.

The end-to-end mathematical verdict is still wrong.

- `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:212-216` defines spanning as "some component has both `FACE_INNER_BIT` and `FACE_OUTER_BIT`". That is not the CPU reference behavior.
- The CPU reference performs a radial test over actual face-port coordinates in `tile-probe/crates/fat-stripe/src/orchestrator.rs:104-206`: it computes inner/outer radius thresholds and marks a component spanning only if it has ports on both sides of those radial thresholds.
- This difference is not cosmetic. A component can touch the rectangular inner and outer shell faces without satisfying the radial condition, and vice versa, especially once the tiling is clipped to an annular sector.

There is also a near-origin semantic mismatch.

- The CPU reference unions all primes with `a^2 + b^2 <= k_sq` into one origin-connected component before component extraction in `tile-probe/crates/moat-kernel/src/tile.rs:372-389`.
- The CUDA path only records the first component encountered with `gaussian_norm_u64(a,b) <= k_sq` in `src/face_extract.cuh:173-175`. It does not merge separate near-origin components first.
- This matters only for tiles containing the origin, but it is a real mismatch with the reference tile semantics.

### 2. Binary Protocol Integrity
The C++ and Rust struct layouts match on current targets.

- The C++ protocol structs are declared in `src/face_port_io.h:6-57` and size-checked with `static_assert`.
- The Rust equivalents in `tile-probe/crates/fat-stripe-cuda/src/protocol.rs:11-65` use `#[repr(C)]` plus `Pod`, and the unit test in `tile-probe/crates/fat-stripe-cuda/src/protocol.rs:314-321` verifies the same sizes: 24, 16, 32, 44, and 12 bytes.
- Field order also matches exactly: `magic/version/flags/k_sq/tile_side/...` on both sides, so there is no obvious padding or alignment mismatch on x86_64/aarch64.

The protocol is still only conditionally safe.

- The plan says "Little-endian throughout" in `research/2026-03-26-fat-stripe-cuda-plan.md:105`, but the implementation writes raw native structs with `fwrite(&value, sizeof(T), 1, file)` in `src/fat_stripe_cuda.cu:252-266` and reads them via `pod_read_unaligned` / `cast_slice_mut` in `tile-probe/crates/fat-stripe-cuda/src/protocol.rs:214-226`. There is no byte-order normalization.
- That is fine on current little-endian hosts, but it is not a portable wire format. A big-endian reader or writer would silently misparse fields.

Validation is asymmetric and incomplete.

- C++ validates manifest magic/version/flags in `src/fat_stripe_cuda.cu:269-283`.
- Rust validates manifest and stream magic/version in `tile-probe/crates/fat-stripe-cuda/src/protocol.rs:170-184`, but it does not reject nonzero `flags` or nonzero reserved fields.
- The writer always emits zeroed stream `flags` and `reserved` in `src/fat_stripe_cuda.cu:348-356`, so Rust is currently accepting states the producer never intends to emit.
- `tile-probe/crates/fat-stripe-cuda/src/driver.rs:135-145` checks `tile_id` against the submitted job list, but it does not verify `a_lo`, `b_lo`, or `side`. A stale or reordered file with matching tile IDs could still be composed at the wrong coordinates.

Face-bit encoding is consistent because it is not serialized at all.

- The final protocol revision in `research/2026-03-26-fat-stripe-cuda-plan.md:156-168` dropped `ComponentRecord` and made `component_faces` a derived value.
- The C++ writer emits four separate face arrays in `src/fat_stripe_cuda.cu:311-315`, and Rust reconstructs face bits from those lists in `tile-probe/crates/fat-stripe-cuda/src/bridge.rs:14-27`.
- That derivation is internally consistent, but the bridge does not verify that a record listed on a face actually lies inside the corresponding face band.

### 3. Batch Dispatch Correctness
The batch dispatcher is correct as written.

- The kernel maps `blockIdx.x` to `(tile_idx,row)` via `tile_idx = block / side_exp` and `row = block % side_exp` in `src/batch_dispatch.cuh:134-156`. That matches the design in `research/2026-03-26-fat-stripe-cuda-plan.md:253-271`.
- Each tile writes into its own bitmap slice via `bitmaps + tile_idx * bitmap_words` in `src/batch_dispatch.cuh:155`, and host-side slicing uses the same indexing in `src/batch_dispatch.cuh:306-308`.
- The launcher rejects `num_tiles == 0` and `num_tiles > batch_capacity` in `src/batch_dispatch.cuh:255-257`, zeros exactly `num_tiles * bitmap_words` in `src/batch_dispatch.cuh:265-266`, and launches exactly `num_tiles * side_exp` blocks in `src/batch_dispatch.cuh:271-287`.
- The outer loop in `src/fat_stripe_cuda.cu:359-390` handles the last partial batch correctly with `min(batch_capacity, num_jobs - batch_start)`.

I did not find an off-by-one in bitmap sizing or block dispatch.

- `sample_geom.side_exp` is derived once from `k_sq` and `tile_side` in `src/fat_stripe_cuda.cu:343-345`, which is valid because every job in a manifest shares those two values.
- Grid size is clamped against `unsigned int` launch limits in `src/batch_dispatch.cuh:195-205` and `src/batch_dispatch.cuh:271-279`.

The remaining issues here are robustness, not correctness.

- If the user supplies an explicit batch size that does not fit, `create_batch_context()` just fails at allocation time in `src/batch_dispatch.cuh:207-228`; it does not downshift.
- Host pinned allocation happens after device allocation in `src/batch_dispatch.cuh:213-217`, so auto-sizing from device free memory does not guarantee the host-side pinned buffer will fit.

### 4. Bridge Correctness
`bridge.rs` reconstructs `TileOperator` correctly if the input stream is trusted.

- It rebuilds `component_faces` by OR-ing the four face lists in `tile-probe/crates/fat-stripe-cuda/src/bridge.rs:14-27`, which is exactly how the design revision expects face bits to be recovered.
- It maps the protocol header back to `a_min/a_max/b_min/b_max` in `tile-probe/crates/fat-stripe-cuda/src/bridge.rs:29-45`, consistent with the inclusive CUDA-side `side` convention.

The bridge is still under-validated.

- `k_sq` is unused in `tile-probe/crates/fat-stripe-cuda/src/bridge.rs:10-13`, so the bridge never cross-checks the geometric meaning of a face port against the step bound.
- It does not verify that an `inner` port actually satisfies `a - a_lo <= collar`, that a port lies inside `[a_lo, a_lo + side] x [b_lo, b_lo + side]`, or even that the four face vectors are sorted or deduplicated. It trusts the C++ buckets completely.
- Because of that, a corrupted stream can still produce a syntactically valid `TileOperator` that composes incorrectly.

There is also a small edge-case bug in origin handling.

- `tile-probe/crates/fat-stripe-cuda/src/protocol.rs:288-294` only rejects `origin_component >= num_components`; negative values less than `-1` are accepted.
- `tile-probe/crates/fat-stripe-cuda/src/bridge.rs:41-42` then collapses every negative value to `None`. That is harmless for well-formed output, but it is still weaker validation than intended.

I would rate the reconstruction logic itself as mechanically correct but not self-checking.

### 5. Composition Pipeline
Ordering inside each generated stripe is stable, but the generated geometry does not match the CPU reference pipeline.

The good part:

- `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:164-175` enumerates each stripe in increasing `b_lo`.
- `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:180-195` preserves that order when building `TileJob`s.
- The CUDA binary writes results back in job order even though face extraction is parallelized: it fills `batch_results[i]` in `src/fat_stripe_cuda.cu:372-382` and serializes them in index order in `src/fat_stripe_cuda.cu:384-390`.
- `tile-probe/crates/fat-stripe-cuda/src/driver.rs:135-145` consumes that stream order directly, so there is no hidden reorder bug between the CUDA output and horizontal composition.

The bad part is semantic, not ordering-related.

- The new orchestrator tiles the full rectangle `[r_min, r_max) x [b_min, b_max)` in `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:155-177`.
- The CPU reference instead computes `b_max_stripe` per stripe using annular geometry and the first-octant constraint `b <= a` when `b_min == 0` in `tile-probe/crates/fat-stripe/src/orchestrator.rs:43-63`.
- That means the two orchestrators can process different sets of tiles even before any CUDA logic runs.

Partial edge tiles are also broken.

- The CPU reference clips the top of each stripe with `a_hi = (a_lo + tile_height).min(a_end)` in `tile-probe/crates/fat-stripe/src/orchestrator.rs:41` and clips the right edge of each chunk/tile with `b_chunk_hi = ... .min(b_max_stripe)` and `tb_hi = tb_hi_raw.min(b_chunk_hi)` in `tile-probe/crates/fat-stripe/src/orchestrator.rs:69` and `tile-probe/crates/fat-stripe/src/chunk.rs:65-69`.
- The new orchestrator never clips: every tile is created with the fixed `tile_side` in `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:155-177`, and the CUDA binary always writes that fixed `side` in `src/fat_stripe_cuda.cu:297-309`.
- There is no validation that `(r_max - r_min)` and `(b_max - b_min)` are multiples of `tile_side`, so out-of-range coverage is silent.

There is a second geometry mismatch: the meaning of `side`.

- The CUDA path treats `side` as an inclusive delta: `a_hi = a_lo + tile_side` and `nominal_extent = tile_side + 1` in `src/fat_stripe_cuda.cu:193-199`, and Rust reconstructs `a_max = a_lo + side` in `tile-probe/crates/fat-stripe-cuda/src/bridge.rs:29-33`.
- The CPU fat-stripe path passes `a_hi - 1` and `tb_hi - 1` into `build_tile_from_primes()` in `tile-probe/crates/fat-stripe/src/chunk.rs:68`, so a configured width of 2000 means an inclusive max of `a_lo + 1999`, not `a_lo + 2000`.
- The design doc explicitly identified this as a critical alignment check in `research/2026-03-26-fat-stripe-cuda-plan.md:798-805`.

The current differential test does not catch this.

- `tile-probe/crates/fat-stripe-cuda/tests/differential.rs:82-88` compares against `build_tile(case.a_lo, case.a_lo + side, case.b_lo, case.b_lo + side, ...)`, which bakes in the inclusive CUDA convention.
- That is useful for tile-local parity with `tile_main.cu`, but it does not validate parity with the CPU fat-stripe orchestrator.

### 6. Edge Cases & Safety
Arithmetic is safe at the stated target scale, but edge geometry handling is weak.

- Coordinates are truncated to `i32` on the protocol boundary in `src/face_port_io.h:15-20` and `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:198-201`. At `R ≈ 1e9`, that is still well inside range.
- Norm computations use 128-bit intermediates in `src/tile_kernel.cuh:24-32`, so `a^2 + b^2` is safe at the target scale.
- The row sieve mostly uses `int64_t`/`uint64_t` intermediates in `src/row_sieve.cuh:133-266`, which is also safe around `1e9`.

The main safety issues are geometric and operational.

- The octant axis (`b ≈ 0`) is handled correctly because axis points bypass the sieve mark check and go through the exact predicate in `src/row_sieve.cuh:251-260`.
- The diagonal boundary (`b ≈ a`) is not enforced by the new orchestrator; only the CPU reference clamps to `b <= a` in `tile-probe/crates/fat-stripe/src/orchestrator.rs:43-63`.
- `tile-probe/crates/fat-stripe-cuda/src/bridge.rs:29-33` performs `header.a_lo + header.side as i32` before widening to `i64`. That is safe at the intended scale but would overflow near `i32::MAX`.

OOM behavior is mixed.

- Auto mode (`cuda_batch_size == 0`) derives capacity from `cudaMemGetInfo` in `src/batch_dispatch.cuh:175-193`.
- If even one tile will not fit, the CUDA binary exits with an allocation error, which is acceptable.
- If the user asks for an explicit too-large batch, there is no retry/downshift path; allocation failure becomes a hard error in `src/batch_dispatch.cuh:207-228`.

Crash/partial-output handling is acceptable.

- `tile-probe/crates/fat-stripe-cuda/src/driver.rs:89-105` checks child exit status before opening the output file.
- If the child exits successfully but the file is truncated, `tile-probe/crates/fat-stripe-cuda/src/protocol.rs:187-226` will return an I/O or protocol error while reading.
- Batch directories are cleaned on both success and failure in `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:86-93` and `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:233-238`.

### 7. Code Quality
There are multiple confidence-reducing gaps even where I did not find a direct correctness bug.

- `CudaFatStripeConfig.collar` is stored in `tile-probe/crates/fat-stripe-cuda/src/config.rs:10-23`, accepted from the CLI in `tile-probe/crates/fat-stripe-cuda/src/main.rs:18-20`, and passed through `new()` in `tile-probe/crates/fat-stripe-cuda/src/main.rs:45-56`, but it is never read by the orchestrator or bridge. The actual collar always comes from `ceil_sqrt_u64(k_sq)` on the C++ side in `src/fat_stripe_cuda.cu:188-208`.
- The bridge takes `k_sq` but discards it immediately in `tile-probe/crates/fat-stripe-cuda/src/bridge.rs:10-13`.
- The protocol comment says `num_primes` is the total primes "in this tile" in `src/face_port_io.h:43`, but both implementations actually count the expanded/collar-padded prime set: `src/face_extract.cuh:147-149` and `tile-probe/crates/moat-kernel/src/tile.rs:305`.

Testing is weaker than it looks.

- The end-to-end differential test returns early if `FAT_STRIPE_CUDA_BIN` is unset in `tile-probe/crates/fat-stripe-cuda/tests/differential.rs:34-37`.
- In the local run, `cargo test -p fat-stripe-cuda --test differential -- --nocapture` printed `skipping differential test: FAT_STRIPE_CUDA_BIN is not set` and still passed.
- The plan called for a campaign-level CPU-vs-CUDA composition test in `research/2026-03-26-fat-stripe-cuda-plan.md:631-643`, but I did not find such a test in the new crate.

Validation should also be tightened.

- `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs:142-152` checks only basic ordering/positivity, not divisibility by `tile_side`, not `b_min >= 0`, and not consistency between the requested geometry and the implementation's fixed-size tiles.
- `tile-probe/crates/fat-stripe-cuda/src/protocol.rs:170-184` does not reject nonzero flags/reserved values.
- `tile-probe/crates/fat-stripe-cuda/src/driver.rs:135-145` does not verify returned `a_lo`, `b_lo`, or `side` against the input jobs.

## Recommendations
1. Port the CPU reference campaign semantics into `tile-probe/crates/fat-stripe-cuda/src/orchestrator.rs` before relying on results. That means per-stripe annular/diagonal clipping, support for partial edge tiles or an explicit divisibility precondition, and the full radial spanning test currently implemented in `tile-probe/crates/fat-stripe/src/orchestrator.rs:104-206`.
2. Resolve the `side` contract explicitly. Either make the CUDA path use the CPU fat-stripe convention (`a_max = a_lo + width - 1`) or document and enforce that `tile_side` means inclusive delta everywhere. Update the differential test to compare against the CPU orchestrator's actual tile bounds, not just `build_tile(a_lo, a_lo + side, ...)`.
3. Fix origin semantics for tiles containing the origin by mirroring `build_tile_from_primes()` in `tile-probe/crates/moat-kernel/src/tile.rs:372-389`: union all primes with `norm <= k_sq` before final component labeling.
4. Harden the protocol reader and driver. Serialize explicitly as little-endian, reject nonzero flags/reserved fields, and verify `a_lo`, `b_lo`, and `side` for every returned tile header.
5. Make the differential test meaningful in automation. Either require `FAT_STRIPE_CUDA_BIN` in CI, mark the test `ignored` when the binary is unavailable, or add a build step that materializes the CUDA binary before test execution.
6. Remove or enforce dead surface area: either drop `--collar` from the Rust CLI/config or validate that it matches `ceil(sqrt(k_sq))`; reject campaign ranges that cannot be represented by the implementation's fixed-size tiles.
