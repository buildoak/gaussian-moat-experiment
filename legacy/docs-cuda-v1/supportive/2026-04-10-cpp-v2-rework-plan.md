---
title: C++ TileOp V2 Rework Plan
date: 2026-04-10
engine: codex
type: design-note
status: complete
refs: [docs/tile_spec.md, docs/tile_operations.md, docs/grid_spec.md, docs/compositor_spec.md, docs/supportive/2026-04-09-port-overflow-census.md, tile-cpp/include/constants.h, tile-cpp/include/types.h, tile-cpp/src/encode.cpp, tile-cpp/src/face_extract.cpp, tile-cpp/src/process_tile.cpp, tile-cpp/tests/test_face_encode.cpp, tile-cpp/tests/census_tiles.cpp, tile-cpp/tests/test_sieve.cpp, tile-cpp/tests/test_e2e.cpp]
---

# C++ TileOp V2 Rework Plan

## Decision

Implement TileOp V2 in `tile-cpp` as the 128-byte dynamic packed layout from the current specs, not as the older 192-byte `PORTS_PER_FACE=24` expansion from the overflow census note. Stage the work behind a small encode/decode abstraction so the approved group-bit-steal encoding is implemented once and reused everywhere.

## Rationale

The authoritative specs now require:

- fixed `128` byte TileOps with a `3` byte offset header and dynamic O-I-L-R payload packing (`docs/tile_spec.md:171-225`)
- overflow as all `0xFF` and dead/empty as `off_I = off_L = off_R = 3` (`docs/tile_spec.md:255-284`)
- compositor-side offset-derived parsing (`docs/compositor_spec.md:112-139`)

The census note recommends a different design: `192` bytes and `PORTS_PER_FACE=24` (`docs/supportive/2026-04-09-port-overflow-census.md:108-119`). That recommendation predates the current V2 spec and must not drive implementation unless the user explicitly reopens the spec.

Rejected alternatives:

1. Keep V1 and just raise `PORTS_PER_FACE`.
Killed by the spec. The format is no longer fixed four-face slots, and the current Rust compositor contract is header-derived.

2. Implement the census note’s 192-byte format instead.
Killed by authority chain and blast radius. It changes record size, memory budget, and every downstream parser.

3. Keep treating `h1 >= 256` as overflow.
Killed by the approved resolution. Group-bit steal encodes the shared-boundary row exactly and eliminates this false blocker.

Assumptions:

- `tile-cpp` is currently the C++ reference producer only; it does not yet implement the compositor.
- `FaceData` remains the phase boundary between extraction/pruning and encoding.
- The executor may add decode helpers used by tests and tools even if production currently only encodes.

## Resolved h1 Encoding

The `h1 = 256` contradiction is resolved by the approved group-bit-steal encoding:

- store `group_byte = ((h1 >> 8) << 7) | (group_id & 0x7F)`
- store `h1_byte = h1 & 0xFF`
- decode `group_id = group_byte & 0x7F`
- decode `h1 = ((group_byte >> 7) << 8) | h1_byte`

This uses the fact that `h1` needs only 9 bits (`0..256`) while observed
group counts remain far below the new global cap of `127`.

## Plan

### 1. Replace V1 constants and introduce a TileOp V2 parse/emit vocabulary

What to do:

1. In `tile-cpp/include/constants.h:21-24`, remove `PORTS_PER_FACE` as an encoding constant. Replace it with V2 constants:
   - `TILEOP_HEADER_BYTES = 3`
   - `TILEOP_PAYLOAD_BYTES = 125`
   - byte offsets/constants for dead and overflow sentinels
2. In `tile-cpp/include/types.h:11-28`, keep `TileOp { uint8_t bytes[128]; }`, but add explicit helper structs for V2 metadata. Minimum set:
   - `TileOpFaceView` with pointer/count fields for group bytes and optional packed `h1` bytes
   - `TileOpLayout` or equivalent with `off_I`, `off_L`, `off_R`, derived counts, `h_start`, status flags
3. Add public helpers in `tile-cpp/include/encode.h` and implement them in `tile-cpp/src/encode.cpp`:
   - `make_overflow_tileop()`
   - `make_empty_tileop()`
   - `parse_tileop_v2(const TileOp&)`
   - `max_group_label(const TileOp&)`
   - face accessors for groups and packed `h1` slices, plus decoded `face_h1()`
4. Validate headers structurally in the parser:
   - offsets monotone: `3 <= off_I <= off_L <= off_R <= 128`
   - derived counts non-negative
   - `h_start + l_cnt + r_cnt <= 128`
   - dead/overflow recognized before normal parsing

Before:

