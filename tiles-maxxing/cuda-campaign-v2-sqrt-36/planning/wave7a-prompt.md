## Worker 7A: M7 Flag Remap and Overflow Finalization

Finish K5 by unpacking dense group flags into inner_flags/outer_flags and enforcing overflow gates.

### Files you own
- `src/kernel_face_encode_v2.cu` — add flag unpacking and overflow finalization
- `include/cuda_campaign/tileop.cuh` — TileOp v3 layout definitions if needed

### What to implement

1. **Flag unpacking**
   - Read `group_flags[]` from M4 (2 bits per group: inner|outer)
   - Unpack into `inner_flags` and `outer_flags` in TileOp output
   - Match CPU `build_tileop()` flag layout exactly

2. **Overflow finalization**
   - If `n[f] > 255` for any face → overflow TileOp
   - If `sum(n) > 192` → overflow TileOp
   - Overflow path: zero payload + `tile_flags=0x01`
   - Empty path: zero payload + `tile_flags=0x02`

3. **256 B TileOp v3 layout**
   - Preserve exact byte layout from CPU reference
   - `n[4]` + `face_groups[192]` + flags + padding

### CPU reference (bit-for-bit match required)
- `cpp-campaign-v2/src/tileop.cpp` — `build_tileop()` function
- `cpp-campaign-v2/include/campaign/tileop.h` — TileOp struct layout

### Existing pieces
- K5 DSU and sort/pack landed in Wave 5B/5C
- K5 integration wiring landing in Wave 6A
- M4 group_flags available from K4 output

### Verification gate
- Full 256 B TileOp parity passes for 1024 tiles at R=10000, K=36
- Overflow path writes exactly zero payload + `tile_flags=0x01`
- Empty path writes exactly zero payload + `tile_flags=0x02`

### Do NOT touch
- Host driver wiring (Worker 6A owns that)
- K4 files
- CLI/test files (Worker 7B owns those)

### Deliverable
Commit with K5 flag remap and overflow finalization. CUDA compile must pass.
