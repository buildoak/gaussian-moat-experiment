## Worker 2B: M3 K4 Union and Compression Lift

Lift v1 K4 (union-find) kernel phases A and B into the v2 scaffold.

### Files you own
- `src/kernel_uf_v2.cu` — K4 lift (phases A+B only)
- `include/cuda_campaign/uf_buffers.cuh` — buffer types for UF phase

### Source material
- v1 kernel: `../campaign-sqrt-36/tile_cuda_multi_kernel/src/kernel_uf.cu`
- v1 includes: `../campaign-sqrt-36/tile_cuda_multi_kernel/include/`
- CPU reference: `../cpp-campaign-v2/src/tileop.cpp:159-170` (build_local_dsu)
- CPU DSU: `../cpp-campaign-v2/include/campaign/union_find.h:47-52` (smaller-root-wins)
- CPU find: `../cpp-campaign-v2/src/union_find.cpp:43-53` (path-halving)

### What to lift (Phases A+B only)
- **Phase A:** Union via backward-offset scan using atomic_union (smaller-root-wins)
- **Phase B:** Full path compression pass with __syncthreads() fence

### What NOT to implement yet
- Dense-remap (Phase C) — that's M4, Wave 4
- Geo-flag staging (Phase B.5) — that's M4, Wave 3/4
- Group-flag accumulation (Phase D) — that's M4, Wave 4

Leave clear TODO hooks or device-buffer slots for these later phases.

### Key changes from v1 to v2
1. `MAX_PRIMES_GPU = 6144`
2. Keep v1's `atomic_union` at `kernel_uf.cu:57-75` verbatim (smaller-root-wins, path-splitting CAS)
3. Keep v1's compression pass at `kernel_uf.cu:134-137` with __syncthreads() fence
4. Update `include/cuda_campaign/kernels.cuh` with K4 launch API

### Verification gate
- With synthetic CPU-generated prime-position arrays uploaded directly to K4 input buffers
- `d_parent[]` after full compression matches CPU `build_local_dsu(primes)` + `dsu.find(i)` bit-for-bit
- No dependency on Worker 2A's K3 output (use synthetic input)

### Do NOT touch
- K1, K2, K3 files
- K5 files
- Test files

### Deliverable
Commit with working K4 phases A+B lift. Clear hooks for M4 extensions. If no nvcc, commit code that passes host syntax check.
