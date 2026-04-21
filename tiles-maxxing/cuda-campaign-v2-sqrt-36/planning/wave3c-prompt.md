## Worker 3C: M5 K5 Empty/Overflow Skeleton and Face-Strip Filter

Start K5 with the stable skeleton: empty output, overflow output, and face-strip filtering.

### Files you own
- `src/kernel_face_encode_v2.cu` — new K5 kernel skeleton
- `include/cuda_campaign/face_encode_buffers.cuh` — K5 buffer types
- `tests/test_face_groups_parity.cpp` — update to test empty/overflow paths

### What to implement
1. **Empty tile path:** If `prime_count == 0`, single-thread writes `TileOp{tile_flags=0x02}` (256 bytes of zero except byte 228 = 0x02)

2. **Overflow tile path:** If K4 set `remap.overflow`, single-thread writes `TileOp{tile_flags=0x01}` (256 bytes of zero except byte 228 = 0x01)

3. **Face-strip filter (Phase 1):** Per-face warp-scan producing `face_indices[f][M]` in ascending prime-index order. A prime is on face f if it passes `on_face_strip(prime, coord, face)` via face_perp + C-boundary check.

### CPU reference
- `../cpp-campaign-v2/src/tileop.cpp:151-155` — overflow_tileop
- `../cpp-campaign-v2/src/tileop.cpp:230-233` — empty branch
- `../cpp-campaign-v2/src/tileop.cpp:86-90` — on_face_strip logic
- `../cpp-campaign-v2/src/tileop.cpp:92-103` — face_indices building
- `../cpp-campaign-v2/include/campaign/tileop.h` — TileOp struct layout

### What NOT to implement yet
- Face-strip DSU (Phase 2) — that's Wave 5
- Port sorting and packing (Phases 3-5) — that's Wave 5/6
- Flag remap (Phase 6) — that's Wave 7

### Verification gate
- Empty TileOp matches CPU byte-for-byte (synthetic prime_count=0)
- Overflow TileOp matches CPU byte-for-byte (synthetic remap.overflow=true)
- Face indices debug output matches CPU `build_face_ports` intermediates

### Do NOT touch
- K1-K4 kernel files
- K3/K4 test harness files

### Deliverable
Commit with K5 skeleton. Empty/overflow paths verified. Face-strip filter produces debug output.
