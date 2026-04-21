## Worker 1D: Host Pipeline Scaffold Artifact

Convert the M8 host-pipeline requirements into a detailed implementation spec.

### Output file
- `planning/2026-04-21-host-pipeline-scaffold.md`

### What to document
1. **File ownership:** Which files will M8 workers create/modify
2. **Buffer interfaces:** 
   - SoA flat buffers layout
   - Per-chunk sizing (200k tiles per chunk)
   - Two-phase buffer overlay: how K1/K2 space becomes K3/K4/K5 space
3. **Stream schedule:**
   - 3 CUDA streams: H2D, compute, D2H
   - Triple-buffered pinned host buffers
   - Overlap pattern: stream A compute n, B D2H n-1, host compositor ingest n-2
4. **Memory budget:**
   - 24 GB 4090 target
   - Peak <8.5 GB with two-phase overlay
   - Device memory table for all kernel buffers
5. **Snapshot handoff:**
   - GPU produces TileOps
   - Host compositor ingests
   - Unmodified CPU `write_snapshot` writes final output

### References
- Canonical plan section 3, M8: `../planning/2026-04-21-synthesis-canonical-plan.md`
- v1 two-phase overlay: `../campaign-sqrt-36/tile_cuda_multi_kernel/src/main.cu:240-252`
- CPU campaign main: `../cpp-campaign-v2/apps/campaign_main.cpp:250-494`
- Performance plan: `../planning/2026-04-21-codex-performance.md` (section on memory layout)

### Verification gate
- Artifact is complete with concrete buffer sizes, stream roles, and file assignments
- No code files modified

### Do NOT touch
- Any source files
- Any existing code

### Deliverable
`planning/2026-04-21-host-pipeline-scaffold.md` ready for Wave 8 workers to implement.
