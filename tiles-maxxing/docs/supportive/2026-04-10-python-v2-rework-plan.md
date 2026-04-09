---
title: Python TileOp V2 Rework Plan
date: 2026-04-10
engine: codex
type: design-note
status: complete
refs:
  - docs/tile_spec.md
  - docs/tile_operations.md
  - docs/grid_spec.md
  - docs/compositor_spec.md
  - docs/supportive/2026-04-09-port-overflow-census.md
---

# Python TileOp V2 Rework Plan

## Decision

Rework the Python validator around one shared TileOp v2 module, then make every validator, compositor helper, and measurement script consume that module instead of duplicating V1-era fixed-slot assumptions.

## Rationale

This is the lowest-risk path because the current breakage is not one bug. It is a split brain:

1. `tile-validator/ports.py` and `tile-validator/tile.py` already moved partway toward the spec-aligned pipeline.
2. `tile-validator/compose.py` and `tile-validator/validate.py` still decode a fixed 16-slot face layout.
3. `diagnose_tile.py`, `measure_uncapped.py`, and `group_stats.py` duplicate pre-shared-module logic and still reason in terms of `MAX_PORTS_PER_FACE = 16`.

Alternatives rejected:

- Patch each script in place and keep duplicate encode/decode logic.
  Rejected because V2’s failure mode is semantic drift. The fixed-offset decoder in one file and the encoder in another will diverge again.
- Put decode helpers into `compose.py` and let other files import from there.
  Rejected because `compose.py` is a consumer, not the canonical owner of TileOp layout rules. It also mixes byte parsing with union logic.
- Keep the Python tools V1-shaped and only use them for rough diagnostics.
  Rejected because the user asked for validators that prove C++ TileOp v2 output is correct. Approximate tooling cannot close that gate.

Assumptions:

- TileOp v2 format is authoritative exactly as specified in `docs/tile_spec.md` v5 and `docs/tile_operations.md` v2.
- The approved `h1 >> 1` encoding is part of this rework. Python should store `h1_packed = h1 >> 1`, decode `h1 = 2*h1_packed + face_parity`, and treat face-parity recovery as shared layout logic rather than ad hoc matcher behavior.
- Tests and sampling must stay at operating radii only: `sqrt(a_lo^2 + b_lo^2) >= 800,000,000`, off-axis, tile-aligned.

## Current State Map

### Shared code already present

- `tile-validator/ports.py:9-215`
  Owns face extraction, clustering, pruning, and encoding.
- `tile-validator/tile.py:117-183`
  Owns the canonical 5-phase Python pipeline and calls `ports.encode_tileop()`.
- `tile-validator/uf.py:62-91`
  Shared component builder; no TileOp layout assumptions.
- `tile-validator/primes.py:20-76`
  Shared primality backend; no TileOp layout assumptions.

### Files with V1 layout assumptions

- `tile-validator/ports.py:10`
  `MAX_PORTS_PER_FACE = 16` still drives overflow decisions.
- `tile-validator/ports.py:188-214`
  `encode_tileop()` still writes fixed face blocks at `0/16/32/48/64/80`.
- `tile-validator/compose.py:10-35`
  Decoder assumes fixed block offsets and zero-terminated 16-slot face arrays.
- `tile-validator/validate.py:35-66`
  Validator still checks that bytes `96..127` are zero-filled, which is invalid under V2 because those bytes now hold live payload.
- `tile-validator/validate.py:93-118`
  L/R matching still wraps `delta_h` mod 256, which is wrong under the approved decode rule. Matching must move to signed raw-h1 arithmetic after decoding.

### Scripts duplicating old pipeline logic

- `tile-validator/diagnose_tile.py:33-389`
- `tile-validator/measure_uncapped.py:48-256`
- `tile-validator/group_stats.py:52-334`

These scripts duplicate sieve, compact, UF, face extraction, clustering, pruning, and overflow judgment instead of consuming `tile.py` + `ports.py`.

## TileOp V2 Mapping For Python

Directive: represent TileOp v2 in Python as raw `bytes` plus one parsed metadata object derived from the 3-byte header.

Required parse rules:

- Overflow tile:
  `tileop[0] == 0xFF` and the entire 128 bytes must be `0xFF`.
