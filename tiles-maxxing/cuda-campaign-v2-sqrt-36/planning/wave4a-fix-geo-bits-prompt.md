## Bug Fix: M4 Geo Bits Divergence

The M4 geo staging in `kernel_uf_v2.cu` has a bug. Parity test shows:
- `prime_geo_bits[21]` on tile `(i=0, j=1)`: CPU=`2`, CUDA=`0`

### Your task
1. Investigate the geo bit staging logic in `kernel_uf_v2.cu` (Phase B.5)
2. Compare against CPU reference in `cpp-campaign-v2/src/geo_tests.cpp` and `cpp-campaign-v2/src/tileop.cpp`
3. Find the bug and fix it
4. Verify fix passes parity on Jetson

### Key files
- `src/kernel_uf_v2.cu` — the bug is here, in Phase B.5 geo staging
- `include/cuda_campaign/i128_sq_leq.cuh` — geo helper (landed in Wave 3)
- `cpp-campaign-v2/src/geo_tests.cpp` — CPU reference for `is_inner`/`is_outer` logic
- `cpp-campaign-v2/src/tileop.cpp` — CPU reference for how geo bits are set per prime
- `apps/cuda_vs_cpu_diff.cpp` — run with `--m4` flag to test parity

### Geo bits encoding
- Bit 0: `is_inner` (prime is inside inner radius)
- Bit 1: `is_outer` (prime is outside outer radius)
- Value `2` means `is_outer=1, is_inner=0`
- Value `0` means both flags are false

### Likely bug areas
1. Wrong coordinate transformation when calling geo test
2. Missing or incorrect tile offset (a_lo, b_lo) when computing absolute coordinates
3. i128 comparison edge case in the geo helper
4. Wrong buffer indexing when storing geo bits

### Verification gate
- `cuda_vs_cpu_diff --r-inner 100 --r-outer 500 --limit 100 --m4` passes with no geo divergence
- Build succeeds on Jetson with nvcc

### Deliverable
Commit with the fix. Include a brief explanation of what was wrong.
