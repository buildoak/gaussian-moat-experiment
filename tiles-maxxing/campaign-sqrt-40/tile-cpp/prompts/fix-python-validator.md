# Fix Python Validator: Consecutive Clustering + Face Label Transposition

## Context

Investigation at `docs/supportive/2026-04-09-ports-after-mismatch-investigation.md` confirmed two bugs in `tile-validator/ports.py`:

1. **Clustering bug:** Python checks new primes against ALL primes in the current cluster (reverse scan). The spec and C++ reference use CONSECUTIVE-PAIR comparison only (compare with immediately previous sorted prime). Python must match.

2. **Face label transposition:** Python face labels are transposed relative to spec/C++ convention: `I <-> L`, `O <-> R`. Must be fixed to match spec.

## What To Fix

### Fix 1: Clustering in ports.py

Find the function that clusters face primes into ports (likely `_cluster_face_primes` or `_connected_to_cluster` or similar).

**Current (wrong):** Scans backward through the entire current cluster to check if a new prime connects to ANY earlier member.

**Correct:** Compare only with the immediately previous sorted face prime. If `dist_sq(primes[i], primes[i-1]) > k_sq`, start a new port. This is the spec-compliant consecutive-pair rule.

The C++ reference in `tile-cpp/src/face_extract.cpp` function `cluster_face_ports()` shows the correct algorithm:
```cpp
for (int i = 1; i < face_prime_count; ++i) {
    const int dx = face_primes[i].tile_col - face_primes[i-1].tile_col;
    const int dy = face_primes[i].tile_row - face_primes[i-1].tile_row;
    if (dx * dx + dy * dy > K_SQ) {
        // start new port
    }
}
```

Translate this to Python. The distance must use sieve-relative (row, col), NOT tile-relative, NOT (h, depth). Make sure you use the same coordinate pair as C++.

IMPORTANT: Read the C++ code first to understand exactly which coordinates it uses for the distance check. Then match that in Python.

### Fix 2: Face Label Transposition in ports.py

Read the face classification logic in `tile-validator/ports.py` (the `collect_face_primes` function or equivalent).

Compare with C++ `tile-cpp/src/face_extract.cpp` `collect_face_primes()`:
- Face I (FACE_I=0): `tile_row < COLLAR`, h = tile_col
- Face O (FACE_O=1): `tile_row >= TILE_SIDE - COLLAR + 1`, h = tile_col
- Face L (FACE_L=2): `tile_col < COLLAR`, h = tile_row
- Face R (FACE_R=3): `tile_col >= TILE_SIDE - COLLAR + 1`, h = tile_row

Fix Python to match this exactly. The transposition was I<->L, O<->R.

### Fix 3: Verification

After making changes, run `python3 tile-validator/sample.py` and verify the output matches the C++ reference:

Expected operating-point results (from C++ run_tile):
- 45 deg (601040640, 601040640): prime_count=2040, group_count after pruning should match C++
- 30 deg (736121088, 424999936): prime_count=2055
- 15 deg (820888320, 220000000): prime_count=2013

The ports_after counts should now be:
- 45 deg: 54 (was 52 under old Python)
- 30 deg: 64 (was 61)
- 15 deg: 51 (was 48)

Run the C++ tests too for cross-reference:
```bash
cd tile-cpp/build && ./test_e2e
```

## Key Files To Read and Modify

**Read (reference, DO NOT modify):**
- `tile-cpp/src/face_extract.cpp` — C++ reference for both clustering and face classification
- `tile-cpp/include/constants.h` — constant values
- `docs/tile_spec.md` — spec sections on port definition
- `docs/tile_operations.md` — spec on face extraction
- `docs/supportive/2026-04-09-ports-after-mismatch-investigation.md` — full investigation with witnesses

**Modify:**
- `tile-validator/ports.py` — fix clustering function and face classification
- Any other .py file that depends on the old clustering or face labeling

**DO NOT modify any C++ files. C++ is the canonical reference.**

## Verification Gate

The fix is done when:
1. `python3 tile-validator/sample.py` runs without error
2. ports_after counts match C++ exactly on all 3 operating-point tiles (54, 64, 51)
3. Per-face port counts match C++ on all 3 tiles
4. No other test or validation script is broken
5. The clustering function is a simple consecutive-pair scan (no reverse cluster iteration)

Print the before/after comparison to stdout so the coordinator can verify.
