---
title: Tile Validator Alignment Plan
date: 2026-04-09
engine: codex
type: analysis
status: complete
refs: [docs/tile_spec.md, docs/tile_operations.md, tile-validator/]
---

## Scope

This plan compares the Python validator in `tile-validator/` against the authoritative tile specs in `docs/tile_spec.md` and `docs/tile_operations.md`, with `compose.py` checked where it still encodes tile-matching assumptions. The authority chain is specs over code.

## Pipeline Status Matrix

| Phase | Spec reference | Python status | Alignment summary |
|---|---|---|---|
| Sieve | `tile_operations.md` S4, `tile_spec.md` S2/S11 | Partial | Correct halo size and Euclidean adjacency constant, but Python brute-forces Gaussian primality over all 271x271 points instead of the spec row-sieve + deterministic confirmation pipeline. |
| Compact | `tile_operations.md` S5 | Missing | No bitmap, no prefix-popcount table, no dense `prime_pos` array. |
| Union-find | `tile_operations.md` S6 | Partial | Connectivity threshold matches `k=40`, but UF policy and outputs do not match spec. |
| Face extract | `tile_operations.md` S7, `tile_spec.md` S3 | Divergent | Python extracts ports from a symmetric `abs(distance) <= collar` boundary zone and clusters by full 2D adjacency, not the spec’s in-tile collar scan and spec-defined port semantics. |
| Encode | `tile_operations.md` S8, `tile_spec.md` S4 | Missing | No 128-byte TileOp, no zero-sentinel packing, no overflow sentinel, no reserved bytes. |

## Work Items

