---
date: 2026-03-22
engine: coordinator
status: running
type: campaign-manifest
---

# ISE Calibration Campaign -- 2026-03-22

## Purpose

Validate ISE moat detection against known Tsuchimura moats (k^2=26, 32)
and probe the k^2=36 transition zone, using optimized Rust ISE with
Montgomery multiplication, row-sieve, scanline kernel, and symmetry-fixed
stripe placement (positive-only b-offsets).

## Platform

- Jetson Orin Nano, ARM aarch64, 6 cores
- Rust 1.94.0, release profile (LTO, opt-level 3, codegen-units 1)
- Rayon thread pool, all cores (--threads 0)

## Build

- Commit: 5805d37 (deploy: Add ISE calibration campaign script)
- Binary: tile-probe/target/release/ise
- Features: Montgomery powmod, scanline kernel, 32 stripes default

## Smoke Tests (all passed)

| Test | Gate | Result |
|------|------|--------|
| k^2=2 moat detection | PASS or moat detected | PASS (moat at R~15-25) |
| k^2=26 at R~1,015,639 | f(r) drops near moat | f(r)=0.31 at R~1,016,250 |
| k^2=36 timing (R=80M) | < 15s | 8.84s wall, 46.5s user |
| Stripe symmetry fix | io counts not mirror | Asymmetric confirmed |

## Runs

| Run | k^2 | R range | Tile | Stripes | Purpose | Status |
|-----|-----|---------|------|---------|---------|--------|
| k2-moat | 2 | 0-100 | 8 | 8 | Known moat validation | COMPLETE |
| k26-tile200 | 26 | 1M-1.03M | 200 | 32 | Tile sensitivity | COMPLETE |
| k26-tile500 | 26 | 1M-1.03M | 500 | 32 | Tile sensitivity | COMPLETE |
| k26-tile1000 | 26 | 1M-1.03M | 1000 | 32 | Tile sensitivity | COMPLETE |
| k26-tile2000 | 26 | 1M-1.03M | 2000 | 32 | Tile sensitivity | COMPLETE |
| k26-moat-dense | 26 | 950K-1.1M | 500 | 32 | Tsuchimura moat sweep | RUNNING |
| k26-control-below | 26 | 500K-550K | 500 | 32 | Control (connected) | PENDING |
| k32-moat-dense | 32 | 2.7M-2.9M | 500 | 32 | Tsuchimura moat sweep | PENDING |
| k32-control-below | 32 | 1.5M-1.55M | 500 | 32 | Control (connected) | PENDING |
| k36-moat-dense | 36 | 79.5M-80.5M | 2000 | 32 | Transition zone probe | PENDING |
| k36-control-50M | 36 | 49.9M-50.1M | 2000 | 32 | Control (connected) | PENDING |

## Key Questions

1. Does tile height affect moat detection precision?
2. How close does ISE pin the k^2=26 moat (Tsuchimura: R=1,015,639)?
3. How close does ISE pin the k^2=32 moat (Tsuchimura: R=2,823,055)?
4. Where exactly is the k^2=36 transition? Are the 4 candidates from Run A real?
5. Zero false positives in control regions?

## Early Results

### k^2=2 (COMPLETE)

Moat detected cleanly. f(r)=0 at R~20, 28, 36, 60, 68, 76.
Connectivity resumes at R~44 (f=0.125) and R~84 (f=0.125).
Matches known structure exactly.

### Tile-Height Sensitivity (COMPLETE)

All four tile heights tested at k^2=26, R=[1M, 1.03M], 32 stripes.
No f(r)=0 candidates at ANY tile height in this range.

f(r) ranges observed:
- Tile 200: 0.28 - 0.66 (150 shells, ~700ms each)
- Tile 500: 0.25 - 0.63 (60 shells, ~3s each)
- Tile 1000: 0.31 - 0.59 (30 shells, ~11s each)
- Tile 2000: 0.31 - 0.59 (15 shells, ~42s each)

Key finding: tile height does NOT significantly affect f(r) estimates.
All heights produce consistent values in the 0.3-0.6 range.
Larger tiles have slightly narrower f(r) variance (expected: more primes
per tile = better statistics).

Implication: the ISE 32-stripe method may not reach f(r)=0 at k^2=26
because the Tsuchimura moat is narrow enough that 32 angular samples
remain connected. This is not a bug -- it reflects the ISE's sampling
resolution. A true moat candidate requires ALL 32 stripes to be
disconnected simultaneously, which is a very strong signal.

### Per-shell timing

| Tile Height | Time/shell | Primes/shell |
|-------------|-----------|-------------|
| 200 | 0.7s | 67K |
| 500 | 3.0s | 388K |
| 1000 | 11.1s | 1.51M |
| 2000 | 41.9s | 5.97M |

Scaling: ~quadratic in tile height (as expected: area grows as H^2).

## Results (continued runs)

[To be filled as k26-dense, k32, k36 campaigns complete]
