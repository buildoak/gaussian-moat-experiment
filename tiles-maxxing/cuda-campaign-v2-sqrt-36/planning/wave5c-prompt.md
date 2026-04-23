## Worker 5C: M6 Canonical Port Sort and face_groups Packing

Implement the warp-local bitonic sort and face-order byte packing as a separate K5 helper.

### Files you own
- `src/kernel_face_sort_pack.cu` — NEW FILE: implement sort and pack kernel
- `include/cuda_campaign/face_sort_pack.cuh` — NEW FILE: buffer structs and launch API
- `tests/test_port_sort_collision.cpp` — port-sort collision adversarial test

### What to implement

1. **Warp-local bitonic sort**
   - Sort representatives per face
   - Comparator: `(h, p_perp, global_wire_label)` — all three keys, in order
   - The third-key label tiebreak is critical for determinism

2. **Face-order byte packing**
   - Pack sorted representatives into `n[4]` (count per face) and `face_groups[192]`
   - Padding bytes after `sum(n)` must be zero
   - Match CPU `build_face_ports()` output layout exactly

### CPU reference (bit-for-bit match required)
- `cpp-campaign-v2/src/tileop.cpp` — `build_face_ports()` output format
- `cpp-campaign-v2/include/campaign/tileop.h` — `FacePorts` struct layout

### Existing pieces
- K5 skeleton in `kernel_face_encode_v2.cu` (Worker 5B adds DSU/representatives)
- This is a separate helper kernel, called after Phase 3 completes

### Verification gate
- With synthetic per-face representative records:
  - `n[4]` matches CPU for 100 fixtures
  - `face_groups[192]` matches CPU for 100 fixtures
- Port-sort collision adversarial case passes (ties broken by wire_label)
- Padding bytes after `sum(n)` are zero

### Do NOT touch
- `kernel_face_encode_v2.cu` (Worker 5B owns that)
- K4 files
- Host driver wiring

### Deliverable
Commit with K5 sort/pack helper. CUDA compile must pass. Test harness must compile.