- `tile-cpp/include/constants.h:21-24` encodes V1 as `PORTS_PER_FACE=16`
- `tile-cpp/include/types.h:11-13` exposes only raw bytes
- `tile-cpp/src/encode.cpp:11-70` emits bytes without a corresponding parser

After:

- one source of truth for V2 layout
- tests/tools decode through the same helper path the future compositor will use

Verification gate:

- A new parser-focused unit section in `tile-cpp/tests/test_face_encode.cpp` proves:
  - all-zero V1-style bytes are rejected as invalid V2, not misread as a live tile
  - `make_empty_tileop()` parses as empty/dead with zero counts
  - all-`0xFF` parses as overflow
  - a handcrafted normal TileOp yields exact `(o_cnt, i_cnt, l_cnt, r_cnt, h_start)`

Failure mode:

- If parser math is wrong, synthetic cases fail with count mismatches before any end-to-end tile processing is touched.

### 2. Rework the encoder from fixed slots to dynamic O-I-L-R packing

What to do:

1. Replace the V1 write path in `tile-cpp/src/encode.cpp:11-70`.
2. Compute face counts from `FaceData` by face, but emit in spec order `O, I, L, R`, not the current extraction order.
3. Enforce V2 byte budget:
   - overflow if `group_count > 127`
   - overflow if `o_cnt + i_cnt + 2*l_cnt + 2*r_cnt > 125`
4. Write the header first:
   - `off_I = 3 + o_cnt`
   - `off_L = off_I + i_cnt`
   - `off_R = off_L + l_cnt`
5. Pack payload sections exactly as specified:
   - `bytes[3 .. off_I)` O groups
   - `bytes[off_I .. off_L)` I groups
   - `bytes[off_L .. off_R)` L groups
   - `bytes[off_R .. off_R + r_cnt)` R groups
   - then L `h1` bytes, then R `h1` bytes
   - zero the optional trailing pad if present
6. Keep overflow as `memset(0xFF)` of the full 128 bytes.

Before:

- `tile-cpp/src/encode.cpp:36-67` writes fixed 16-byte face slots and fixed `L/R h1` blocks at bytes `64` and `80`
- overflow is triggered by `count > PORTS_PER_FACE`, which is a V1 rule

After:

- encoder matches `docs/tile_spec.md:173-208`
- sparse faces donate bytes to dense faces

Verification gate:

- Synthetic encode tests prove the exact header bytes and payload byte positions for:
  - a mixed-face tile with nonzero O/I/L/R counts
  - a tile that fits V2 but would have overflowed V1 because one face exceeds `16`
  - a tile that exceeds the `125` byte budget and poisons to all `0xFF`

Failure mode:

- If the executor accidentally preserves I-O-L-R payload order or fixed offsets, the parser round-trip will return the wrong face counts immediately.

### 3. Normalize empty/dead TileOp handling in `process_tile`

What to do:

1. In `tile-cpp/src/process_tile.cpp:36-64`, stop returning a zeroed `TileResult` as an implicit TileOp.
2. Initialize `result.tileop` to `make_empty_tileop()` before any early return.
3. Ensure the zero-prime early return still emits a valid V2 empty layout.
4. If the C++ reference later adds explicit dead-tile geometry checks, reuse the same empty-layout helper.

Before:

- `std::memset(&result, 0, sizeof(result))` yields `bytes[0..2] = 0`, which is not the V2 dead/empty header

After:

- every produced TileOp is either valid empty, valid normal, or valid overflow

Verification gate:

- A unit test calls `process_tile` on a synthetic coordinate path that returns no primes or bypasses encode and verifies:
  - `tileop.bytes[0..2] == {3,3,3}`
  - `tileop.bytes[3] == 0`

Failure mode:

- If this is missed, downstream V2 decoders and future compositor work will misclassify empty tiles as corrupt records.

### 4. Keep face extraction/pruning logic, but harden interfaces around V2 assumptions

What to do:

1. Leave the core clustering and pruning algorithms in place:
   - `tile-cpp/src/face_extract.cpp:66-203`
   - `tile-cpp/src/prune.cpp:20-66`
2. Tighten the contract on `FaceData` in `tile-cpp/include/types.h:23-28`:
   - document that ports are stored in extraction order `I, O, L, R`
   - document that encoding reorders to `O, I, L, R`
3. Preserve `Port::h1` as at least `uint16_t`; do not narrow it before the `>> 1` pack step.
4. Add an explicit helper that computes per-face counts before encode so encode no longer relies on contiguous same-face spans by accident.
5. Add a targeted boundary test for the shared-endpoint convention:
   - `tile_row == TILE_SIDE` and `tile_col == TILE_SIDE` must still be in-tile
   - `FACE_O` and `FACE_R` include offsets `250..256`

