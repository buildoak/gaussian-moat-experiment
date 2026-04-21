## Worker 3B: M4 Geo-Test Staging

Implement the signed-epsilon i128 square/compare helper and per-prime geo-flag staging.

### Files you own
- `include/cuda_campaign/i128_sq_leq.cuh` — hand-coded signed-eps i128 sq + leq
- `src/kernel_geo_flags.cu` — per-prime `is_inner`/`is_outer` staging logic
- `tests/test_geo_i128_sweep.cpp` — update test to use new GPU API

### What to implement
1. **i128_sq_leq helper:** Takes signed `eps` (int64_t), squares via `__umul64hi` + `__umul64lo`, compares to bound. Handle two's-complement correctly for negative eps.

2. **Per-prime geo staging:** For each prime, compute:
   - `(a, b) = (a_lo + col - C, b_lo + row - C)`
   - `norm_sq = a² + b²`
   - `eps_i = norm_sq - R_inner² - K`, `eps_o = norm_sq - R_outer² - K`
   - Prefilter via `llabs(eps) <= prefilter_bound`
   - Full test via `i128_sq_leq` for boundary cases
   - Output: 2-bit `prime_geo_bits[i]` (bit 0 = inner, bit 1 = outer)

### CPU reference
- `../cpp-campaign-v2/src/geo_tests.cpp:22-67` — is_inner/is_outer logic
- `../cpp-campaign-v2/include/campaign/campaign_constants.h:65-73` — prefilter + i128 constants
- Canonical plan risk #4: sign discipline for negative eps

### Verification gate
- Geo i128 sweep passes against CPU for inner and outer boundary bands at R=1000
- Sweep `norm_sq` over `[R_inner²-2K, R_inner²+2K] ∪ [R_outer²-2K, R_outer²+2K]`
- Bit-for-bit match with CPU `geo_tests.cpp`

### Do NOT touch
- K4 dense-remap logic (that's Wave 4)
- K4 kernel file structure (just add geo staging as callable device function)

### Deliverable
Commit with i128 helper + geo staging. Tests updated to call GPU path when available.