- Dead tile:
  `off_I == off_L == off_R == 3` and `tileop[3] == 0`.
- Normal tile offsets:
  `off_I = tileop[0]`, `off_L = tileop[1]`, `off_R = tileop[2]`.
- Derived counts:
  `o_cnt = off_I - 3`
  `i_cnt = off_L - off_I`
  `l_cnt = off_R - off_L`
  `r_cnt = (125 - o_cnt - i_cnt - 2*l_cnt) >> 1`
  `h_start = off_R + r_cnt`
- Payload slices:
  `O groups = tileop[3:off_I]`
  `I groups = tileop[off_I:off_L]`
  `L groups = tileop[off_L:off_R]`
  `R groups = tileop[off_R:off_R+r_cnt]`
  `L h1>>1 = tileop[h_start:h_start+l_cnt]`
  `R h1>>1 = tileop[h_start+l_cnt:h_start+l_cnt+r_cnt]`
- Optional pad:
  byte 127 may be padding when residual budget is odd and must be ignored by decoders.

Validation invariants Python must enforce for every normal tile:

1. `len(tileop) == 128`
2. `3 <= off_I <= off_L <= off_R <= 127`
3. `o_cnt + i_cnt + 2*l_cnt + 2*r_cnt <= 125`
4. `len(L groups) == len(L h1>>1)` and `len(R groups) == len(R h1>>1)`
5. All stored groups lie in `[1, 255]`
6. Empty tile means all decoded face sections are empty
7. Group decode order is payload order `O-I-L-R`, while group assignment order remains `I-O-L-R`

## Exact File And Function Plan

### 1. Add one shared TileOp v2 module

Directive:
Create a new shared module, preferably `tile-validator/tileop.py`, and move all byte-layout knowledge there.

Why a new module:

- `ports.py` should own phase-4/5 semantic objects, not low-level parse policy.
- `compose.py` should consume parsed TileOps, not define the format.

Functions to add:

- `is_overflow_tileop(tileop: bytes) -> bool`
  Before: only exists in `compose.py:16-17`.
  After: shared canonical sentinel check that also confirms full poisoning when used in validator mode.
- `is_dead_tileop(tileop: bytes) -> bool`
  New helper implementing the spec 3-way status check.
- `parse_tileop(tileop: bytes) -> ParsedTileOp`
  New shared parser returning offsets, counts, and per-face slices.
- `decode_face_groups(tileop: bytes, face: str) -> list[int]`
  Before: `compose.py:20-27` uses fixed 16-byte blocks.
  After: header-derived slicing.
- `decode_face_h1(tileop: bytes, face: str, *, tile_origin: tuple[int, int]) -> list[int]`
  Before: `compose.py:30-35` uses fixed offsets.
  After: header-derived slicing for `L/R`, empty for `I/O`.
- `decode_tileop(tileop: bytes) -> dict[str, dict[str, list[int]]]`
  Before: `compose.py:38-42`.
  After: shared v2 decode wrapper.
- `validate_tileop_structure(tileop: bytes) -> list[str]`
  New helper returning structural violations for validator and diagnostic scripts.

Verification gate:

- On a known alive tile, parser returns offsets/counts consistent with face-port lengths from `process_tile()`.
- On an overflow tile, parser reports overflow and does not attempt normal count derivation.
- On an empty tile, parser reports zero-length face sections and dead status.

### 2. Rewrite encoder in `ports.py`

Directive:
Replace fixed-slot encoding with V2 dynamic packed encoding and keep `ports.py` as the single encoder.

Exact changes:

- `tile-validator/ports.py:10`
  Remove `MAX_PORTS_PER_FACE = 16` as an encoding constraint. If a constant remains, it should describe spec data budget, not an artificial per-face cap.
- `tile-validator/ports.py:188-214` `encode_tileop(...)`
  Before:
  overflows if any face exceeds 16; writes fixed groups at bytes `0/16/32/48`; zero-fills tail.
  After:
  computes `o_cnt/i_cnt/l_cnt/r_cnt`, checks packed budget `o+i+2l+2r <= 125`, checks `group_count <= 255`, packs each L/R anchor as `h1 >> 1`, writes header bytes `off_I/off_L/off_R`, packs payload in `O/I/L/R/Lh1/Rh1` order, leaves only optional terminal pad.