Before:

- extraction already respects `<= TILE_SIDE` (`tile-cpp/src/face_extract.cpp:81`, `tile-cpp/src/process_tile.cpp:22`)
- encode assumes same-face ports appear contiguously and in face index order

After:

- the extraction/pruning side stays stable
- the encoding side stops depending on V1 slot thinking

Verification gate:

- Existing extraction tests are expanded to include representative interior and shared-boundary anchors and prove extraction preserves the `uint16_t` value before serialization.

Failure mode:

- If someone narrows `h1` early, the test catches the loss before the pack/decode helper runs.

### 5. Implement the resolved group-bit-steal serialization rule

What to do:

1. Add a single pack/decode helper in `tile-cpp/src/encode.cpp` responsible for converting internal `uint16_t h1` and `uint8_t group` to packed bytes and back.
2. Route all L/R group-byte writes through `pack_group_byte(group, h1) = ((h1 >> 8) << 7) | (group & 0x7F)`.
3. Route all reads through `decode_group_id(group_byte) = group_byte & 0x7F` and `decode_h1(group_byte, h1_byte) = ((group_byte >> 7) << 8) | h1_byte`.
4. Add boundary tests covering `h1=0`, representative interior anchors, and the old `h1=256` case.

Directive:

- Do not scatter group-byte bit extraction through the codebase.
- Do not compare stored bytes directly when matching faces.

Verification gate:

- There is exactly one place in the codebase that decides how `h1` is packed and decoded.
- Boundary tests demonstrate exact round-trip for representative anchors, including `h1=256`.

Failure mode:

- Without this isolation, later spec changes will require another cross-file audit and risk mismatched tool behavior.

### 6. Update every V1 decoder in tests and tools to use the shared V2 parser

What to do:

1. Replace the fixed-slot decode logic in `tile-cpp/tests/census_tiles.cpp:33-49`.
2. Census should decode per-face counts from the V2 header via `parse_tileop_v2()`, not by counting nonzero bytes in four 16-byte regions.
3. Add V2-specific census columns:
   - `off_I`, `off_L`, `off_R`
   - `o_cnt`, `i_cnt`, `l_cnt`, `r_cnt`
   - `payload_bytes_used`
   - `payload_slack`
   - `status` (`normal`, `empty`, `overflow`, `invalid`)
4. Record overflow reason at the census site if available from encoder diagnostics:
   - `group_label_overflow`
   - `payload_budget_overflow`
   - no dedicated `h1` overflow reason; group-bit steal is part of the normal encode path
5. Update `tile-cpp/tests/run_tile.cpp:64-73` and `tile-cpp/tests/test_e2e.cpp:36-37` to print V2-aware summaries instead of “first 32/64 bytes” only.

Before:

- `census_tiles.cpp` assumes fixed byte ranges and therefore will lie once V2 lands

After:

- tools become validation instruments for the new layout, not stale observers

Verification gate:

- Running the census on a small operating-point sample shows:
  - decoded counts equal `ports_after_pruning` aggregated by face
  - `payload_bytes_used = o+i+2*l+2*r`
  - no `invalid` rows for normal tiles

Failure mode:

- If the executor forgets this step, the implementation may be correct while the measurement tooling falsely reports nonsense.

### 7. Replace low-radius tests and add operating-point V2 coverage

What to do:

1. Rewrite `tile-cpp/tests/test_sieve.cpp:143-205` because it violates the project rule that all tests run at `R >= 800M`.
2. Split test intent:
   - keep exact brute-force or near-origin math checks only if moved out of the main required test suite and clearly marked non-validation
   - add operating-point tests at `R >= 800,000,000` and off-axis coordinates
3. Update `tile-cpp/tests/test_e2e.cpp:68-75` so every coordinate is compliant. `tile_c` is not; `sqrt(820888320^2 + 220000000^2) < 800M`.
4. Add at least two angular positions, e.g. one around `45°` and one around `30°`, both aligned to `256`.
5. Expand `tile-cpp/tests/test_face_encode.cpp:99-140` into V2 round-trip tests:
   - parser on handcrafted V2 bytes
   - encoder/parser round-trip on synthetic `FaceData`
   - budget overflow case
   - empty-layout case

Rejected alternative:

- Keeping the existing low-radius tests as “good enough smoke”.
Killed by project policy and by the fact that near-origin density is explicitly non-representative.

Verification gate:

- The default test set contains no coordinates with radius below `800M`.
- The V2 encode/decode tests all run without relying on fixed slots.

Failure mode:

- If low-radius fixtures remain, the suite can pass while missing operating-point regressions in port counts and pruning behavior.

