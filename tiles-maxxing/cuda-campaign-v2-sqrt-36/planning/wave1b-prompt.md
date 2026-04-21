## Worker 1B: K3/K4 Parity Harness Scaffold

Build the test-side CPU oracle and fixture machinery for M3/M4 verification gates.

### Files you own
- `tests/test_k3k4_parent_parity.cpp`
- `tests/test_dense_remap_adversarial.cpp`
- `tests/test_geo_i128_sweep.cpp`
- `tests/support/k3k4_parity_support.h`
- `tests/support/k3k4_parity_support.cpp`

### What to build
1. **Parent-array parity test (M3 gate):** CPU runs `build_local_dsu(primes)` + `dsu.find(i)` for each i. Test should be ready to compare against GPU `d_parent[]` once K4 exists.

2. **Dense-remap adversarial test (M4 gate):** Construct a tile with 50 components where primes are sorted such that lowest-index prime's root is numerically HIGHER than a later prime's root. This tests that label assignment is by first-appearance-in-prime-index-order, not by root-ID order.

3. **Geo i128 sweep test (M4 gate):** Sweep `norm_sq` over `[R_inner²-2K, R_inner²+2K] ∪ [R_outer²-2K, R_outer²+2K]` at R=1000. Test should be ready to compare GPU geo flags against CPU `geo_tests.cpp`.

### CPU references
- `../cpp-campaign-v2/src/tileop.cpp:159-170` — build_local_dsu
- `../cpp-campaign-v2/src/tileop.cpp:172-204` — dense_remap_roots (first-appearance order)
- `../cpp-campaign-v2/include/campaign/union_find.h` — DSU interface
- `../cpp-campaign-v2/src/geo_tests.cpp:22-67` — is_inner/is_outer

### Verification gate
- Test targets compile (with stubs for GPU API if kernel entrypoints don't exist yet)
- If kernel API not ready, produce compile-gated artifact listing exact symbols expected from K3/K4
- No production CUDA files modified

### Do NOT touch
- Any `src/*.cu` files
- Any `include/cuda_campaign/*.cuh` files (except you may read them for API signatures)

### Deliverable
Commit with test scaffolds that compile. Tests may be marked as "pending GPU" if kernel API not wired yet.
