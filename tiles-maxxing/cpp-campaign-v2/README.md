# cpp-campaign-v2

Unified C++20 reference campaign for Gaussian-moat detection. This is the CPU
reference implementation used for local checks and CUDA parity work. The hot path is
integer-only, deterministic, and supports both verdict-only runs and optional
snapshot emission for CPU/CUDA parity checks.

## Build

```bash
cmake -DK_SQ=36 -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

```bash
# Tiny radii smoke test, verdict-only:
./build/campaign_main --k-sq=36 --r-inner=10000 --r-outer=10032 --region full-octant

# Project parameters with optional snapshot emission:
./build/campaign_main --k-sq=36 --r-inner=80000000 --r-outer=80008192 \
    --region full-octant --snapshot-out /tmp/snapshot.bin
```

`--out` remains an alias for `--snapshot-out`, but new scripts should prefer
the explicit flag. By default no snapshot is written.

Status: **active reference implementation.** Grid, region parsing, constants,
BZ check, sieve, UF, geometry flags, TileOp encoding, full compositor,
streaming compositor, snapshot writer, and campaign CLI are implemented and
covered by CTest.
