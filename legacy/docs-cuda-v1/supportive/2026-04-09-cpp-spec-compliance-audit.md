---
title: C++ tile-cpp spec compliance audit
date: 2026-04-09
engine: codex
type: audit
status: complete
refs: [docs/tile_spec.md, docs/tile_operations.md, docs/compositor_spec.md, docs/grid_spec.md, tile-cpp/include/constants.h, tile-cpp/src/sieve.cpp, tile-cpp/src/compact.cpp, tile-cpp/src/union_find.cpp, tile-cpp/src/face_extract.cpp, tile-cpp/src/prune.cpp, tile-cpp/src/encode.cpp, tile-cpp/src/process_tile.cpp]
---

## Summary
52 PASS / 1 FAIL / 1 AMBIGUOUS out of 54 checklist checks.

Additional non-checklist finding: 1 FAIL.

Checklist totals:
- FAIL: `10.2 MAX_PRIMES, MAX_PORTS bounds are safe`
- AMBIGUOUS: `2.2 Miller-Rabin witnesses are sufficient for norms up to ~1.7e18`

Additional non-checklist finding:
- `process_tile()` reports tile-proper prime count in `TileResult.prime_count`, while the tile-operations spec defines `prime_count` as the Phase 2 compacted count over the full 271x271 sieve domain.

## Findings (FAIL and AMBIGUOUS only)

### F1. FAIL — `MAX_PRIMES` contradicts the tile-operations capacity requirement
Spec reference:
`docs/tile_operations.md:580-583` says: "tile proper now has 66,049 lattice points, so any tile-proper point count or `MAX_PRIMES`-style capacity assumption ... must be sized above 66,049."

Code reference:
`tile-cpp/include/constants.h:27-28`
```cpp
constexpr int MAX_PRIMES = 16384;
constexpr int MAX_PORTS  = 2048;
```

What the spec says vs what the code does:
The spec gives an explicit lower bound for any `MAX_PRIMES`-style capacity: above `66,049`. The code hard-codes `MAX_PRIMES = 16,384`, and that value sizes multiple core buffers (`tile-cpp/include/types.h:24`, `tile-cpp/src/union_find.cpp:10`, `tile-cpp/src/process_tile.cpp:41,51`, `tile-cpp/src/face_extract.cpp:112,175,178`). Even if `16,384` is probably enough at the 850M operating point, it does not satisfy the spec's stated requirement.

Severity:
MODERATE

Suggested fix:
Either raise `MAX_PRIMES` above `66,049` to match the current spec, or amend `docs/tile_operations.md` to replace the blanket `> 66,049` rule with a justified operating-point bound if lower memory is intended.

### F2. AMBIGUOUS — Miller-Rabin operating-range spec conflicts with the checklist/radius math
Spec reference:
`docs/tile_operations.md:200-212` says:
`n = a^2 + b^2 < 1.73 * 10^9 < 2^32`
and
"At our operating point ... 4 witnesses, deterministic, zero error."

Conflicting interpretation:
The user checklist for this audit requires verification "for norms up to ~1.7e18 (R=850M, norm=a^2+b^2)", which matches the actual radius arithmetic implied elsewhere in the project.

Code reference:
`tile-cpp/src/sieve.cpp:13-19,166-190`
```cpp
constexpr uint64_t kLargeWitnesses[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };
...
if (n < 3215031751ULL) { ... 2,3,5,7 ... }
...
for (uint64_t witness : kLargeWitnesses) {
    if (!miller_rabin_witness(n, d, s, witness)) {
        return false;
    }
}
```

What the spec says vs what the code does:
If `n` is really `< 2^32` at the operating point, the code's large-witness 64-bit path is unnecessary but harmless. If the checklist's `~1.7e18` range is the intended operating point, the spec text in `docs/tile_operations.md` is wrong and the code is the only thing handling the larger range. Because the spec itself is internally inconsistent on the norm scale, this check cannot be scored cleanly as PASS or FAIL from the canonical docs alone.

