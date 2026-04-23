## Worker 8C: M8 Snapshot SHA Gate Harness

Build acceptance harness for the M8 ship gate.

### Files you own
- `tests/test_snapshot_sha_R80M.cpp` — NEW FILE: SHA comparison test
- `scripts/run_snapshot_sha_gate.sh` — NEW FILE: gate runner script

### What to implement

1. **SHA-256 comparison harness**
   - Run CPU snapshot generation
   - Run CUDA snapshot generation
   - Compute SHA-256 for both outputs
   - Compare and report pass/fail

2. **Scale support**
   - Smoke scale: small R value for quick CI
   - Full scale: R=80M K=36 for ship gate
   - Same comparison logic for both

3. **Failure reporting**
   - On mismatch: print both hashes, paths
   - Optionally invoke `cuda_vs_cpu_diff --verbose` for first divergence

### Usage pattern
```bash
# Smoke test
./scripts/run_snapshot_sha_gate.sh --smoke

# Full ship gate
./scripts/run_snapshot_sha_gate.sh --full --r 80000000 --k 36
```

### Verification gate
- Harness can run CPU and CUDA snapshot generation
- SHA-256 comparison works correctly
- Fails with clear output on injected mismatch

### Do NOT touch
- Production code (kernels, dispatcher, campaign app)
- Only test infrastructure

### Deliverable
Commit with SHA gate harness. Scripts must be executable.
