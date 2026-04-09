# Fix: Audit-Identified Bugs in tile-cpp

You are fixing 4 issues found by a spec compliance audit of a Gaussian moat tile
processor. The project is at `tiles-maxxing/tile-cpp/`. Read all relevant source
files before making changes.

## Bug 1 (CRITICAL): Port clustering merges non-adjacent ports

**File:** `src/face_extract.cpp` — the port clustering logic

**Problem:** The code uses O(n²) all-pairs union-find to cluster face primes into
ports. This merges primes that are close in 2D but far apart in h-order, collapsing
distinct ports via transitive shortcuts through non-adjacent primes.

**Example:** tile (10,10), Face I — primes at tile coords (row=0,col=163), (row=6,col=165),
(row=0,col=169). Adjacent distances in h-sorted order: 40 and 52. Spec yields 2 ports.
But the all-pairs code sees the non-adjacent pair (0,163)↔(0,169) at distance 36 and
merges everything into 1 port.

**Spec says (Section 7.2):** "Ports are maximal contiguous clusters of face primes on
the same face, where consecutive primes are within sqrt(k) distance along the face axis."
The key word is "consecutive" — only test adjacent pairs in the h-sorted order.

**Fix:** Replace the O(n²) all-pairs UF with a linear scan:
1. Sort face primes by h coordinate (already done)
2. Scan consecutive pairs: if `dist²(prime[i], prime[i+1]) <= K_SQ`, they're in the same port
3. Otherwise start a new port
4. This is a simple greedy linear scan, NOT union-find

IMPORTANT: Re-read the spec Section 7.2 carefully. The clustering uses full 2D distance
between CONSECUTIVE (h-sorted) primes, not just h-distance. But it only tests consecutive
pairs — no transitive closure through non-adjacent primes.

Wait — re-read the spec more carefully. "Port clustering: two adjacent face primes
(sorted by h) are in the same port iff their squared distance <= k."
This means: sort by h, scan linearly, consecutive pairs only. Simple greedy grouping.

BUT — consider this edge case: if primes A, B, C are sorted by h, and dist(A,B) <= K
and dist(B,C) <= K but dist(A,C) > K, they're ALL in the same port (transitivity via B).
The linear scan handles this naturally: A and B merge, then B and C merge, so A, B, C
are one port.

The BUG is that the current code also merges A and C directly even when dist(A,B) > K
and dist(B,C) > K but dist(A,C) <= K. The linear scan would correctly keep them as
separate ports.

## Bug 2: MAX_PRIMES=8192 overflows on (0,0) tile

**File:** `include/constants.h`

**Problem:** Tile (0,0) has 9,174 Gaussian primes. MAX_PRIMES=8192 causes assert
failure in debug and buffer overflow in release.

**Fix:** Bump `MAX_PRIMES` to 16384. This is safe: 16384 * 4 bytes (prime_pos) +
16384 * 2 bytes (parent) = ~98 KB, well within stack limits. The spec says the C++
reference should heap-allocate, but for now a larger stack buffer is the minimal fix.
Also check if any other fixed-size buffers need corresponding increases.

## Risk: sieve_tile requires caller-zeroed bitmap

**File:** `src/sieve.cpp` and `include/sieve.h`

**Problem:** sieve_tile documents "bitmap must be zero-initialized by caller" but
doesn't zero it internally. process_tile does zero it, so e2e is safe, but the
phase API is fragile.

**Fix:** Add `memset(bitmap, 0, BITMAP_WORDS * sizeof(uint32_t))` at the start of
`sieve_tile()`. Remove the documentation about caller zeroing.

## Note: O/R face depth off-by-one

**File:** `src/face_extract.cpp`

**Problem:** O/R face depth computed as `TILE_SIDE - tile_row` / `TILE_SIDE - tile_col`,
giving depth 1..7 instead of 0..6. Should be `TILE_SIDE - 1 - tile_row` etc.

**Fix:** Change to `TILE_SIDE - 1 - tile_row` and `TILE_SIDE - 1 - tile_col`.

## Verification

After all fixes:

1. Rebuild in debug mode with ASan+UBSan:
```bash
cd tiles-maxxing/tile-cpp
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j4
```

2. Run ALL tests:
```bash
./test_sieve && ./test_compact_uf && ./test_face_encode && ./test_e2e
```

3. CRITICAL: Test the (0,0) tile — it must not crash or assert:
   Add a (0,0) tile to test_e2e if not already present.

4. CRITICAL: Test tile (10,10) or similar small tile and verify port counts are
   reasonable (the auditor found the port clustering bug on this tile).

5. If test_face_encode has tests for port clustering, update them to match
   the new linear-scan behavior.

## Style

- Minimal changes. Don't refactor working code.
- Keep C++17, no STL containers in hot paths.
- Update comments where behavior changes.
