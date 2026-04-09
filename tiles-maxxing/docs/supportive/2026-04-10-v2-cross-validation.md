---
date: 2026-04-10
engine: claude
status: complete
---

# TileOp V2 Cross-Validation Report

## Summary

Cross-validated the C++ (`tile-cpp/`) and Python (`tile-validator/`) TileOp V2 implementations on 15 diverse tiles. **All 15 tiles produce byte-for-byte identical 128-byte TileOps.** One encoder bug was found and fixed in both implementations (h1 byte placement). One spec-level issue was identified in the h1 parity decode path.

## Test Matrix

| Category | Tiles | Radius Range | Notes |
|----------|-------|-------------|-------|
| R~860M operating radius | 6 | 0.85-0.99B | Diverse angles 6.6-62 deg |
| High port count | 3 | 0.85-0.99B | V1 overflow territory |
| Low density / sparse | 2 | 1.9-1.98B | Very large radius |
| Parity stress | 4 | 0.85B | Even/odd a_lo, b_lo combos |

## Results: Primary Gate (Encoding Identity)

**PASS: 15/15 tiles produce byte-for-byte identical TileOps.**

All tiles match exactly on:
- `prime_count` (sieve phase)
- `group_count` (union-find + pruning)
- `ports_before_pruning` (face extraction)
- `ports_after_pruning` (dead-end pruning)
- All 128 TileOp bytes (header, O/I/L/R groups, L/R h1, padding)
- `tileop_status` (normal/dead/overflow)

Per-tile detail:

| Tile (a_lo, b_lo) | R | Angle | Primes | Groups | Ports | O | I | L | R |
|---|---|---|---|---|---|---|---|---|---|
| (601040640, 601040640) | 0.850B | 45.0 | 2040 | 11 | 54 | 15 | 12 | 12 | 15 |
| (850000128, 490746880) | 0.981B | 30.0 | 2026 | 8 | 48 | 13 | 12 | 7 | 16 |
| (700000000, 700000000) | 0.990B | 45.0 | 2012 | 10 | 48 | 9 | 15 | 15 | 9 |
| (830000128, 200000000) | 0.854B | 13.5 | 2062 | 7 | 57 | 15 | 15 | 17 | 10 |
| (400000000, 750000000) | 0.850B | 61.9 | 2024 | 10 | 54 | 13 | 11 | 17 | 13 |
| (860000000, 100000000) | 0.866B | 6.6 | 1973 | 10 | 40 | 5 | 15 | 10 | 10 |
| (601040384, 601040384) | 0.850B | 45.0 | 2046 | 7 | 40 | 12 | 8 | 8 | 12 |
| (601040896, 601040640) | 0.850B | 45.0 | 2108 | 10 | 56 | 12 | 15 | 15 | 14 |
| (700000256, 700000256) | 0.990B | 45.0 | 2042 | 11 | 52 | 13 | 13 | 13 | 13 |
| (1400000000, 1400000000) | 1.980B | 45.0 | 1976 | 11 | 44 | 9 | 13 | 13 | 9 |
| (1900000000, 100000000) | 1.903B | 3.0 | 2023 | 12 | 46 | 14 | 15 | 9 | 8 |
| (601040640, 601040896) | 0.850B | 45.0 | 2108 | 10 | 56 | 14 | 15 | 15 | 12 |
| (601040641, 601040640) | 0.850B | 45.0 | 2035 | 12 | 55 | 17 | 11 | 12 | 15 |
| (601040640, 601040641) | 0.850B | 45.0 | 2035 | 12 | 55 | 15 | 12 | 11 | 17 |
| (601040641, 601040641) | 0.850B | 45.0 | 2030 | 13 | 56 | 17 | 11 | 11 | 17 |

No tile exceeded the V2 payload budget (125 bytes). Payload utilization ranged from 40 to 85 bytes (32-68% of budget), with 40-65 bytes of padding.

## Bug Found and Fixed: h1 Byte Placement

### Symptom
Both encoders placed h1 packed bytes immediately after the actual R group labels, not at the parser's expected `h_start`. The parser derives `r_cnt` from the spec's budget formula `r_cnt = (125 - o_cnt - i_cnt - 2*l_cnt) >> 1`, which gives the maximum possible R count including zero-padded slots. When the actual R port count was less than `derived_r_cnt` (which is always the case at operating radius -- 40-65 bytes of slack), `h_start` pointed into zero-padded territory.

### Root Cause
The spec defines `h_start = off_R + r_cnt` where `r_cnt` is the budget-derived maximum. The encoder wrote h1 at `off_R + actual_r_cnt` instead. Both C++ and Python had the same bug.

### Fix
Both encoders now pad the R group section with zeros to `derived_r_cnt` slots before writing h1 bytes at the spec-derived `h_start`. The R group padding bytes are already zero from initialization. The change is in:
- `tile-cpp/src/encode.cpp`: `encode_tileop()` -- compute `derived_r_cnt` and skip cursor to `h_start`
- `tile-validator/ports.py`: `encode_tileop()` -- same logic

### Verification
After the fix, all 15 tiles produce identical TileOps between C++ and Python, and the parser correctly reads h1 values at the expected positions. C++ unit tests (test_face_encode, test_e2e) continue to pass.

## Finding: h1 Half-Step Parity Decode Issue

### Observation
The h1 half-step round-trip (encode h1 -> stored = h1>>1 -> decode = 2*stored + face_parity) fails for 42.4% of L/R ports (159 out of 375 checked). The decode is always off by exactly 1.

### Root Cause
The spec's Section 3.1 parity argument ("face primes occupy exactly one parity class of along-face positions") holds for primes at the **face boundary** (depth=0, fixed cross-face coordinate). But the face extraction includes primes across the full collar depth (0-6), where the cross-face coordinate varies. Port anchors (h1 = min along-face position within a cluster) can sit at any depth, so their parity is not determined by the face's fixed coordinate alone.

Example from tile (601040640, 601040640), L face:
- Port h1=12: anchor prime at depth=5, b=601040645 (odd) -> h1 parity = 0
- Port h1=17: anchor prime at depth=0, b=601040640 (even) -> h1 parity = 1
- Same face, opposite h1 parities

### Impact
The `decode_packed_h1()` / `face_h1()` functions are used by the compositor for horizontal port matching (`A.h1 == B.h1 + delta_h`). Since both sides of the comparison use the same (potentially wrong) parity function, the delta_h matching still works correctly when delta_h=0 (shared boundary). For delta_h != 0, the off-by-1 could cause spurious matches or missed matches.

### Not Fixed
This is a spec-level design issue requiring a decision:
1. Store full h1 (costs 1 extra byte per L/R port, widening from u8 to u9 -- impractical)
2. Define h1 as the boundary-depth-0 anchor only (changes port semantics)
3. Accept the ~42% decode error as tolerable (compositor uses group identity, not h1 values, for same-boundary matching)
4. Store parity per port (1 bit per L/R port, ~3-4 bytes per tile)

## Files Modified

- `tile-cpp/src/encode.cpp` -- h1 placement fix in `encode_tileop()`
- `tile-cpp/tests/run_tile.cpp` -- emit full 128-byte tileop_hex
- `tile-validator/ports.py` -- h1 placement fix in `encode_tileop()`
- `tile-validator/cross_validate.py` -- comprehensive cross-validation script
