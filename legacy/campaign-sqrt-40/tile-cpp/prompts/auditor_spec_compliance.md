# Audit: tile-cpp Implementation vs Spec Compliance

You are auditing a C++ implementation of a Gaussian moat tile processor against
its specification. Your job is to find bugs, spec violations, and correctness
issues — not style nits. Be thorough, adversarial, and precise.

## Spec (source of truth)

Read this file completely — it is the authoritative algorithm description:
- `tiles-maxxing/docs/tile_operations.md`

## Implementation under audit

All files in `tiles-maxxing/tile-cpp/`:
- `include/constants.h` — compile-time constants
- `include/types.h` — shared data structures
- `include/sieve.h` + `src/sieve.cpp` — Phase 1: row sieve (modular arith, MR, Cornacchia, sieve table init, per-row sieve)
- `include/compact.h` + `src/compact.cpp` — Phase 2: prefix popcount + bit extraction
- `include/union_find.h` + `src/union_find.cpp` — Phase 3: backward offsets + UF + component building
- `include/face_extract.h` + `src/face_extract.cpp` — Phase 4: face primes, port clustering, group assignment
- `include/encode.h` + `src/encode.cpp` — Phase 5: dead-end pruning + TileOp packing
- `include/process_tile.h` + `src/process_tile.cpp` — end-to-end wiring

## Python reference (secondary check)

The Python validator is the prior reference implementation:
- `tiles-maxxing/tile-validator/tile.py` — brute-force sieve
- `tiles-maxxing/tile-validator/uf.py` — union-find (NOTE: uses rank-based, spec says NO rank)
- `tiles-maxxing/tile-validator/ports.py` — face extraction and port clustering
- `tiles-maxxing/tile-validator/primes.py` — Gaussian primality
- `tiles-maxxing/tile-validator/pruning_analysis.py` — dead-end pruning

## Existing CUDA headers (algorithmic reference)

The proven CUDA codebase that the C++ reimplements:
- `src/modular_arith.cuh` — mulmod, powmod, Montgomery
- `src/miller_rabin.cuh` — deterministic MR with tiered witnesses
- `src/cornacchia.cuh` — fast_sqrt_neg1, tonelli_shanks
- `src/row_sieve.cuh` — row sieve kernel (the production algorithm)
- `src/tile_kernel.cuh` — Gaussian primality helpers

## Audit Checklist

For EACH phase, read the implementation AND the corresponding spec section,
then report:

### Phase 1: Sieve (spec Sections 2, 3, 4)
- [ ] Constants match: SIDE_EXP=271, SIEVE_LIMIT=10000, 609 split / 619 inert primes
- [ ] Sieve table packing: `(root << 16) | p` where root = min(r, p-r)
- [ ] Euclidean mod used (not C++ %) for negative values
- [ ] Parity elimination: `(a ^ b) & 1 == 0` → composite
- [ ] Split sieve: computes residue = `(euclidean_mod(a, p) * r) % p`, marks BOTH +residue and -residue classes
- [ ] Inert sieve: only when `euclidean_mod(a, p) == 0`, marks `b ≡ 0 mod p`
- [ ] Small-norm rescue: if `|a| <= 100`, un-marks false positives where norm is actually prime
- [ ] MR confirmation: axis points use special rule (`|coord| ≡ 3 mod 4` and is_prime)
- [ ] MR witnesses match spec: tiered {2,3,5}, {2,3,5,7}, {2,3,5,7,11}
- [ ] Trial division by primes up to 97 before MR
- [ ] Output bitmap: 1 = prime, 0 = not prime (correct polarity)
- [ ] Bitmap bit layout: position = row * SIDE_EXP + col

### Phase 2: Compact (spec Section 5)
- [ ] Prefix popcount is EXCLUSIVE prefix sum
- [ ] Bit extraction uses ctz + clear-lowest-bit pattern
- [ ] prime_pos stores flat bitmap positions as uint32_t (not u16)
- [ ] bitmap_pos_to_uf_index correctly uses prefix table + popcount mask
- [ ] Final bitmap word masked to prevent out-of-range bits

### Phase 3: Union-Find (spec Section 6)
- [ ] NO rank array — spec is explicit: smaller-index-wins, no rank
- [ ] Path HALVING (not full compression): `parent[x] = parent[parent[x]]`
- [ ] Backward offsets: exactly 64 offsets, boundary INCLUSIVE (dr²+dc² <= 40)
- [ ] Offset enumeration: dr < 0 OR (dr == 0 AND dc < 0)
- [ ] Bounds checking on neighbor positions (0 <= nr < SIDE_EXP, 0 <= nc < SIDE_EXP)
- [ ] Flatten pass after all unions: parent[i] = find(i) for all i

### Phase 4: Face Extract (spec Section 7)
- [ ] Face membership: INSIDE tile proper (not halo), within COLLAR of boundary
- [ ] Face I: tile_row < COLLAR (rows 0..6)
- [ ] Face O: tile_row >= TILE_SIDE - COLLAR (rows 249..255)
- [ ] Face L: tile_col < COLLAR (cols 0..6)
- [ ] Face R: tile_col >= TILE_SIDE - COLLAR (cols 249..255)
- [ ] Corner primes belong to MULTIPLE faces
- [ ] Port clustering uses FULL 2D distance (not just along-face)
- [ ] h coordinate: I/O → tile_col, L/R → tile_row
- [ ] Group assignment: 1-based, sequential in I→O→L→R order, ports sorted by ascending h
- [ ] Group comes from parent[uf_index] (interior component root)

### Phase 5: Encode (spec Section 8)
- [ ] Dead-end pruning: group on exactly 1 face with exactly 1 port → pruned
- [ ] Group renumbering after pruning: contiguous 1..N
- [ ] TileOp byte layout:
  - Bytes 0-15: Face I groups [u8; 16]
  - Bytes 16-31: Face O groups [u8; 16]
  - Bytes 32-47: Face L groups [u8; 16]
  - Bytes 48-63: Face R groups [u8; 16]
  - Bytes 64-79: Face L h1 offsets [u8; 16]
  - Bytes 80-95: Face R h1 offsets [u8; 16]
  - Bytes 96-127: Reserved (zero)
- [ ] Ports sorted by ascending h within each face
- [ ] Zero-padded to 16 slots
- [ ] Overflow sentinel: groups[0] = 0xFF if >16 ports on a face
- [ ] h1 = min along-face coordinate of port primes

### Cross-cutting
- [ ] process_tile wiring: correct phase order, correct data passing between phases
- [ ] Memory safety: no buffer overflows on edge cases (empty tile, max primes)
- [ ] Integer overflow safety: norm computation for large coordinates

## Output Format

For each finding, report:
```
[SEVERITY] Phase N, file:line — description
  Spec says: <what the spec requires>
  Code does: <what the code actually does>
  Impact: <correctness impact>
  Suggested fix: <if applicable>
```

Severities:
- **BUG**: Will produce wrong results
- **SPEC-VIOLATION**: Diverges from spec but may not affect correctness
- **RISK**: Potential issue under certain conditions
- **NOTE**: Observation, not a bug

End with a summary: total findings by severity, overall assessment of implementation quality, and whether you'd trust this to produce correct TileOps.
