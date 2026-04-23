## Worker 8A: M8 campaign_main_cuda Fork and Snapshot Handoff

Fork the CPU campaign main into a CUDA campaign app.

### Files you own
- `apps/campaign_main_cuda.cpp` — NEW FILE: CUDA campaign main
- `CMakeLists.txt` — register new target

### What to implement
1. Fork CPU `campaign_main.cpp` structure from `cpp-campaign-v2/apps/`
2. Replace tile-processing loop with calls to GPU dispatcher
3. Preserve CPU grid construction, compositor, and snapshot writer behavior
4. Wire up to existing K1-K5 pipeline via host_driver API

### Key integration points
- Use `launch_k1_to_k5` or equivalent from host_driver.h
- Feed TileOps to existing CPU snapshot writer (don't reimplement)
- Keep grid iteration logic from CPU version

### CPU reference
- `cpp-campaign-v2/apps/campaign_main.cpp` — the original CPU app
- `cpp-campaign-v2/src/snapshot.cpp` — snapshot writing logic

### Verification gate
- CUDA campaign app builds
- With small stub/debug TileOp source, writes snapshot through CPU `write_snapshot` path
- Manifest metadata matches CPU campaign output format

### Do NOT touch
- Kernel files
- Streaming dispatcher (Worker 8B owns that)
- Test harness files (Worker 8C owns those)

### Deliverable
Commit with campaign_main_cuda fork. Build must succeed.
