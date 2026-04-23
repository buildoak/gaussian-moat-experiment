## Worker 8B: M8 Three-Stream Dispatcher and Buffer Overlay

Implement chunked GPU dispatcher with streaming overlap.

### Files you own
- `src/host_driver.cpp` — add streaming dispatcher
- `include/cuda_campaign/host_driver.h` — public dispatcher API

### What to implement

1. **Three-stream architecture**
   - Stream 1: H2D (host-to-device) transfers
   - Stream 2: Compute (kernel execution)
   - Stream 3: D2H (device-to-host) transfers
   - Overlap transfers with compute for pipeline efficiency

2. **Chunked dispatch**
   - Process tiles in chunks (configurable size)
   - Double/triple buffer to keep GPU fed while transferring

3. **Buffer overlay (two-phase reuse)**
   - K1/K2 buffers can be reused for K3/K4/K5 after phase boundary
   - Reduce peak GPU memory usage

4. **Public API**
   - `dispatch_tile_batch(tiles, count, output_tileops)` or similar
   - Clean interface for campaign_main_cuda to call

### Verification gate
- Small-region async dispatch produces same TileOp bytes as `cuda_vs_cpu_diff`
- CUDA memory peak reflects two-phase overlay design
- Sanitizer or CUDA error checks report no stream misuse

### Do NOT touch
- Kernel implementation files
- campaign_main_cuda app (Worker 8A owns that)
- Test harness files (Worker 8C owns those)

### Deliverable
Commit with streaming dispatcher. CUDA build must succeed.
