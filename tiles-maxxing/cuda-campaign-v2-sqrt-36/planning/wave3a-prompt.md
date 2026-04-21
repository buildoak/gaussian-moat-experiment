## Worker 3A: M3 Launch and API Wiring

Wire K3 and K4 into the host-side test path.

### Files you own
- `include/cuda_campaign/kernels.cuh` — update with full K1-K4 launch orchestration API
- `src/host_driver.cpp` — implement launch orchestration and debug download plumbing
- `apps/cuda_vs_cpu_diff.cpp` — wire K1-K4 path for parent-parity testing

### What to implement
1. Host API that runs K1 → K2 → K3 → K4 in sequence for a tile batch
2. Debug download for `d_parent[]` array after K4 compression
3. First-divergence reporting: tile index + prime index where GPU != CPU

### Existing pieces
- K1+K2 lifted in Wave 1 (commit f1096dc)
- K3 compact lifted in Wave 2 (commit 8cee097)
- K4 union/compression lifted in Wave 2 (commit b6116a2)
- M1 stub passthrough in `cuda_vs_cpu_diff.cpp` (commit 3aeef39)

### Verification gate
- M3 parent-parity test can invoke K1-K4 through the public host API
- Reports first divergence with tile index and prime index
- M2 gates (K1+K2 parity) still pass

### Do NOT touch
- Kernel implementation files (`kernel_*.cu`)
- Test harness files (Worker 1B/1C owns those)

### Deliverable
Commit with K1-K4 launch wiring. Host syntax check must pass.
