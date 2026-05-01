# Fix Spec: MAX_PRIMES Capacity Distinction

## Context
Audit finding F1: `docs/tile_operations.md` line ~580 says any MAX_PRIMES-style capacity must be above 66,049. But 66,049 is the total lattice points in tile proper (257x257), NOT the maximum prime count. At R~850M operating point, expanded-domain primes peak around ~2,300 and tile-proper primes around ~2,055. The C++ code uses MAX_PRIMES=16,384 which has 7x headroom over observed peaks.

The 66,049 rule is wrong for prime-count buffers. It conflates lattice-point capacity with prime-count capacity.

## What To Fix

Read `docs/tile_operations.md` and find the section around line 580 that states the > 66,049 capacity requirement.

Replace it with a nuanced rule that distinguishes:

1. **Lattice-point buffers** (bitmap, sieve domain): must accommodate 271x271 = 73,441 points (sieve domain) or 257x257 = 66,049 points (tile proper). These are BITMAP_BITS and BITMAP_WORDS.

2. **Prime-count buffers** (prime_pos, UF parent, face primes): sized by maximum expected prime count, NOT lattice points. At R >= 800M:
   - Expanded-domain (271x271) primes: observed peak ~2,310 across operating-point tiles
   - Tile-proper (257x257) primes: observed peak ~2,055
   - Theoretical upper bound: prime density in Gaussian integers at norm N ~ 1/(2 ln N). At N ~ 1.4e18, density ~ 1/84. Over 73,441 points: ~874 expected. Observed is higher due to sieve-domain geometry but well below 16,384.
   - Safe capacity: 16,384 (7x headroom over observed, 19x over expected)
   - CUDA constraint: shared memory budget is 48KB. Each prime consumes ~6 bytes in the hot pipeline (4B position + 2B UF parent). At 16,384: ~96KB total across buffers. At 66,049: ~384KB. The latter cannot fit in shared memory even with register spilling.

3. **Port-count buffers** (face ports, TileOp encoding): MAX_PORTS=2,048 is safe. Observed peak: ~75 ports before pruning, ~64 after. 

Also fix the spec text at docs/tile_operations.md around line 200-212 that says "n < 2^32" for norms. Change to reflect the actual operating-point norm range. At R=850M with sieve domain offset 7:
- Max coordinate: a_lo + S + COLLAR = a_lo + 263
- At 45 deg: a_lo = 601,040,640, so max a = 601,040,903
- Max norm: (601,040,903)^2 + (601,040,903)^2 ~ 7.22e17
- This is well within u64 range but far exceeds u32
- State: "norms fit in u64; __int128 intermediates required for modular arithmetic"

Do NOT change any C++ code. Only modify spec documents.

## Files to Modify
- `docs/tile_operations.md` — the main target

## Files to Read (reference only)
- `tile-cpp/include/constants.h` — current constant values
- `docs/supportive/2026-04-09-cpp-spec-compliance-audit.md` — full audit findings
- `docs/tile_spec.md` — cross-reference for constants table

## Verification
After editing, grep for "66,049" and "66049" across all spec files to ensure no stale references remain. Also grep for "< 2^32" or "< 2\^32" to catch the norm range text.