### [priority: critical] Restrict face extraction to in-tile collar, not the shared halo
- **Spec says:** `tile_operations.md` S3 says halo primes "are NOT extracted as face primes"; S7.1 defines face primes as points "inside the tile proper" and within collar distance of a boundary.
- **Python does:** [`tile-validator/ports.py:45`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/ports.py#L45) extracts face points with `abs(...) <= COLLAR` around each boundary, so `I` includes `y in [ty-7, ty+7]`, `O` includes `y in [ty+S-7, ty+S+7]`, `L` includes `x in [tx-7, tx+7]`, and `R` includes `x in [tx+S-7, tx+S+7]`.
- **Gap:** The spec keeps halo primes in Phase 3 connectivity only; Python promotes halo primes into Phase 4 ports. That changes which ports exist, which groups survive pruning, and which shared-face matches are possible.
- **Fix:** Rewrite `_face_primes()` around sieve-domain coordinates or tile-relative coordinates so membership is `in_tile` plus `tile_row < COLLAR`, `tile_row >= S-COLLAR`, `tile_col < COLLAR`, `tile_col >= S-COLLAR` exactly as in `tile_operations.md` S7.1.
- **Risk if unfixed:** The validator will disagree with any spec-faithful C++ or CUDA implementation at the first externally visible stage: face ports.

### [priority: critical] Replace fingerprint-based face matching with spec matching rules
- **Spec says:** `tile_spec.md` S5.2 uses positional matching for aligned I/O faces; S5.3 uses `h1` equality with `delta_h` for L/R faces; S5.4 says full-fingerprint matching exists only as a diagnostic verification mode.
- **Python does:** [`tile-validator/compose.py:50`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/compose.py#L50) and [`tile-validator/compose.py:86`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/compose.py#L86) union shared-face ports by full `fingerprint == fingerprint`. [`tile-validator/validate.py:47`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/validate.py#L47) treats shared-face fingerprint equality as P1.
- **Gap:** Python’s primary composition rule is the old full-fingerprint protocol. The spec’s production rule is groups-only positional matching on I/O and `h1` matching on L/R.
- **Fix:** Keep fingerprint comparison only as an optional diagnostic check. Change normal composition helpers to match I/O faces by slot index after zero-sentinel count agreement, match L/R faces by `h1` and supplied `delta_h`, and reject aligned I/O count mismatch as a validation failure.
- **Risk if unfixed:** Python can falsely disagree with spec-faithful TileOps even when connectivity is correct, so it cannot serve as a reference validator.

### [priority: critical] Implement the actual 128-byte TileOp encode phase
- **Spec says:** `tile_spec.md` S4.1 and `tile_operations.md` S8.1 define a fixed 128-byte layout: I/O/L/R groups in bytes `0..63`, L/R `h1` in `64..95`, bytes `96..127` zero-filled.
- **Python does:** [`tile-validator/compose.py:15`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/compose.py#L15) models `TileOp` as a Python object containing `Port` lists and per-face lists; there is no byte encoding anywhere in `tile-validator/`.
- **Gap:** The validator never emits the artifact the specs define. It validates a legacy in-memory port structure instead of the wire format that C++ and CUDA must reproduce.
- **Fix:** Add an encode stage that returns both structured metadata and raw `bytes[128]`. Use raw-byte equality as the default comparison target for implementation validation.
- **Risk if unfixed:** Python may appear “correct” while still masking byte-layout, sentinel, padding, and ordering bugs in C++/CUDA.

### [priority: critical] Add dead-end pruning to the main extraction/encode path
- **Spec says:** `tile_spec.md` S4.5 and `tile_operations.md` S7.4 require pruning before emit: omit a group touching exactly one face with exactly one port on that face.
- **Python does:** [`tile-validator/ports.py:124`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/ports.py#L124) returns all extracted ports with no pruning. Pruning exists only as analysis tooling in [`tile-validator/pruning_analysis.py:131`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/pruning_analysis.py#L131).
- **Gap:** The validator’s main pipeline and its composition helpers operate on unpruned ports, while the spec defines TileOp contents after pruning.
- **Fix:** Move pruning into the canonical tile-processing path, before port-count checks and byte encoding. Keep `pruning_analysis.py` as a report utility around the same shared implementation.
- **Risk if unfixed:** Port counts, group counts, overflow behavior, and composition results will all diverge from spec outputs.

### [priority: critical] Reserve group `0` as empty sentinel and start labels at `1`
- **Spec says:** `tile_spec.md` S4.1 and S4.4 reserve group `0` for empty slots; first real group is `1`.
- **Python does:** [`tile-validator/ports.py:138`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/ports.py#L138) starts `next_group = 0`; [`tile-validator/pruning_analysis.py:140`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/pruning_analysis.py#L140) renumbers surviving groups back to `0..N-1`.
- **Gap:** Python group IDs collide with the spec’s empty-slot sentinel. That is harmless only because Python never encodes the fixed TileOp layout.
- **Fix:** Change canonical group numbering to `1..255` and keep `0` exclusively for absent ports. Ensure post-pruning renumbering preserves that convention.
- **Risk if unfixed:** Any eventual encoder will be wrong by construction, and empty-slot handling cannot be validated.

### [priority: critical] Implement overflow-sentinel handling exactly as specified
- **Spec says:** `tile_spec.md` S4.6 says if any face exceeds 16 ports after pruning, the kernel writes `0xFF` to all 128 bytes; `tile_operations.md` S8.2 requires overflow signaling when a face exceeds 16 ports or groups exceed `255`.
- **Python does:** There is no overflow logic in the main pipeline. `TileOp.face_counts` in [`tile-validator/compose.py:24`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/compose.py#L24) just returns list lengths, and no encoder checks slot limits.
- **Gap:** Python cannot model the spec’s failure-safe behavior for rare wide tiles.
- **Fix:** After pruning, check `max ports/face > 16` and `group_count > 255`; if triggered, emit all-`0xFF` bytes and route composition through the conservative-bridge path.
- **Risk if unfixed:** The validator will disagree exactly on the cases where the spec uses sentinels to preserve correctness.

### [priority: high] Replace brute-force primality enumeration with the spec’s sieve contract
- **Spec says:** `tile_operations.md` S4 defines a five-step row sieve: parity elimination, split-prime sieve, inert-prime sieve, small-norm rescue, and deterministic Miller-Rabin confirmation.
- **Python does:** [`tile-validator/tile.py:22`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/tile.py#L22) loops over every lattice point in the 271x271 region and calls [`tile-validator/primes.py:25`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/primes.py#L25) `is_gaussian_prime()`.
- **Gap:** Python computes the right mathematical set of primes in many cases, but not via the spec’s phase boundaries or intermediate outputs. It cannot validate the C++/CUDA sieve phase step-for-step.
- **Fix:** Rework Phase 1 to emit the spec bitmap and expose intermediate artifacts needed for comparison. Keep a slow direct-primality path only as a debug cross-check.
- **Risk if unfixed:** The validator can only validate final prime sets, not the prescribed phase contract or any sieve/bitmap bugs in lower-level implementations.

### [priority: high] Make primality confirmation deterministic and spec-bounded
- **Spec says:** `tile_operations.md` S4.3 requires deterministic Miller-Rabin at this norm range, with fixed witness sets and no probabilistic error.
- **Python does:** [`tile-validator/primes.py:12`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/primes.py#L12) prefers `gmpy2.is_prime`, described in the file header as "probabilistic primality"; only the `sympy` fallback is exact.
- **Gap:** The validator’s default backend is not guaranteed to follow the spec’s deterministic primality contract.
- **Fix:** Implement the spec witness sets directly in Python and use that path for validator output. Optional third-party libraries can remain as performance checks, not as ground truth.
- **Risk if unfixed:** Rare backend-dependent disagreements would undermine the validator exactly where it is supposed to be authoritative.

### [priority: high] Add the missing compact phase outputs
- **Spec says:** `tile_operations.md` S5 requires a packed bitmap, exclusive popcount prefix table, `prime_count`, and dense `prime_pos[]`.
- **Python does:** No module in `tile-validator/` builds a bitmap or prefix table; [`tile-validator/ports.py:172`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/ports.py#L172) runs UF directly over a sorted list of prime tuples.
- **Gap:** Phase 2 is absent, so Phase 3 is not validating the same data model as the spec.
- **Fix:** Introduce a canonical phase API such as `sieve_tile_bitmap() -> bitmap`, `compact_bitmap() -> (prime_count, prefix, prime_pos)`, then build UF from those products.
- **Risk if unfixed:** C++/CUDA compaction errors cannot be isolated or compared against Python.

### [priority: high] Align union-find policy with the spec’s deterministic smaller-root rule
- **Spec says:** `tile_operations.md` S6.2 uses no rank array and makes the smaller-index root the parent; S6.4 then flattens all paths.
- **Python does:** [`tile-validator/uf.py:1`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/uf.py#L1) implements union by rank; [`tile-validator/ports.py:172`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/ports.py#L172) uses that UF both for full-tile components and face-port clustering.
- **Gap:** Component representatives are produced by a different algorithm than the spec. That does not necessarily change connectivity partitions, but it does break phase-level equivalence and deterministic representative expectations.
- **Fix:** Replace the validator UF with a spec-faithful implementation for tile processing. If a faster UF is kept for experiments, isolate it from canonical outputs.
- **Risk if unfixed:** Phase-3 comparisons will be noisy, and representative-dependent debugging against C++/CUDA will be harder than necessary.

### [priority: high] Rework port clustering around the spec’s face semantics
- **Spec says:** `tile_spec.md` S3 defines a port as a maximal cluster of face primes ordered by along-face position, with same-port iff consecutive face-prime gap is `<= 6`; `tile_operations.md` S7.2 expresses clustering as sorted-by-`h` face primes with `distance^2 <= k`.
- **Python does:** [`tile-validator/ports.py:85`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/ports.py#L85) builds a full 2D adjacency graph over face-zone primes and takes connected components, independent of sorted consecutive order.
- **Gap:** Python currently implements “face-zone connected components,” not the spec’s port-construction rule. It can merge or split ports differently whenever same-`h`, diagonal, or depth-chain connections matter.
- **Fix:** After correcting face membership, sort face primes by `h` and implement the exact port rule chosen by the specs. Because `tile_spec.md` S3 and `tile_operations.md` S7.2 describe the rule differently, this item should begin by resolving that spec wording into one canonical algorithm before editing Python.
- **Risk if unfixed:** Port counts, anchors, group assignments, and matching behavior will remain unstable relative to the specs.

### [priority: high] Fix coordinate-system drift and misleading face documentation
- **Spec says:** `tile_spec.md` S2.1 uses the origin corner `(min-x, min-y)` and maps `I` to the bottom edge and `O` to the top edge.
- **Python does:** [`tile-validator/tile.py:25`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/tile.py#L25) calls `(tx, ty)` the "top-left corner"; [`tile-validator/ports.py:10`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/ports.py#L10) says `Top (I)` and `Bottom (O)`.
- **Gap:** The implementation mostly uses `x - tx` and `y - ty` as if `(tx, ty)` were the minimum corner, but the docs and face labels still describe the pre-spec orientation.
- **Fix:** Rename tile coordinates to `a_lo`, `b_lo` or `x_lo`, `y_lo`, update comments and docstrings to the min-corner convention, and make face naming consistent with spec language everywhere.
- **Risk if unfixed:** Future edits will keep reintroducing sign, boundary, and neighbor-direction bugs because the code comments point in the wrong direction.

### [priority: medium] Separate canonical TileOp validation from diagnostic fingerprint tooling
- **Spec says:** `tile_spec.md` S5.4 explicitly permits a full-fingerprint verification mode in the Python validator.
- **Python does:** [`tile-validator/ports.py:30`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/ports.py#L30) makes `(h1,h2,d1,d2)` part of the core `Port`; [`tile-validator/validate.py:167`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/validate.py#L167) treats fingerprint uniqueness as a primary property.
- **Gap:** Diagnostic fingerprint data is mixed into the canonical pipeline, which made it easy for the old matching rule to become the default.
- **Fix:** Keep fingerprint fields in a debug-only structure or optional trace output. Canonical tile outputs should be spec TileOps plus optional intermediate phase dumps.
- **Risk if unfixed:** The validator will continue to optimize around the wrong surface area.

### [priority: medium] Update composition helpers to validate the encoded TileOp, not `Port` objects
- **Spec says:** `tile_spec.md` S6 composes TileOps after encoding; the compositor reads group arrays and `h1` arrays, not raw prime lists.
- **Python does:** [`tile-validator/compose.py:32`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/compose.py#L32) composes two `TileOp` wrapper objects that still contain raw `Port` instances.
- **Gap:** The current composition path bypasses the serialized representation and therefore cannot catch byte-order, padding, or count-sentinel bugs.
- **Fix:** Add a decode or accessor layer over the 128-byte TileOp and run composition from encoded bytes by default.
- **Risk if unfixed:** Python will miss exactly the class of bugs that usually appear during C++/CUDA porting: packing and decoding bugs.

### [priority: medium] Replace old validation properties with spec-facing checks
- **Spec says:** `tile_spec.md` S4-S6 and `tile_operations.md` S4-S8 define byte layout, deterministic group numbering, dead-end pruning, and matching rules as the observable contract.
- **Python does:** [`tile-validator/validate.py:1`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/validate.py#L1) centers the suite on fingerprint determinism and uniqueness rather than encoded TileOps and phase outputs.
- **Gap:** The property suite reinforces the legacy model instead of the spec model.
- **Fix:** Redefine the validator test surface around bitmap agreement, compact agreement, component partition agreement, encoded TileOp byte agreement, and spec matching agreement on shared faces.
- **Risk if unfixed:** Regressions against the actual spec can slip through while legacy fingerprint properties still pass.

### [priority: low] Remove or quarantine non-operating-point sample targets from canonical validation scripts
- **Spec says:** `tile_operations.md` S10.2 still lists small test tiles, but project instructions in `AGENTS.md` require validator testing at `R >= 800M`.
- **Python does:** [`tile-validator/sample.py:116`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/sample.py#L116), [`tile-validator/expanded_sample.py:40`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/expanded_sample.py#L40), and [`tile-validator/pruning_analysis.py:194`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/pruning_analysis.py#L194) still center small and medium radii.
- **Gap:** The shipped scripts do not reflect the project’s current operating-point validation policy.
- **Fix:** Keep tiny-radius cases only as local debugging fixtures and move canonical reports to off-axis `R >= 800M` coordinates aligned to `256`.
- **Risk if unfixed:** The validator will keep producing comforting but low-value evidence.

## Things The Python Code Already Does Correctly

- `tile-validator/tile.py` derives `HALO = S + 2*COLLAR + 1 = 271`, matching `tile_spec.md` S2 and `tile_operations.md` S3.
- `tile-validator/tile.py` uses Euclidean `dx*dx + dy*dy <= K` adjacency in [`tile-validator/tile.py:51`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/tile.py#L51), matching `tile_spec.md` S2 and `tile_operations.md` S6.1.
- `tile-validator/ports.py` scans faces in `("I", "O", "L", "R")` at [`tile-validator/ports.py:27`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/ports.py#L27), which matches the group-label first-appearance order required by `tile_spec.md` S4.4 and `tile_operations.md` S7.3.
- `tile-validator/pruning_analysis.py` uses the correct dead-end criterion in [`tile-validator/pruning_analysis.py:68`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/pruning_analysis.py#L68), matching `tile_spec.md` S4.5 and `tile_operations.md` S7.4, even though that logic is not yet part of the canonical pipeline.

## Spec Gaps Or Ambiguities To Flag

- `tile_spec.md` S3 defines ports by consecutive along-face separation `<= 6`, while `tile_operations.md` S7.2 describes clustering sorted face primes by `distance^2 <= k`. Python should not be changed until one canonical face-clustering rule is declared for implementation.
- `tile_spec.md` S4.6 says overflow writes `0xFF` to all 128 bytes, while `tile_operations.md` S8.2 only states `groups[0] = 0xFF`. The tile spec is stricter and should probably govern, but the encode implementation should confirm that explicitly.
- The specs define final tile outputs well, but they do not yet define a canonical validator API for exposing intermediate artifacts such as bitmap words, prefix arrays, and canonicalized component partitions. Adding that would make Python-vs-C++ phase diffs much cleaner.

## Constants And Magic Numbers To Derive From Spec Parameters

- `tile-validator/tile.py:16` hard-codes `S = 256`; derive from a single spec-parameter module shared by all validator phases.
- `tile-validator/tile.py:18` hard-codes `COLLAR = 7`; derive from `ceil(sqrt(k))` or at least centralize it with a spec assertion against `k = 40`.
- `tile-validator/tile.py:19` derives `HALO` correctly but only locally; export it as the canonical `SIDE_EXP`.
- `tile-validator/compose.py:53`, [`tile-validator/compose.py:89`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/compose.py#L89), and several validation loops depend on implicit face widths and unbounded port counts instead of `MAX_PORTS_PER_FACE = 16`.
- `tile-validator/ports.py:139` and [`tile-validator/pruning_analysis.py:142`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/pruning_analysis.py#L142) embed legacy `0`-based group numbering; replace with spec constants `GROUP_EMPTY = 0` and `GROUP_LABEL_MIN = 1`.
- [`tile-validator/expanded_sample.py:326`](/Users/otonashi/thinking/building/gaussian-moat-cuda/tiles-maxxing/tile-validator/expanded_sample.py#L326) hard-codes a non-spec alternative `K=36`; keep it only as a what-if experiment, not in canonical validator output.

## Recommended Execution Order

1. Normalize coordinate conventions and phase APIs.
2. Implement spec-faithful Phase 1 and Phase 2 outputs: bitmap, prefix table, `prime_pos`.
3. Replace UF with the spec algorithm and rebuild Phase 3 on compacted data.
4. Rewrite face extraction and port clustering from the corrected phase outputs.
5. Add dead-end pruning, `1`-based group numbering, and overflow handling.
6. Emit real 128-byte TileOps and move composition to encoded-byte inputs.
7. Demote fingerprint checks to optional diagnostics and rewrite the validation suite around spec outputs.

## Bottom Line

The biggest blockers are not performance shortcuts; they are semantic mismatches. Until the validator stops extracting halo primes as ports, stops matching by full fingerprints, and starts emitting the actual 128-byte TileOp after pruning, it cannot serve as a spec-faithful reference for C++ or CUDA.
