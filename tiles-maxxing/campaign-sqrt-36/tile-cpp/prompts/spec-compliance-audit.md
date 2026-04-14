# Spec Compliance Audit: C++ tile-cpp vs All Specifications

## Your Role
You are a spec compliance auditor. Your job is adversarial: find every place where C++ code deviates from spec, no matter how small. Assume the code is guilty until proven innocent. A "looks fine" verdict requires line-by-line evidence.

## Scope
Audit the entire C++ implementation in `tile-cpp/` against all four canonical specs:
1. `docs/tile_spec.md` — tile geometry, TileOp format, port definition, matching rules
2. `docs/tile_operations.md` — 5-phase pipeline contract, phase-by-phase requirements
3. `docs/compositor_spec.md` — composition semantics, face matching, overflow behavior
4. `docs/grid_spec.md` — grid layout (only relevant for constants the tile kernel uses)

Authority chain: Specs > Code. If code contradicts spec, that's a finding.

## Audit Checklist

For each item below, read the relevant spec section AND the C++ code, then verdict: PASS / FAIL / AMBIGUOUS.

### 1. Tile Geometry
- [ ] TILE_SIDE = 256, TILE_POINTS = 257 (constants.h vs tile_spec.md S2)
- [ ] Tile proper is closed [0, S] x [0, S] = 257x257 (face_extract.cpp in-tile check)
- [ ] Sieve domain is 271x271 = (S+1+2*collar)^2 (constants.h SIDE_EXP)
- [ ] COLLAR = 7 = ceil(sqrt(40)) (constants.h vs tile_spec.md)
- [ ] Adjacent tiles share boundary row/column at offset S=256

### 2. Sieve Phase (Phase 1)
- [ ] Gaussian primality definition matches spec (sieve.cpp is_gaussian_prime_point)
  - Off-axis: a^2+b^2 is rational prime
  - On-axis (a=0): |b| is prime AND |b| mod 4 == 3
  - On-axis (b=0): |a| is prime AND |a| mod 4 == 3
- [ ] Miller-Rabin witnesses are sufficient for norms up to ~1.7e18 (R=850M, norm=a^2+b^2)
- [ ] Split prime table: all p < 10000 with p = 1 mod 4, count = 609
- [ ] Inert prime table: all p < 10000 with p = 3 mod 4, count = 619
- [ ] No integer overflow in norm computation (check __int128 usage)
- [ ] Bitmap indexing: bit = row * SIDE_EXP + col, word = bit/32, mask = 1 << (bit%32)

### 3. Compact Phase (Phase 2)
- [ ] Prefix table is exclusive prefix sum of popcount
- [ ] Prime positions extracted in order (low bit to high bit, word 0 to word N)
- [ ] bitmap_pos_to_uf_index correctly maps bitmap position to dense index

### 4. Union-Find Phase (Phase 3)
- [ ] Backward offsets: exactly 64 entries, all (dr,dc) with dr^2+dc^2 <= 40, dr<0 or (dr==0 && dc<0)
- [ ] Path halving (not path splitting, not full compression)
- [ ] Final flatten pass: every parent[i] points to root
- [ ] No race conditions in sequential processing (not relevant for CPU but check for CUDA-readiness)

### 5. Face Extraction Phase (Phase 4)
- [ ] Face I: tile_row < COLLAR (rows 0..6), h = tile_col
- [ ] Face O: tile_row >= TILE_SIDE - COLLAR + 1 (rows 250..256), h = tile_col
- [ ] Face L: tile_col < COLLAR (cols 0..6), h = tile_row
- [ ] Face R: tile_col >= TILE_SIDE - COLLAR + 1 (cols 250..256), h = tile_row
- [ ] Only in-tile primes extracted (0 <= tile_row <= TILE_SIDE, 0 <= tile_col <= TILE_SIDE)
- [ ] Halo-only primes excluded from faces
- [ ] Sort order: (h, boundary_depth, tile_row, tile_col) — verify boundary_depth definition per face
- [ ] Corner primes: primes in corners belong to TWO faces — verify they appear on both

