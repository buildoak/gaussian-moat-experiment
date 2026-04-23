## Worker 5B: M6 Face-Strip DSU and Representative Selection

Add K5 Phase 2 and Phase 3: per-face DSU and canonical representative selection.

### Files you own
- `src/kernel_face_encode_v2.cu` — add Phase 2 (per-face DSU) and Phase 3 (representative selection)
- `include/cuda_campaign/face_encode_buffers.cuh` — add debug output buffers for DSU roots and representatives

### What to implement

1. **Phase 2: Per-face DSU**
   - For each face (0-3), run DSU over face-strip primes
   - Use CPU nested-loop pair order exactly (deterministic)
   - Face-strip = primes on the boundary edge for that face
   - Store DSU roots per face-strip prime

2. **Phase 3: Canonical representative selection**
   - For each DSU component on each face, select the representative
   - Strict lexicographic minimum: `(h, p_perp)` where h is distance along face, p_perp is perpendicular coordinate
   - Output: per-face representative records

### CPU reference (bit-for-bit match required)
- `cpp-campaign-v2/src/tileop.cpp` — `build_face_ports()` function
- `cpp-campaign-v2/src/tileop_internal.h` — face port structures

### Existing pieces
- K5 skeleton with empty/overflow paths landed in Wave 3 (commit baa0490)
- Face-strip filter logic already in skeleton

### Verification gate
- Debug output for per-face DSU roots matches CPU for 100 test tiles
- Selected representatives match CPU for 100 test tiles
- Adversarial face-port fixtures pass

### Do NOT touch
- Final port sorting and byte packing (Worker 5C owns that)
- K4 kernel files
- Host driver wiring (Worker 5A/6A owns that)

### Deliverable
Commit with K5 Phase 2+3. CUDA compile must pass.
