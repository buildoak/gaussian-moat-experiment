## M1: Scaffold + CPU-oracle link + host-side stub round-trip

You are implementing M1 from the canonical plan (attached as context file).

### Goal
Build the project scaffold so that:
1. `cuda-campaign-v2-sqrt-36/` builds with CMake
2. CMake uses `add_subdirectory(../cpp-campaign-v2 cpp_campaign_v2_build EXCLUDE_FROM_ALL)` to link `libcampaign.a`
3. A `cuda_vs_cpu_diff` CLI exists that:
   - Runs CPU tile processing via `campaign::process_tile(coord, constants, grid)`
   - Stubs CUDA as identity passthrough (returns CPU result unchanged)
   - Diffs bytes and exits 0 on a 1-tile region

### Directory structure (from canonical plan section 5)
```
cuda-campaign-v2-sqrt-36/
├── CMakeLists.txt                      # -DK_SQ=36 default; add_subdirectory(../cpp-campaign-v2)
├── include/cuda_campaign/
│   ├── constants.cuh                   # mirrors cpp-campaign-v2 constants
│   ├── tileop.cuh                      # verbatim TileOp struct + static_asserts
│   └── kernels.cuh                     # K1-K5 launch-function API (stubs for now)
├── src/
│   └── stub_passthrough.cu             # stub: just returns CPU result
├── apps/
│   └── cuda_vs_cpu_diff.cpp            # the diff CLI
└── tests/
    └── test_stub_roundtrip.cpp         # basic sanity test
```

### References
- CPU library: `../cpp-campaign-v2/` — use `campaign::process_tile`, `Grid`, `CampaignConstants`, `TileOp`
- CPU entry: `cpp-campaign-v2/src/process_tile.cpp`, `include/campaign/tileop.h:95-102`
- v1 CUDA (for later milestones, don't lift yet): `../campaign-sqrt-36/tile_cuda_multi_kernel/`

### Verification gate
Run `cuda_vs_cpu_diff` on a small region (e.g., R=100, 1 tile). It should:
- Process the tile via CPU
- "Process" via CUDA stub (identity passthrough)
- `memcmp` the two 256-byte TileOp results
- Exit 0 if identical, exit 1 if different

### CMake notes
- Use CUDA separable compilation (`CUDA_SEPARABLE_COMPILATION ON`)
- `CMAKE_CUDA_ARCHITECTURES=89` default
- `-DK_SQ=36` propagates to both CPU and CUDA via `target_compile_definitions`

### Deliverable
Working build + `cuda_vs_cpu_diff` that passes on 1 tile. Commit when done.