### 6. Port Clustering (Phase 4 continued)
- [ ] Consecutive-pair comparison: dist_sq(primes[i], primes[i-1]) > K_SQ starts new port
- [ ] Distance uses tile_col/tile_row (NOT h/depth, NOT absolute coords)
- [ ] K_SQ = 40 consistently
- [ ] Port h1 = h of first prime in cluster (minimum h in sorted order)
- [ ] Component root taken from first prime in cluster via UF parent lookup

### 7. Group Assignment
- [ ] Groups numbered 1, 2, 3, ... in order of first appearance across faces I, O, L, R
- [ ] Group 0 reserved (empty slot sentinel)
- [ ] root_to_group mapping is global across all faces (same component = same group)
- [ ] Group numbering is deterministic

### 8. Dead-End Pruning (Phase 5a)
- [ ] Dead-end = group with exactly 1 port on exactly 1 face
- [ ] Dead-end groups removed entirely
- [ ] Surviving groups renumbered 1..N contiguously
- [ ] Port order preserved within each face after pruning

### 9. TileOp Encoding (Phase 5b)
- [ ] Layout matches spec:
  - bytes[0..15]: Face I groups
  - bytes[16..31]: Face O groups
  - bytes[32..47]: Face L groups
  - bytes[48..63]: Face R groups
  - bytes[64..79]: Face L h1 values
  - bytes[80..95]: Face R h1 values
  - bytes[96..127]: zero padding
- [ ] Overflow trigger: any face > 16 ports OR group_count >= 255 OR h1 > 255
- [ ] Overflow action: ALL 128 bytes set to 0xFF
- [ ] h1 stored only for L/R faces (NOT I/O) — verify I/O h1 is NOT written
- [ ] Unused slots zero-filled
- [ ] Port ordering within face: ascending h1

### 10. Constants Cross-Check
- [ ] Every constant in constants.h matches its spec definition
- [ ] MAX_PRIMES, MAX_PORTS bounds are safe (won't overflow at R=850M)
- [ ] OVERFLOW_SENTINEL = 0xFF
- [ ] NUM_FACES = 4, FACE_I=0, FACE_O=1, FACE_L=2, FACE_R=3

### 11. Edge Cases
- [ ] What happens when a face has 0 primes? (empty face)
- [ ] What happens when all groups are dead-ends? (0 surviving groups)
- [ ] What happens when h1 = 256 (shared boundary)? Should trigger overflow
- [ ] What happens at origin (a_lo=0, b_lo=0)? On-axis primality
- [ ] What happens when two primes on the same face have identical (h, depth)?

## Output Format

Write your audit to `docs/supportive/2026-04-09-cpp-spec-compliance-audit.md` with this structure:

```
## Summary
X PASS / Y FAIL / Z AMBIGUOUS out of N checks

## Findings (FAIL and AMBIGUOUS only)
For each finding:
- Spec reference (file, section, exact quote)
- Code reference (file, line number, exact code)
- What the spec says vs what the code does
- Severity: CRITICAL (affects correctness) / MODERATE (edge case) / LOW (style/clarity)
- Suggested fix

## Full Audit Trail
Every checklist item with verdict and evidence (spec quote + code line)
```

Also print a summary (under 30 lines) to stdout: how many pass/fail/ambiguous, and list every FAIL with one-line description.

## IMPORTANT
- Read EVERY spec file cover to cover. Do not skip sections.
- Read EVERY C++ source and header file. Do not skip files.
- When a spec section is ambiguous, mark it AMBIGUOUS and quote both possible interpretations.
- Do not assume the code is correct. The spec is the authority.
- Pay special attention to off-by-one errors, boundary conditions, and overflow handling.
- Check that the C++ distance computation in clustering uses the SAME coordinates as the spec defines.
