## Bug Fix: K5 Test Infrastructure Prime Ordering Mismatch

The K5 parity test fails due to a prime ordering mismatch between CPU and GPU, not a kernel bug.

### Problem
- CPU `process_tile()` sorts primes by `(a, b)` lexicographically
- GPU compacts primes from bitmap in row-major order `(row * SIDE_EXP + col)`
- Different ordering → different wire_label assignments → different face_groups
- This causes false parity failures even though the kernel logic is correct

### Your task
Fix the K5 parity test infrastructure so CPU oracle uses the same prime ordering as GPU.

### Approach options
1. **Preferred:** In the test/comparison path, reorder CPU primes to match GPU row-major order before running CPU face_groups computation
2. **Alternative:** Add a canonicalization step that makes results order-independent

### Key files to investigate
- `apps/cuda_vs_cpu_diff.cpp` — the parity comparison logic
- `cpp-campaign-v2/src/tileop.cpp` — CPU `process_tile()` and `build_face_ports()`
- `src/kernel_compact.cu` — GPU prime compaction order
- `tests/test_full_tileop_parity.cpp` — test harness

### How M4 handles this (reference)
M4 parity works because wire_label assignment follows the same prime order on both sides. Check how M4 comparison is structured and apply similar approach to K5.

### Verification gate
- `cuda_vs_cpu_diff --k5 --r-inner 1000 --r-outer 10000 --limit 100` passes
- Existing M2-M4 gates still pass
- ctest suite still passes

### Deliverable
Commit with the fix. Brief explanation of what was changed.
