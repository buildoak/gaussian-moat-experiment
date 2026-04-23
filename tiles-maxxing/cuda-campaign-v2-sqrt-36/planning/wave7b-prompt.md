## Worker 7B: M7 Full Parity CLI Hardening

Harden the diff CLI for the M7 acceptance gate.

### Files you own
- `apps/cuda_vs_cpu_diff.cpp` — add verbose first-divergence reporting
- `tests/test_full_tileop_parity.cpp` — ensure test covers full 256B TileOp comparison

### What to implement

1. **Verbose divergence reporting**
   - `cuda_vs_cpu_diff --verbose` flag
   - On mismatch, report:
     - Tile index (i, j)
     - Byte offset within TileOp
     - CPU byte value
     - GPU byte value
   - First divergence only (don't flood output)

2. **Test hardening**
   - Ensure `test_full_tileop_parity` compares all 256 bytes
   - Synthetic injected mismatch test: verify --verbose output works
   - Existing M6 parity path still exits zero

### Existing pieces
- `cuda_vs_cpu_diff` with M4 parity from Wave 5A
- K5 integration wiring landing in Wave 6A
- `test_full_tileop_parity.cpp` scaffold from earlier waves

### Verification gate
- `cuda_vs_cpu_diff --verbose` reports tile index, byte offset, CPU byte, GPU byte for injected mismatch
- Existing M6 parity path still exits zero (no regression)

### Do NOT touch
- Kernel implementation files
- K5 algorithm files

### Deliverable
Commit with CLI hardening. Build must pass.
