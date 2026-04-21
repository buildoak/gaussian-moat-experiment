## Worker 1A: M2 K1+K2 Lift

Lift v1 K1 (sieve) and K2 (Miller-Rabin) kernels into the v2 scaffold.

### Files you own
- `include/cuda_campaign/gpu_math.cuh` — Barrett, Montgomery MR, __umul64hi helpers
- `include/cuda_campaign/fj64_table.cuh` — verbatim copy of FJ64 table
- `include/cuda_campaign/campaign_constants.cuh` — mirrors CPU campaign_constants.h for __constant__ upload
- `include/cuda_campaign/constants.cuh` — update if needed
- `src/constants_upload.cu` — once-per-campaign upload of constants, FJ64, Barrett, bk_offsets
- `src/kernel_sieve.cu` — K1 lift
- `src/kernel_mr.cu` — K2 lift
- `include/cuda_campaign/kernels.cuh` — update launch API

### Source material
- v1 kernels: `../campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_sieve.cu`, `kernel_mr.cu`, `gpu_math.cuh`
- v1 constants: `../campaign-sqrt-36/tile_cuda_multi_kernel/include/gpu_constants.cuh`
- CPU reference: `../cpp-campaign-v2/src/sieve.cpp`, `primality.cpp`, `include/campaign/fj64_table.h`
- CPU constants: `../cpp-campaign-v2/include/campaign/campaign_constants.h`

### Key changes from v1 to v2
1. `COLLAR = ceil_isqrt(K_SQ)` → `C = floor_isqrt(K_SQ)` (both are 6 at K=36, add static_assert)
2. `MAX_PRIMES_GPU = 6144` (up from v1's smaller value)
3. FJ64 table goes in global memory (512 KB too large for __constant__)
4. SHA-256 of FJ64 table must match `campaign_constants.h:28-29`

### Verification gate
1. Build succeeds with CUDA enabled (host-only syntax check OK if no nvcc)
2. 16-tile K1 bitmap parity at R=1000: reconstruct CPU halo bitmap, memcmp against GPU
3. Full-band MR parity: for every n in [R_inner²-K, R_outer²+K] at R=1000, is_prime_cuda(n) == is_prime(n)
4. FJ64 SHA-256 matches pinned constant

### Do NOT touch
- K3, K4, K5 files
- test files (Worker 1B/1C owns those)

### Deliverable
Commit with working K1+K2 lift. If no nvcc available, commit code that passes host syntax check and document CUDA build as pending.
