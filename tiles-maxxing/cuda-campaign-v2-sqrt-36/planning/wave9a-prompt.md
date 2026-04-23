## Worker 9A: M8 Full K=36 R=80M Byte-Parity Run

Run the full M8 acceptance gate on the 4090.

### Your task
1. Connect to vast.ai instance (`ssh8.vast.ai:18302`)
2. Ensure latest code is deployed and built
3. Run full-scale acceptance test:
   - Generate CPU snapshot at R=80M K=36
   - Generate CUDA snapshot at R=80M K=36
   - Compare SHA-256 hashes
4. Produce artifact with results

### Commands
```bash
# CPU snapshot (reference)
./campaign --k-sq=36 --r-outer=80000000 --output cpu_snapshot

# CUDA snapshot
./campaign_main_cuda --k-sq=36 --r-outer=80000000 --output cuda_snapshot

# Or use the SHA gate script
./scripts/run_snapshot_sha_gate.sh --full --r 80000000 --k 36
```

### Artifact output
Write results to `artifacts/2026-04-22-m8-r80m-sha-report.md` with:
- Command lines used
- Host/GPU identity (nvidia-smi output)
- Snapshot paths
- SHA-256 hashes (CPU and CUDA)
- Elapsed time for each
- Pass/fail verdict

### Verification gate
- Full K=36 R=80M CUDA snapshot SHA-256 equals CPU golden SHA-256
- If mismatch: include first-divergence output from `cuda_vs_cpu_diff --verbose`

### Notes
- This is a long-running test — R=80M is production scale
- The 4090 should handle it but expect significant runtime
- If it times out, report partial results

### Deliverable
Artifact with full acceptance results. Commit the artifact.