Behavioral note:

- Keep face ordering in the payload as `O-I-L-R`.
- Do not change group assignment ordering from `assign_groups()`; it stays `I-O-L-R` per spec.

Verification gate:

- For a non-overflow tile, re-decoding the emitted bytes through the new shared parser returns exactly the same group lists and decoded raw L/R h1 lists as the input ports.
- The byte budget check fires on packed-budget overflow, not on `>16` per-face counts.
- A tile with face counts above 16 but still within `125` bytes encodes successfully.

### 3. Repoint the canonical pipeline to shared TileOp helpers

Directive:
Keep `tile.py` as the canonical pipeline entry point, but make its result richer so all scripts can consume one shape.

Exact changes:

- `tile-validator/tile.py:117-183` `process_tile(...)`
  Before:
  returns `tileop` bytes and high-level port lists, but not decoded TileOp metadata.
  After:
  additionally returns either parsed header/count metadata or a lightweight `tileop_status`/`tileop_decoded` structure so downstream scripts do not each decode ad hoc.

Recommended extra fields:

- `tileop_status`: `normal|dead|overflow`
- `tileop_offsets`: `off_I/off_L/off_R` for normal tiles
- `tileop_counts`: `o_cnt/i_cnt/l_cnt/r_cnt`
- `tileop_decoded`: face groups and decoded raw h1 arrays

Verification gate:

- For alive tiles, `result["ports"]` and `result["tileop_decoded"]` agree exactly.
- For dead tiles, all per-face decoded lists are empty.
- For overflow tiles, decoded normal payload is absent and status is `overflow`.

### 4. Fix decoder and compositor helpers in `compose.py`

Directive:
Turn `compose.py` into a pure consumer of shared V2 parsing, not an owner of layout rules.

Exact changes:

- `tile-validator/compose.py:10-12`
  Remove `FACE_GROUP_OFFSETS`, `FACE_H1_OFFSETS`, `MAX_PORTS_PER_FACE`.
- `tile-validator/compose.py:16-42`
  Delete or replace these local decode helpers with imports from the new shared module.
- `tile-validator/compose.py:124-160` `compose_vertical(...)`
  Keep the shared-prime pairing path first. When falling back to positional slot matching on aligned I/O faces, use header-derived O/I lengths, not zero-terminated fixed slots.
- `tile-validator/compose.py:163-197` `compose_horizontal(...)`
  Use header-derived L/R groups and decoded raw h1 lists. Do not rely on fixed offsets. Preserve the `delta_h == 0` shared-prime identity shortcut.

Failure mode to design for:

- If aligned I/O faces decode to different port counts, this is a validation error, not silent truncation.

Verification gate:

- Existing composition behavior is unchanged for tiles whose V1 and V2 encodings happen to represent the same ports.
- A tile with 17-20 ports on one face composes without decode truncation.
- For a hand-picked horizontal neighbor pair with `delta_h != 0`, the unions derived from decoded L/R data match the shared-prime overlap oracle from raw `ports`.

### 5. Rewrite layout validation in `validate.py`

Directive:
Change the validator from “reserved-tail zero check” to “TileOp v2 structural invariant check”.

Exact changes:

- `tile-validator/validate.py:35-66` `validate_tileop_bytes(...)`
  Before:
  validates fixed-width decode and insists bytes `96..127` are zero.
  After:
  validates:
  `len == 128`,
  overflow poisoning semantics,
  dead-tile semantics,
  offset monotonicity,
  count derivation consistency,
  section agreement on L/R groups vs packed h1 sections,
  decoded groups/h1 equality against `result["ports"]`.
- `tile-validator/validate.py:69-90` `validate_io_alignment(...)`
  Keep shared-prime identity check, but also assert header-derived O/I counts agree when neither side overflows.
- `tile-validator/validate.py:93-118` `validate_lr_matching(...)`
  Report two things separately:
  the shared-prime match count when `delta_h == 0`,
  the decoded raw-h1 equality matches for general `delta_h`.
  Also assert that no matcher path uses wraparound arithmetic on packed bytes; all comparisons must happen after parity decode.

Verification gate:

- Validator passes on normal V2 tiles and fails on intentionally malformed headers.
- A synthetic malformed tile with `off_I > off_L` is rejected.
- A synthetic malformed tile with mismatched L-group and L-h1>>1 section lengths is rejected.