Severity:
MODERATE

Suggested fix:
Correct `docs/tile_operations.md:200-212` to the intended operating-point norm range and state the deterministic witness set required for that range explicitly.

### F3. FAIL (additional, non-checklist) — `TileResult.prime_count` uses tile-proper count instead of Phase 2 compact count
Spec reference:
`docs/tile_operations.md:239-240` defines the Phase 2 output as "`prime_count`, dense array `prime_pos[i]` mapping UF index to bitmap position."
`docs/tile_operations.md:619-621` exposes that value through:
```cpp
TileResult process_tile(int64_t a_lo, int64_t b_lo,
                        const SieveTables& tables);
```

Code reference:
`tile-cpp/src/process_tile.cpp:42-43`
```cpp
const int bitmap_prime_count = compact_primes(bitmap, prefix, prime_pos);
result.prime_count = count_tile_proper_primes(prime_pos, bitmap_prime_count);
```

What the spec says vs what the code does:
The spec's `prime_count` is the dense compacted count for the full `271x271` sieve domain. The implementation stores only the count of primes that lie inside tile proper `[0, 256] x [0, 256]`. That does not affect TileOp correctness, but it does make the exported diagnostic field disagree with the documented phase output.

Severity:
LOW

Suggested fix:
Set `result.prime_count = bitmap_prime_count;` and add a separate `tile_proper_prime_count` field if both diagnostics are needed.

## Full Audit Trail

### 1. Tile Geometry
1. PASS — `TILE_SIDE = 256`, `TILE_POINTS = 257`.
Spec: `docs/tile_spec.md:28-37`, `docs/tile_spec.md:755-758`.
Code: `tile-cpp/include/constants.h:5-8` sets `TILE_SIDE = 256`, `TILE_POINTS = TILE_SIDE + 1`, `SIDE_EXP = TILE_POINTS + 2 * COLLAR`.

2. PASS — Tile proper is closed `[0, S] x [0, S] = 257x257`.
Spec: `docs/tile_spec.md:35-42`, `docs/tile_operations.md:375-389`.
Code: `tile-cpp/src/face_extract.cpp:81-83` uses `tile_row > TILE_SIDE` / `tile_col > TILE_SIDE` as the exclusion test, so `tile_row == 256` and `tile_col == 256` are in-tile.

3. PASS — Sieve domain is `271x271 = (S+1+2*collar)^2`.
Spec: `docs/tile_spec.md:31-32`, `docs/tile_operations.md:87-91`.
Code: `tile-cpp/include/constants.h:7-9,18-19` gives `TILE_POINTS = 257`, `SIDE_EXP = 271`, `BITMAP_BITS = SIDE_EXP * SIDE_EXP`.

4. PASS — `COLLAR = 7 = ceil(sqrt(40))`.
Spec: `docs/tile_spec.md:52-56`.
Code: `tile-cpp/include/constants.h:6,9` sets `COLLAR = 7`, `K_SQ = 40`.

5. PASS — Adjacent tiles share the boundary row/column at offset `S = 256`.
Spec: `docs/tile_spec.md:37-41,92-95`.
Code: `tile-cpp/src/face_extract.cpp:81-83,93-102` includes row/col `256` in tile proper and on the O/R faces.

### 2. Sieve Phase (Phase 1)
6. PASS — Gaussian primality definition matches spec.
Spec: `docs/tile_operations.md:44-50`.
Code: `tile-cpp/src/sieve.cpp:312-332` handles axis cases with `coord mod 4 == 3 && is_prime(abs(coord))`, otherwise tests `a^2 + b^2`.

7. AMBIGUOUS — Miller-Rabin witnesses for the intended operating range.
Spec: `docs/tile_operations.md:200-212`.
Code: `tile-cpp/src/sieve.cpp:166-190`.
Evidence: see Finding F2.

