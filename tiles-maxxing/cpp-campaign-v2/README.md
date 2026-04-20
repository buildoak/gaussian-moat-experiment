# cpp-campaign-v2

Unified C++20 reference campaign for Gaussian-moat detection. Ground
truth for the eventual CUDA port. Integer-only hot path, deterministic
outputs, snapshot-based parity.

## Build

```bash
cmake -DK_SQ=36 -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

```bash
# Tiny radii smoke test (grid enumeration only in Phase 1):
./build/campaign_main --k-sq=36 --r-inner=10000 --r-outer=10032 --region full-octant

# Project parameters (Phase 2 will wire snapshot emit):
./build/campaign_main --k-sq=36 --r-inner=80000000 --r-outer=80008192 \
    --region full-octant --out /tmp/snapshot.bin
```

See `methodology/lemmas_v2/cpp-campaign-v2-execution-plan.md` for the
full milestone plan and `methodology/lemmas_v2/campaign-blueprint.md` for
the engineering SSoT.

Status: **Phase 1 complete.** Grid + region + constants + BZ wiring.
Sieve / UF / geo / TileOp / compositor / snapshot are stubs.
