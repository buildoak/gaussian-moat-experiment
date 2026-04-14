# Audit: tile-cpp Code Faithfulness, Common Sense, and Spec Alignment

You are a second-opinion auditor reviewing a C++ implementation of a Gaussian moat
tile processor. A prior Codex audit found 2 bugs (port clustering and buffer overflow)
that are being fixed in parallel. Your job is INDEPENDENT — audit the code fresh,
looking for issues the first auditor may have missed.

Focus on three axes:
1. **Spec faithfulness** — does the code implement what the spec says?
2. **Code quality / common sense** — are there logical errors, off-by-ones, edge cases?
3. **Cross-phase consistency** — do the phases wire together correctly?

## Source of Truth

**Spec:** `tiles-maxxing/docs/tile_operations.md` — read it COMPLETELY first.

## Implementation Under Audit

All files in `tiles-maxxing/tile-cpp/`:
- `include/constants.h`, `include/types.h` — foundation
- `include/sieve.h` + `src/sieve.cpp` — Phase 1: row sieve
- `include/compact.h` + `src/compact.cpp` — Phase 2: compaction
- `include/union_find.h` + `src/union_find.cpp` — Phase 3: union-find
- `include/face_extract.h` + `src/face_extract.cpp` — Phase 4: face extraction
- `include/encode.h` + `src/encode.cpp` — Phase 5: encoding
- `include/process_tile.h` + `src/process_tile.cpp` — end-to-end wiring
- `tests/test_*.cpp` — test files

## Reference Code

- **Python validator:** `tiles-maxxing/tile-validator/*.py` — prior reference implementation
- **CUDA headers:** `src/*.cuh` — production CUDA code the C++ reimplements

## What to Look For

### Spec Faithfulness
- Does the sieve correctly implement all 5 steps (parity, split, inert, rescue, MR)?
- Does the MR use the correct witness sets and thresholds?
- Is the Cornacchia/sqrt(-1) computation correct?
- Does the UF use smaller-index-wins with path halving (NO rank)?
- Are backward offsets exactly 64, with boundary inclusive (dr²+dc² <= 40)?
- Is the face membership test correct (inside tile, within COLLAR of boundary)?
- Is the group assignment scan order correct (I→O→L→R, ascending h)?
- Is the TileOp byte layout correct per spec Section 8.1?
- Is dead-end pruning correct (1 face, 1 port → prune)?

### Common Sense / Logic Errors
- Integer overflow in norm computation for large coordinates (a²+b² when a,b ~ 800M)?
- Off-by-one errors in face boundaries, sieve domain limits, bitmap indexing
- Euclidean mod vs C++ % for negative numbers — are all negative cases handled?
- Is the sieve table packing correct (root << 16 | p, where root = min(r, p-r))?
- Are axis primes (a=0 or b=0) handled correctly in the sieve?
- Does the compact phase handle the partial last bitmap word correctly?
- Can group labels exceed 255 (u8 overflow in TileOp)?

### Cross-Phase Wiring
- Does process_tile pass the right data between phases?
- Are array sizes consistent across phase boundaries?
- Could any phase produce output that violates another phase's input assumptions?

### Edge Cases
- Tile at origin (0,0) — axis primes, special cases, high density
- Tile containing only axis points
- Tile with no primes (theoretically possible for very large coordinates)
- Tile with single face having exactly 16 ports (boundary of overflow)
- Empty face (no face primes on one side)

## Known Issues (being fixed separately)

These were found by the prior audit. Note them but don't focus on them:
- Port clustering uses all-pairs instead of consecutive-pairs (being fixed)
- MAX_PRIMES=8192 too small for (0,0) tile (being fixed to 16384)
- sieve_tile doesn't zero bitmap internally (being fixed)
- O/R face depth off-by-one (being fixed)

Look for DIFFERENT issues.

## Output Format

For each finding:
```
[SEVERITY] Phase N, file:line — description
  Expected: <what should happen>
  Actual: <what the code does>
  Impact: <correctness/safety impact>
  Fix: <suggested fix if applicable>
```

Severities: BUG, SPEC-VIOLATION, RISK, NOTE

End with:
- Summary table: findings by severity
- Overall trust assessment
- Top 3 things you'd want tested before trusting this code at the 73M-tile operating point