### 8. Update benchmark/census generation to validate V2 behavior, not V1 overflow assumptions

What to do:

1. Keep `tile-cpp/tests/bench_tiles.cpp` as the throughput driver, but add optional reporting of:
   - percentage of empty tiles
   - percentage of overflow tiles
   - average payload bytes used
2. Rework `tile-cpp/tests/census_tiles.cpp:163-198` only as needed to keep operating-point generation compliant, but do not redesign tower placement in this pass unless it blocks V2 validation.
3. Use the census to answer V2-specific questions:
   - what fraction still overflow under the `125` byte budget
   - what is the distribution of payload slack
   - how often encoded `h1` reaches `256` and exercises the stolen bit

Verification gate:

- A pilot census at `R=860,000,000` produces nonzero counts for `payload_slack` and a materially lower overflow rate than the old 33.16% V1 census.

Failure mode:

- If overflow remains close to 33%, the executor likely preserved a V1 limit or miscomputed the budget.

### 9. Keep build-system churn minimal

What to do:

1. `tile-cpp/CMakeLists.txt:1-55` can stay structurally unchanged.
2. Only add new test source files if the existing `test_face_encode.cpp` becomes too overloaded; otherwise prefer extending current targets.
3. Do not widen the public build surface until V2 encoding is stable.

Verification gate:

- `cmake --build` still produces the same top-level targets:
  - `test_sieve`
  - `test_compact_uf`
  - `test_face_encode`
  - `test_e2e`
  - `run_tile`
  - `bench_tiles`
  - `census_tiles`

Failure mode:

- If the executor adds many new executables instead of extending the existing ones, maintenance cost rises without improving proof quality.

## Order Of Operations

1. Step 1 first.
Reason: every downstream test/tool change should consume the shared parser, not re-implement V2.

2. Step 2 second.
Reason: encoder is the core format change and the main regression source.

3. Step 3 third.
Reason: empty/dead validity affects all callers and early returns.

4. Step 4 fourth.
Reason: this hardens the producer-side contract without changing extraction semantics prematurely.

5. Step 5 is a gate, not optional cleanup.
Reason: the pack/decode helper must be the single source of truth for group-bit steal.

6. Step 6 after parser+encoder are stable.
Reason: tools must validate the new truth, not stale byte offsets.

7. Step 7 after the format stabilizes.
Reason: rewrite tests once, against the final parser/encoder path.

8. Step 8 after tests pass.
Reason: benchmark/census are proof tools, not primary implementation scaffolding.

9. Step 9 throughout, with no separate branch of work.

## Verification Matrix

The executor is done when all of the following are true:

1. `encode_tileop()` never references fixed offsets `16`, `32`, `48`, `64`, or `80` as face-slot semantics.
2. `census_tiles.cpp` no longer counts ports by scanning four fixed 16-byte slots.
3. Empty tiles serialize as `{3,3,3}` header, not zero bytes.
4. Overflow still serializes as all `0xFF`.
5. All default validation coordinates satisfy radius `>= 800,000,000` and are off-axis.
6. A synthetic V2 round-trip test proves parser and encoder agree on counts and section boundaries.
7. There is a single isolated helper governing group-bit-steal pack/decode.

## Risks

1. Misapplied group-byte decode.
This is the main `h1` risk. If bit 7 or the low byte is handled incorrectly, matches will be off by 256 or by the wrong group ID.

2. Empty versus dead semantics.
The current C++ producer has no explicit dead-tile geometry pass. Using the same empty layout for “no ports” and “dead” may be acceptable for now, but the executor should not claim those semantics are fully separated unless the geometry check is added.

3. Tool drift.
The biggest likely failure is not the encoder; it is stale tools silently decoding V2 bytes as V1. That is why the shared parser comes first.

4. False confidence from low-radius tests.
If the executor leaves those tests in place, the suite will under-sample the actual operating regime.

5. Misordered payload sections.
`FaceData` is naturally produced in `I, O, L, R` order, while V2 payload order is `O, I, L, R`. This is an easy place to produce valid-looking but wrong bytes.

## What I Would Pressure-Test During Review

1. Any patch that keeps `PORTS_PER_FACE` in encode/decode logic is incomplete.
2. Any patch that compares stored L/R bytes directly without group-bit-steal decode is not acceptable.
3. Any patch that updates the encoder but not `census_tiles.cpp` is not validated.
4. Any patch that leaves `test_sieve.cpp` centered on `(100,100)` and `(10000,10000)` is out of policy.

I pressure-tested this plan and found no structural issues once the approved group-bit-steal pack/decode rule is treated as mandatory shared infrastructure.