8. PASS — Split prime table contains all `p < 10000` with `p ≡ 1 (mod 4)`, count `609`.
Spec: `docs/tile_operations.md:128-137`.
Code: `tile-cpp/src/sieve.cpp:375-410`, `tile-cpp/include/constants.h:14`.

9. PASS — Inert prime table contains all `p < 10000` with `p ≡ 3 (mod 4)`, count `619`.
Spec: `docs/tile_operations.md:128-137`.
Code: `tile-cpp/src/sieve.cpp:375-410`, `tile-cpp/include/constants.h:15`.

10. PASS — No integer overflow in norm computation.
Spec: `docs/grid_spec.md:166-169` requires 128-bit-safe squaring at search radii.
Code: `tile-cpp/src/sieve.cpp:10,27-31,76-78` uses `unsigned __int128` for norm and large-modulus multiplication.

11. PASS — Bitmap indexing uses `bit = row * SIDE_EXP + col`, `word = bit/32`, `mask = 1 << (bit%32)`.
Spec: `docs/tile_operations.md:183-188`, `docs/tile_operations.md:247-265`.
Code: `tile-cpp/src/sieve.cpp:343-346` computes `pos = row * SIDE_EXP + col` then `bitmap[pos >> 5] |= 1U << (pos & 31U)`.

### 3. Compact Phase (Phase 2)
12. PASS — Prefix table is an exclusive prefix sum of popcount.
Spec: `docs/tile_operations.md:242-255`.
Code: `tile-cpp/src/compact.cpp:25-34`.

13. PASS — Prime positions are extracted in word order and low-bit-to-high-bit order.
Spec: `docs/tile_operations.md:267-281`.
Code: `tile-cpp/src/compact.cpp:37-45` loops `w = 0..BITMAP_WORDS-1` and uses `__builtin_ctz(word)` with `word &= word - 1`.

14. PASS — `bitmap_pos_to_uf_index` matches the spec formula.
Spec: `docs/tile_operations.md:258-265`.
Code: `tile-cpp/include/compact.h:12-17`.

### 4. Union-Find Phase (Phase 3)
15. PASS — Backward offsets: exactly 64 entries satisfying the distance and ordering predicates.
Spec: `docs/tile_operations.md:294-306`, `docs/tile_operations.md:764-780`.
Code: `tile-cpp/src/union_find.cpp:50-71`.

16. PASS — `find()` uses path halving.
Spec: `docs/tile_operations.md:319-324`.
Code: `tile-cpp/src/union_find.cpp:16-22` uses `parent[x] = parent[parent[x]]; x = parent[x];`.

17. PASS — Final flatten pass stores roots in every `parent[i]`.
Spec: `docs/tile_operations.md:353-360`.
Code: `tile-cpp/src/union_find.cpp:104-106`.

18. PASS — No race conditions in sequential processing.
Spec: `docs/tile_operations.md:31-33` defines the C/C++ path as sequential reference code.
Code: `tile-cpp/src/union_find.cpp:73-106` is single-threaded and uses no shared mutable concurrency primitives.

### 5. Face Extraction Phase (Phase 4)
19. PASS — Face I uses `tile_row < COLLAR`, `h = tile_col`.
Spec: `docs/tile_operations.md:380-383,401-402`.
Code: `tile-cpp/src/face_extract.cpp:88-91`.

20. PASS — Face O uses rows `250..256`, `h = tile_col`.
Spec: `docs/tile_operations.md:381,388-389,401-402`.
Code: `tile-cpp/src/face_extract.cpp:93-95`.

21. PASS — Face L uses `tile_col < COLLAR`, `h = tile_row`.
Spec: `docs/tile_operations.md:382,401-402`.
Code: `tile-cpp/src/face_extract.cpp:96-99`.

22. PASS — Face R uses cols `250..256`, `h = tile_row`.
Spec: `docs/tile_operations.md:383,388-389,401-402`.
Code: `tile-cpp/src/face_extract.cpp:100-102`.

23. PASS — Only in-tile primes are extracted.
Spec: `docs/tile_operations.md:370-389`.
Code: `tile-cpp/src/face_extract.cpp:81-83`.