### 6. Collapse duplicate diagnostic logic into shared pipeline calls

Directive:
Stop maintaining three parallel copies of the tile pipeline.

#### `tile-validator/diagnose_tile.py`

Exact changes:

- Replace duplicated `sieve_bitmap`, `compact_bitmap`, `build_components`, `collect_face_primes`, `cluster_into_ports`, `assign_groups`, `compute_group_face_incidence`, `identify_dead_ends`, `prune`.
- Keep only presentation/reporting functions and have `diagnose()` call `process_tile()` plus shared pruning/classification helpers.
- Expand output to print V2 header fields, packed counts, payload budget usage, decoded face sections, and the face parity used for h1 decode.

Verification gate:

- For a tile that used to be “overflow because max_face > 16”, the script now distinguishes:
  “encodable under V2 budget” vs “still overflow under 125-byte budget”.
- Printed face counts and decoded TileOp counts agree exactly.

#### `tile-validator/measure_uncapped.py`

Exact changes:

- Replace local duplicated pipeline with `process_tile()` plus one uncapped measurement path in shared code.
- Change the measurement target:
  before: “true max face ports ignoring the 16-port cap”.
  after: “true packed-byte usage and V2 envelope status”.
- Keep per-face counts, but add:
  `packed_bytes_used = o+i+2l+2r`
  `packed_bytes_slack = 125 - packed_bytes_used`
  `would_fit_v2`
  `overflow_reason`

Verification gate:

- The script can show tiles with `max_face_ports > 16` but `would_fit_v2 = true`.
- Output distribution includes byte-budget percentiles, not just face-port percentiles.

#### `tile-validator/group_stats.py`

Exact changes:

- Replace local duplicated pipeline with shared helpers.
- Preserve group taxonomy analysis, but stop classifying overflow based on `MAX_PORTS_PER_FACE = 16`.
- Add V2 envelope metrics per tile:
  face counts,
  packed bytes,
  slack,
  overflow reason.

Verification gate:

- Group taxonomy counts remain stable relative to old output for the same tiles.
- Tile overflow classification changes only where V1 cap and V2 budget differ.

### 7. Add a shared classification helper for pruning/measurement

Directive:
Move group-incidence and dead-end classification out of the standalone scripts into shared code, preferably `ports.py` or a small new `analysis.py`.

Functions to centralize:

- group-face incidence
- dead-end detection
- surviving-group classification: `dead_end`, `single_face_multi_port`, `multi_face`
- packed-byte budget calculation

Why:

- `diagnose_tile.py`, `measure_uncapped.py`, `group_stats.py`, and `pruning_analysis.py` all need the same classification logic.

Verification gate:

- `pruning_analysis.py` and `group_stats.py` report identical category counts for the same tile.

### 8. Update `cross_validate.py` to validate V2 bytes, not just V1-era first-32-byte prefixes

Directive:
Make cross-validation prove the C++ encoder matches Python’s V2 encoder end to end.

Exact changes:

- `tile-validator/cross_validate.py:47-60` `run_cpp(...)`
  Extend parsing so it can ingest the full 128-byte TileOp from C++ output. If the C++ binary only prints the first 32 bytes today, the executor must first extend that binary’s text output format or add a machine-readable output mode. This is a dependency, not optional.
- `tile-validator/cross_validate.py:62-72` `run_python(...)`
  Add parsed TileOp metadata to the Python result so comparisons can include offsets and per-face slices.
- `tile-validator/cross_validate.py:120-140`
  Replace “first 32 bytes match” with:
  full 128-byte equality for normal/overflow tiles,
  parsed offset/count equality,
  face-group and decoded raw-h1 equality,
  status equality (`normal/dead/overflow`).

Verification gate:

- A passing run means byte-for-byte identity of the full TileOp, not just summary metrics.
- Mismatch output points to the failing layer: summary counts, header bytes, section slices, or overflow semantics.

### 9. Update user-facing sample scripts to operating-point V2 checks

Directive:
Keep sample scripts, but make them prove V2 properties instead of legacy comparisons.

Exact changes:

- `tile-validator/sample.py`
  Keep the legacy-vs-fixed comparison if it is still useful historically, but add V2 header/count printing and remove any implied correctness criterion based on the old 16-port cap.
- `tile-validator/expanded_sample.py`
  Add packed-byte usage and V2 fit/overflow status to its output.
- `tile-validator/pruning_analysis.py`
  Add packed-byte usage and surviving-group classifications from shared helpers.

Verification gate:

- Every sample report includes enough data to explain why a tile does or does not fit inside the V2 budget.

## New Measurement Scripts Required

### A. Multi-radius V2 census

Directive:
Add a new script, likely `tile-validator/measure_v2_envelope.py`.

Purpose:

- Sample operating-point tiles at multiple radii, not just `R = 860M`.
- Measure V2 fit rate as a function of radius and angle.

Inputs:

- Radii: at minimum three operating radii `>= 800M`
- Angles: at least `30°` and `45°`
- Tile-aligned off-axis coordinates only

Outputs per tile:

- `a_lo`, `b_lo`, radius, angle
- face counts
- total groups after pruning
- `packed_bytes_used`
- `packed_bytes_slack`
- `overflow`
- `overflow_reason`
- `max_face_ports`

Verification gate:

- The script reproduces the known fact that many tiles exceed 16 ports on one face, while most still fit the V2 125-byte budget.

### B. V2 envelope coverage stats

Directive:
Add a second script, likely `tile-validator/v2_coverage_report.py`.

Purpose:

- Aggregate outputs from the multi-radius census into percentiles and coverage rates.

Required summary metrics:

- fit rate by radius
- fit rate by angle
- p50/p95/p99 of `packed_bytes_used`
- frequency of each overflow reason
- p95/p99 of per-face counts under V2

Verification gate:

- Report answers the specific architecture question: whether `125` bytes remains sufficient across operating radii, not just at one stripe near `860M`.

## Order Of Operations

1. Build the shared TileOp v2 parser module.
   Gate: decoder can parse a Python-emitted alive tile, dead tile, and overflow tile correctly.
2. Rewrite `ports.encode_tileop()` to emit V2 bytes.
   Gate: encode-decode round-trip matches input ports on known tiles.
3. Repoint `compose.py` and `validate.py` to the shared parser.
   Gate: validator catches malformed V2 headers and composition handles >16-face-count tiles.
4. Enrich `process_tile()` result shape.
   Gate: downstream scripts no longer need their own decode logic.
5. Collapse `diagnose_tile.py`, `measure_uncapped.py`, and `group_stats.py` onto shared helpers.
   Gate: no duplicated sieve/UF/face extraction code remains in those scripts.
6. Upgrade `cross_validate.py` to compare full 128-byte TileOps.
   Gate: full-byte equality against C++ on operating-point samples.
7. Add multi-radius V2 measurement scripts.
   Gate: outputs include packed-budget stats and overflow-reason breakdowns.

Dependency note:

- Step 6 may require a small C++ output-format change before the Python side can prove full-byte equality. That is a blocking dependency for the final cross-validation gate.

## Risks

1. **Face-parity decode drift.**
   If executors compute face parity differently across `ports.py`, `compose.py`, and `cross_validate.py`, Python will fork the spec even though the packed bytes look plausible.
2. **False confidence from old census numbers.**
   The 2026-04-09 census doc recommends a wider fixed layout. That recommendation predates the current V2 packed layout. Executors must not import its “24 ports/face” recommendation as today’s design target.
3. **C++ output visibility gap.**
   If `run_tile` still exposes only summary metrics or a prefix of bytes, Python cannot prove byte-exact equivalence. That dependency must be handled explicitly.
4. **Script drift if duplicate logic survives.**
   Leaving even one standalone pipeline copy will recreate the current mismatch later.

## Final Verification Gate

The rework is done only when all of the following are true:

1. Python emits and decodes TileOp v2 bytes using one shared layout module.
2. `validate.py` enforces V2 structural invariants instead of V1 reserved-tail assumptions.
3. `diagnose_tile.py`, `measure_uncapped.py`, and `group_stats.py` no longer duplicate the tile pipeline.
4. Cross-validation compares full 128-byte TileOps against C++ at operating-point coordinates only.
5. A new multi-radius measurement path reports V2 packed-budget coverage, not just uncapped face-port counts.
