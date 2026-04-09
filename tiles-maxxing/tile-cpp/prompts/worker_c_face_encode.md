# Worker C: Phase 4 (Face Extract) + Phase 5 (Encode)

## Context

You are implementing face extraction and TileOp encoding for a Gaussian moat
tile processor. Your input is: prime positions + flattened component labels from
Phases 2-3. You produce the final 128-byte TileOp that encodes how boundary
primes connect through the tile interior.

The project lives at `tiles-maxxing/tile-cpp/`. Pure C++17, no CUDA.
Designed for future CUDA port — avoid exceptions, RTTI, STL containers in hot paths.

## Read First

- **Spec:** `tiles-maxxing/docs/tile_operations.md` — Sections 3, 7, 8
- **Python reference (algorithm patterns):**
  - `tiles-maxxing/tile-validator/ports.py` — extract_ports(), _face_primes(), _group_into_ports()
  - `tiles-maxxing/tile-validator/pruning_analysis.py` — classify_groups(), prune_ports()
- **Shared headers (already exist, DO NOT modify):**
  - `tiles-maxxing/tile-cpp/include/constants.h`
  - `tiles-maxxing/tile-cpp/include/types.h`

Read ALL reference files before writing code.

## Your Deliverables

Create these files inside `tiles-maxxing/tile-cpp/`:

### `include/face_extract.h`
```cpp
#pragma once
#include "types.h"
#include <cstdint>

// Phase 4: Extract face primes, cluster into ports, assign group labels.
//
// coord: tile origin coordinates
// bitmap: prime bitmap (BITMAP_WORDS words, 1=prime)
// prefix: prefix popcount table
// prime_pos: dense prime positions (from compact phase)
// prime_count: number of primes
// parent: flattened component roots (from UF phase)
//
// Returns: FaceData with ports ordered I->O->L->R, sorted by ascending h within face.
// Group labels are 1-based, assigned sequentially in scan order.
FaceData extract_faces(const TileCoord& coord,
                       const uint32_t* bitmap, const uint32_t* prefix,
                       const uint32_t* prime_pos, int prime_count,
                       const uint16_t* parent);
```

### `src/face_extract.cpp`
Implementation of face extraction:

**Step 1: Identify face primes (spec Section 7.1)**

A prime at sieve coordinates (row, col) is a face prime if:
1. Inside the tile proper (not in halo), AND
2. Within COLLAR distance of a tile boundary.

```
tile_row = row - COLLAR          // tile-relative (0..255)
tile_col = col - COLLAR

in_tile = (0 <= tile_row < TILE_SIDE) and (0 <= tile_col < TILE_SIDE)

face_I = in_tile and tile_row < COLLAR                  // rows 0..6
face_O = in_tile and tile_row >= TILE_SIDE - COLLAR      // rows 249..255
face_L = in_tile and tile_col < COLLAR                  // cols 0..6
face_R = in_tile and tile_col >= TILE_SIDE - COLLAR      // cols 249..255
```

Corner primes belong to multiple faces. Process them for each face they belong to.

**Step 2: Collect face primes per face**

For each face, collect (uf_index, h_coord, tile_row, tile_col) sorted by h:
- Face I/O: h = tile_col (along the row, 0..255)
- Face L/R: h = tile_row (along the column, 0..255)

Use a fixed-size array (MAX_PORTS or similar bound) — no std::vector in hot path.

**Step 3: Cluster face primes into ports (spec Section 7.2)**

Ports are maximal contiguous clusters. Two face primes are in the same port iff
their squared distance <= K_SQ = 40.

IMPORTANT: Port clustering uses FULL 2D distance between face primes, not just
along-face distance. Face primes span up to COLLAR=7 depth, so perpendicular
distance matters.

Algorithm per face:
```
Sort face primes by h coordinate.
Use union-find (small, per-face) to cluster:
  for each pair of face primes (i, j) where j > i:
      dx = prime_j.tile_col - prime_i.tile_col
      dy = prime_j.tile_row - prime_i.tile_row
      if dx*dx + dy*dy <= K_SQ:
          union(i, j)
```

For efficiency, you can use the same backward-offset scan or spatial bucketing.
But since face prime count is small (~20-80 per tile), brute-force O(n²) per face
is fine for the C++ reference.

**Step 4: Assign group labels (spec Section 7.3)**

Groups come from the INTERIOR component labels (the `parent` array from Phase 3).
Labels are assigned sequentially (1, 2, 3, ...) in scan order: I -> O -> L -> R,
ports within each face sorted by ascending h.