24. PASS — Halo-only primes are excluded from faces.
Spec: `docs/tile_operations.md:108-115`.
Code: `tile-cpp/src/face_extract.cpp:81-83,108-110`.

25. PASS — Sort order is `(h, boundary_depth, tile_row, tile_col)`.
Spec: `docs/tile_spec.md:104-110`, `docs/tile_operations.md:398-408`.
Code: `tile-cpp/src/face_extract.cpp:23-35,38-63`.

26. PASS — Corner primes appear on both faces they touch.
Spec: `docs/tile_operations.md:391`.
Code: `tile-cpp/src/face_extract.cpp:66-121` scans every prime independently for each face; no de-duplication across faces is applied.

### 6. Port Clustering (Phase 4 continued)
27. PASS — Consecutive-pair comparison starts a new port when `dist_sq > K_SQ`.
Spec: `docs/tile_spec.md:104-110`, `docs/tile_operations.md:405-408`.
Code: `tile-cpp/src/face_extract.cpp:146-157`.

28. PASS — Distance uses `tile_col` / `tile_row`, not `h`/depth or absolute coordinates.
Spec: `docs/tile_spec.md:104-110`.
Code: `tile-cpp/src/face_extract.cpp:147-149`.

29. PASS — `K_SQ = 40` is used consistently.
Spec: `docs/tile_spec.md:29`, `docs/tile_operations.md:86,406-408`.
Code: `tile-cpp/include/constants.h:9`, `tile-cpp/src/face_extract.cpp:149`.

30. PASS — Port `h1` is the `h` value of the first prime in the cluster.
Spec: `docs/tile_spec.md:113-117`, `docs/tile_operations.md:453-458`.
Code: `tile-cpp/src/face_extract.cpp:142-143,154-155`.

31. PASS — Component root is taken from the first prime in the cluster.
Spec: `docs/tile_operations.md:420-426`.
Code: `tile-cpp/src/face_extract.cpp:142,154`.

### 7. Group Assignment
32. PASS — Groups are numbered `1, 2, 3, ...` in first-appearance order across faces `I, O, L, R`.
Spec: `docs/tile_spec.md:283-286`, `docs/tile_operations.md:413-427`.
Code: `tile-cpp/src/face_extract.cpp:180-198`.

33. PASS — Group `0` is reserved.
Spec: `docs/tile_spec.md:202-203,275-278`.
Code: `tile-cpp/src/face_extract.cpp:180` starts at `next_group = 1`; zeros remain empty sentinels.

34. PASS — `root_to_group` is global across all faces.
Spec: `docs/tile_operations.md:417-426`.
Code: `tile-cpp/src/face_extract.cpp:175-176,188-192`.

35. PASS — Group numbering is deterministic.
Spec: `docs/tile_spec.md:22,283-286`.
Code: `tile-cpp/src/face_extract.cpp:182-198` combines fixed face order with deterministic face sorting.

### 8. Dead-End Pruning (Phase 5a)
36. PASS — Dead-end means exactly one port on exactly one face.
Spec: `docs/tile_spec.md:299-301`, `docs/tile_operations.md:432-446`.
Code: `tile-cpp/src/prune.cpp:43-46`.

37. PASS — Dead-end groups are removed entirely.
Spec: `docs/tile_spec.md:296-301`.
Code: `tile-cpp/src/prune.cpp:50-53`.

38. PASS — Surviving groups are renumbered contiguously from `1`.
Spec: `docs/tile_operations.md:432-446`.
Code: `tile-cpp/src/prune.cpp:49-65`.

39. PASS — Port order is preserved within each face after pruning.
Spec: `docs/tile_spec.md:200-203`, `docs/tile_operations.md:490-491`.
Code: `tile-cpp/src/prune.cpp:50-63` iterates ports in original order and only filters/remaps.

