---
title: Visible Group Remap Fix
date: 2026-04-22
engine: codex
type: report
status: complete
refs: [tiles-maxxing/cpp-campaign-v2/src/tileop.cpp, tiles-maxxing/cuda-campaign-v2-sqrt-36/src/kernel_uf_v2.cu]
---

## Summary

TileOp dense remap now labels only UF roots that are visible to the 256-byte
wire format. A root is visible when any prime in the component has an
inner/outer geo flag or lies in one of the four face strips. Purely internal
roots keep the zero sentinel and do not count toward the 128 group budget.

The CUDA K4 remap preserves the CPU first-appearance label order by first
marking visible roots, then scanning compressed roots in prime-index order and
assigning labels only to marked roots. K5 continues to consume the same
`wire_label_by_raw_root` table.

## Verification

Remote GPU: vast.ai 35425891, `ssh7.vast.ai:25890`, RTX 4090, left running.

Build:

```text
cmake -S . -B build-k36-sm89 -DK_SQ=36 -DCMAKE_CUDA_ARCHITECTURES=89 -DCMAKE_BUILD_TYPE=Release
cmake --build build-k36-sm89 -j12
```

CUDA ctest:

```text
100% tests passed, 0 tests failed out of 10
```

CPU TileOp regression tests:

```text
./tests/test_tileop --gtest_filter=TileOp.*
9 tests passed
```

Full R=80M campaign:

```text
R_inner: 80000000, R_outer: 80008192
active tiles: 8166667
k1_cand_overflow_count: 0
k4_prime_overflow_count: 0
k4_group_overflow_count: 0
k5_port_overflow_count: 0
VERDICT: SPANNING
```

The observed group overflow count dropped to zero for the R=80M validation
run, and the verdict remains SPANNING as expected below Tsuchimura's moat
boundary.

## Notes

The full standalone CPU `ctest` build did not complete because
`tests/test_sieve.cpp` fails to compile on the remote toolchain at a defaulted
`PrimeRef::operator==`. That failure is outside this change; the changed
TileOp target built and its focused tests passed.
