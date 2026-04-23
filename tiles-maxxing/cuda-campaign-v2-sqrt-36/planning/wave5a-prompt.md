## Worker 5A: M4 Debug Download and Parity Wiring

Wire the new M4 outputs into the host test path. No kernel algorithm changes.

### Files you own
- `include/cuda_campaign/kernels.cuh` — update launch signatures for M4 outputs
- `src/host_driver.cpp` — implement download plumbing for M4 debug buffers
- `apps/cuda_vs_cpu_diff.cpp` — wire M4 parity comparison path

### What to implement
1. Host API that downloads M4 outputs after K4 completes:
   - `d_prime_geo_bits[]`
   - `d_wire_label_by_raw_root[]`
   - `d_max_label`
   - `d_overflow`
   - `d_group_flags[]`
2. First-divergence reporting for each M4 output array
3. CLI flag or mode to run M4-specific parity check

### Existing pieces
- M4 kernel implementation landed in Wave 4 (commit 518a44f)
- M3 parent-parity wiring landed in Wave 3 (commit 6af3be3)
- Buffer structs in `include/cuda_campaign/uf_buffers.cuh`

### Verification gate
- M4 parity tests can download all 5 output arrays
- First-divergence reporting works for each array type
- M2-M3 gates still pass (no regression)

### Do NOT touch
- Kernel implementation files (`kernel_*.cu`)
- Test harness files
- K5 files

### Deliverable
Commit with M4 debug download wiring. Host build must pass.