### 9. TileOp Encoding (Phase 5b)
40. PASS — Byte layout matches the spec.
Spec: `docs/tile_spec.md:175-183,211-216`, `docs/tile_operations.md:481-487`.
Code: `tile-cpp/src/encode.cpp:48,57,65`.

41. PASS — Overflow triggers on `face > 16`, `group_count >= 255`, or `h1 > 255`.
Spec: user checklist; related canonical text at `docs/tile_operations.md:501-537`.
Code: `tile-cpp/src/encode.cpp:15-19,41-44,53-63`.

42. PASS — Overflow poisons all 128 bytes with `0xFF`.
Spec: `docs/tile_spec.md:345-346`, `docs/tile_operations.md:503-505`.
Code: `tile-cpp/src/encode.cpp:7-9`.

43. PASS — `h1` is written only for `L/R`, not `I/O`.
Spec: `docs/tile_spec.md:119-125`, `docs/tile_operations.md:467-470,533-537`.
Code: `tile-cpp/src/encode.cpp:51-67`.

44. PASS — Unused slots are zero-filled.
Spec: `docs/tile_spec.md:200-203`, `docs/tile_operations.md:487`.
Code: `tile-cpp/src/encode.cpp:12-13` zero-initializes the whole TileOp before selective writes.

45. PASS — Port ordering within each face is ascending `h1`.
Spec: `docs/tile_spec.md:200-203`, `docs/tile_operations.md:490-491`.
Code: `tile-cpp/src/face_extract.cpp:54-63,186-198` produces face-local sorted order, and `tile-cpp/src/encode.cpp:47-48` preserves that order.

### 10. Constants Cross-Check
46. PASS — Spec-defined constants in `constants.h` match the canonical values.
Spec: `docs/tile_spec.md:752-766`, `docs/tile_operations.md:731-759`.
Code: `tile-cpp/include/constants.h:5-24,30-35`.

47. FAIL — `MAX_PRIMES` contradicts the spec capacity rule.
Spec: `docs/tile_operations.md:580-583`.
Code: `tile-cpp/include/constants.h:27`.
Evidence: see Finding F1.

48. PASS — `OVERFLOW_SENTINEL = 0xFF`.
Spec: `docs/tile_spec.md:763`, `docs/tile_operations.md:753`.
Code: `tile-cpp/include/constants.h:24`.

49. PASS — `NUM_FACES = 4`, `FACE_I = 0`, `FACE_O = 1`, `FACE_L = 2`, `FACE_R = 3`.
Spec: `docs/tile_spec.md:761-763`.
Code: `tile-cpp/include/constants.h:31-35`.

### 11. Edge Cases
50. PASS — Empty face produces zero ports and zero-filled encoding for that face.
Spec: `docs/tile_spec.md:200-203,260-262`.
Code: `tile-cpp/src/face_extract.cpp:132-134`, `tile-cpp/src/encode.cpp:12-13,36-40`.

51. PASS — If all groups are dead-ends, the surviving TileOp is all zero.
Spec: `docs/tile_spec.md:299-301`, `docs/tile_spec.md:260-262`.
Code: `tile-cpp/src/prune.cpp:43-65`, `tile-cpp/src/encode.cpp:12-13`.

52. PASS — `h1 = 256` on `L/R` triggers overflow.
Spec: user checklist; related storage hazard at `docs/tile_spec.md:46-49,116-117,180-181`.
Code: `tile-cpp/src/encode.cpp:53-55,61-63` overflows when `h1 > 0xFFU`.

53. PASS — Origin/on-axis primality follows the axis rule.
Spec: `docs/tile_operations.md:44-50`.
Code: `tile-cpp/src/sieve.cpp:317-323,472-476`.

54. PASS — Two primes on the same face with identical `(h, depth)` would stay in the same port.
Spec: `docs/tile_spec.md:104-110`, `docs/tile_operations.md:405-408`.
Code: `tile-cpp/src/face_extract.cpp:38-51,146-149`. In practice this tuple is unique per lattice point; if a duplicate ever appeared, `dx = dy = 0`, so clustering would not split it.
