## Worker 2A: M3 K3 Compact Lift

Lift v1 K3 (compact) kernel into the v2 scaffold.

### Files you own
- `src/kernel_compact.cu` — K3 lift
- `include/cuda_campaign/compact_buffers.cuh` — buffer types for compact phase

### Source material
- v1 kernel: `../campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_compact.cu`
- v1 includes: `../campaign-sqrt-36/tile_cuda_multi_kernel/include/`
- CPU reference: `../cpp-campaign-v2/src/sieve.cpp` (prime collection logic)

### Key changes from v1 to v2
1. `MAX_PRIMES_GPU = 6144` (up from v1's smaller value)
2. Keep implementation byte-for-byte close where possible
3. Adapt constants and public launch signatures to M1/M2 scaffold
4. Update `include/cuda_campaign/kernels.cuh` with K3 launch API

### What K3 does
- Takes K2's prime bitmap output
- Compacts marked primes into a dense array of prime positions
- Outputs: `d_prime_pos[]` array, `d_prime_count` per tile

### Verification gate
- K1/K2 plus K3 build succeeds (host syntax check if no nvcc)
- Compacted prime positions and prime counts match CPU for 16 R=1000 test tiles

### Do NOT touch
- K1, K2 files (already lifted)
- K4, K5 files (Worker 2B owns K4)
- Test files

### Deliverable
Commit with working K3 lift. If no nvcc available, commit code that passes host syntax check.