```
group_map: component_root -> group_label (use a fixed-size lookup)
next_group = 1

for face in [I, O, L, R]:
    for port in face_ports sorted by min-h:
        root = parent[port.any_member_uf_index]   // from Phase 3
        if root not in group_map:
            group_map[root] = next_group++
        port.group = group_map[root]
```

**Step 5: Compute h1 for each port**

h1 = minimum along-face coordinate of primes in the port.
Stored as uint8_t (0..255).

For I/O faces: h1 = min(tile_col for primes in port)
For L/R faces: h1 = min(tile_row for primes in port)

### `include/encode.h`
```cpp
#pragma once
#include "types.h"

// Phase 5: Apply dead-end pruning and pack TileOp (128 bytes).
// face_data: from extract_faces (ports with groups, h1)
// Returns: encoded TileOp
TileOp encode_tileop(const FaceData& face_data);
```

### `src/encode.cpp`
Implementation of encoding:

**Step 1: Dead-end pruning (spec Section 7.4)**

A group is a dead-end if it appears on exactly ONE face in exactly ONE port.
Remove all ports belonging to dead-end groups.

```
For each group g:
    Count faces where g appears
    If exactly 1 face:
        Count ports with group g on that face
        If exactly 1 port:
            Mark g as dead-end

Remove all ports with dead-end groups.
```

Do NOT renumber groups after pruning. The group labels stay as assigned in Phase 4.
(The Python validator renumbers; we don't — the TileOp just stores the original labels.)

CORRECTION: Actually, we DO need contiguous group labels for the TileOp because
group labels are packed as u8 and need to be minimal. After pruning, renumber
surviving groups 1..N contiguously. This matches the spec's intent.

**Step 2: Pack TileOp (spec Section 8)**

```
tileop = [0; 128]    // zero-filled

For each face (I=0, O=1, L=2, R=3):
    surviving_ports = ports on this face after pruning, sorted by ascending h
    if len(surviving_ports) > 16:
        tileop[face * 16] = 0xFF     // overflow sentinel
        continue
    for i in 0..len(surviving_ports):
        tileop[face * 16 + i] = surviving_ports[i].group   // u8

// L face h1 offsets (bytes 64-79)
for i, port in enumerate L surviving ports (max 16):
    tileop[64 + i] = port.h1

// R face h1 offsets (bytes 80-95)
for i, port in enumerate R surviving ports (max 16):
    tileop[80 + i] = port.h1

// Bytes 96-127: reserved, remain zero
```

### `tests/test_face_encode.cpp` (optional but recommended)

Standalone test:
1. Create synthetic face data with known ports and groups
2. Test dead-end pruning: group on 1 face with 1 port → pruned
3. Test group renumbering after pruning
4. Test TileOp packing: known ports → expected bytes
5. Test overflow sentinel when >16 ports on a face

Build standalone:
```bash
cd tiles-maxxing/tile-cpp
g++ -std=c++17 -O2 -Wall -Iinclude src/face_extract.cpp src/encode.cpp tests/test_face_encode.cpp -o test_face_encode && ./test_face_encode
```

## Critical Details

- Group labels are 1-based (spec convention). 0 in the TileOp = empty slot.
- h1 is uint8_t. tile_row and tile_col are 0..255, which fits in u8.
- Face primes are INSIDE the tile (not in halo). Halo primes participate in UF
  but are NOT extracted as face primes.
- Corner primes (e.g., tile_row < COLLAR AND tile_col < COLLAR) belong to BOTH
  face I and face L. They appear in both faces' port lists.
- The `parent` array maps UF index → component root. Use `bitmap_pos_to_uf_index`
  (from compact.h) to convert bitmap positions to UF indices.
- Port clustering uses full 2D distance (not just along-face). A face prime
  at (row=8, col=10) and another at (row=12, col=11) have dist²=17 ≤ 40,
  so they're in the same port.

## bitmap_pos_to_uf_index (from compact.h, for reference)

```cpp
int bitmap_pos_to_uf_index(uint32_t pos, const uint32_t* bitmap, const uint32_t* prefix) {
    uint32_t word = pos >> 5;
    uint32_t bit  = pos & 31;
    return static_cast<int>(prefix[word] + __builtin_popcount(bitmap[word] & ((1U << bit) - 1)));
}
```

You will need to include `compact.h` to use this function.

## Style

- C++17, no CUDA headers, no STL containers in hot paths
- Use fixed-size arrays with bounds checks in debug
- `static` or anonymous namespace for internal helpers
- All integer types explicit
