## Worker 6A: M6 K5 Integration Wiring

Connect K5 helper outputs to the host-visible TileOp debug path and test runner.

### Files you own
- `include/cuda_campaign/kernels.cuh` — add K5 launch orchestration API
- `src/host_driver.cpp` — implement K5 launch and debug download plumbing
- `apps/cuda_vs_cpu_diff.cpp` — wire K5 path for face_groups parity testing

### What to implement
1. Host API that runs K5 (face encode + sort/pack) after K4 completes
2. Debug download for K5 outputs:
   - `face_groups[192]` — packed face port bytes
   - `n[4]` — count per face
   - Per-face DSU roots and representatives (from Wave 5B)
3. Wire `cuda_vs_cpu_diff` to invoke full K1-K5 pipeline and compare face_groups output

### Existing pieces
- K5 Phase 2+3 (DSU, representatives) landed in Wave 5B (commit 7dec01b)
- K5 sort/pack helper landed in Wave 5C (commit f7745b1)
- M4 debug wiring landed in Wave 5A (commit 428859e)
- Buffer structs in `face_encode_buffers.cuh` and `face_sort_pack.cuh`

### Verification gate
- `cuda_vs_cpu_diff` can invoke full K1-K5 pipeline
- face_groups parity test passes through the public host API
- M2-M4 gates still pass (no regression)

### Do NOT touch
- Kernel implementation files (`kernel_*.cu`)
- K5 algorithm files (Workers 5B/5C own those)

### Deliverable
Commit with K5 integration wiring. Host build must pass.
