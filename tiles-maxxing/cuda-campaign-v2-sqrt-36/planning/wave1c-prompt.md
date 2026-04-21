## Worker 1C: K5 Parity Harness Scaffold

Build the test-side CPU oracle and fixture machinery for M5-M7 verification gates.

### Files you own
- `tests/test_face_groups_parity.cpp`
- `tests/test_port_sort_collision.cpp`
- `tests/test_full_tileop_parity.cpp`
- `tests/support/k5_parity_support.h`
- `tests/support/k5_parity_support.cpp`

### What to build
1. **Face groups parity test (M6 gate):** Compare GPU `n[4]` + `face_groups[192]` against CPU for 100 test tiles. Padding bytes `face_groups[sum(n)..192]` must be zero.

2. **Port sort collision test (M6 gate):** Construct adversarial tile with two face-ports colliding at same `(h, p_perp)` — verify 3rd-key label tiebreak is applied. The comparator must be `(h, p_perp, global_wire_label)`.

3. **Full TileOp parity test (M7 gate):** Full 256 B memcmp for 1024 tiles at R=10000, K=36. Must handle empty (tile_flags=0x02) and overflow (tile_flags=0x01) cases.

### CPU references
- `../cpp-campaign-v2/src/tileop.cpp:92-148` — build_face_ports
- `../cpp-campaign-v2/src/tileop.cpp:143-147` — std::sort with 3-key comparator
- `../cpp-campaign-v2/src/tileop.cpp:151-155` — overflow_tileop
- `../cpp-campaign-v2/src/tileop.cpp:230-233` — empty branch
- `../cpp-campaign-v2/src/tileop.cpp:254-278` — face-order write loop
- `../cpp-campaign-v2/include/campaign/tileop.h:110-119` — bit_set/bit_test

### Verification gate
- Test targets compile (with stubs for GPU API if kernel entrypoints don't exist yet)
- CPU-side adversarial fixture generation runs locally
- No production CUDA files modified

### Do NOT touch
- Any `src/*.cu` files
- Any `include/cuda_campaign/*.cuh` files

### Deliverable
Commit with test scaffolds that compile. Tests may be marked as "pending GPU" if kernel API not wired yet.
