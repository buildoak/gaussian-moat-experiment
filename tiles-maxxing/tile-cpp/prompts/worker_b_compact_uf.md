# Worker B: Phase 2 (Compact) + Phase 3 (Union-Find)

## Context

You are implementing the compaction and union-find phases of a Gaussian moat
tile processor. Your input is a bitmap of Gaussian primes (271×271 bits, from
Phase 1 sieve). You produce: (1) a dense prime position array, and (2) flattened
connected component labels.

The project lives at `tiles-maxxing/tile-cpp/`. Pure C++17, no CUDA.
Designed for future CUDA port — avoid exceptions, RTTI, STL containers in hot paths.

## Read First

- **Spec:** `tiles-maxxing/docs/tile_operations.md` — Sections 5, 6, Appendix A
- **Shared headers (already exist, DO NOT modify):**
  - `tiles-maxxing/tile-cpp/include/constants.h`
  - `tiles-maxxing/tile-cpp/include/types.h`

## Your Deliverables

Create these files inside `tiles-maxxing/tile-cpp/`:

### `include/compact.h`
```cpp
#pragma once
#include "constants.h"
#include <cstdint>

// Phase 2: Compact bitmap into dense prime position array.
// bitmap: input, BITMAP_WORDS uint32_t words, 1 = prime
// prefix: output, BITMAP_WORDS uint32_t words, exclusive prefix popcount
// prime_pos: output, up to MAX_PRIMES uint32_t entries (flat bitmap positions)
// Returns: prime_count
int compact_primes(const uint32_t* bitmap, uint32_t* prefix, uint32_t* prime_pos);

// Convert bitmap position to UF index (O(1) with prefix table)
inline int bitmap_pos_to_uf_index(uint32_t pos, const uint32_t* bitmap, const uint32_t* prefix) {
    uint32_t word = pos >> 5;
    uint32_t bit  = pos & 31;
    return static_cast<int>(prefix[word] + __builtin_popcount(bitmap[word] & ((1U << bit) - 1)));
}
```

### `src/compact.cpp`
Implementation of `compact_primes`:

```
Algorithm (spec Section 5):

1. Compute popcount per word:
   for w in 0..BITMAP_WORDS:
       count[w] = popcount(bitmap[w])

2. Exclusive prefix sum:
   prefix[0] = 0
   for w in 1..BITMAP_WORDS:
       prefix[w] = prefix[w-1] + count[w-1]
   prime_count = prefix[BITMAP_WORDS-1] + count[BITMAP_WORDS-1]

3. Bit extraction (dense position array):
   for w in 0..BITMAP_WORDS:
       word = bitmap[w]
       idx = prefix[w]
       while word != 0:
           bit = __builtin_ctz(word)     // count trailing zeros
           prime_pos[idx] = w * 32 + bit
           idx++
           word &= word - 1             // clear lowest set bit

   prime_pos[i] stores the flat bitmap position of the i-th prime.
   To recover coordinates: row = pos / SIDE_EXP, col = pos % SIDE_EXP.
```

Use `__builtin_popcount` for popcount and `__builtin_ctz` for trailing zeros.
These are GCC/Clang builtins available in C++17 (map to CUDA `__popc`/`__ffs` later).

### `include/union_find.h`
```cpp
#pragma once
#include "constants.h"
#include <cstdint>

// Precomputed backward offsets for neighbor scan.
// All (dr, dc) with dr^2 + dc^2 <= K_SQ and (dr < 0 or (dr == 0 and dc < 0)).
struct BackwardOffsets {
    int dr[64];
    int dc[64];
    int count;  // should be 64
};

// Compute backward offset table (call once at init).
void init_backward_offsets(BackwardOffsets& offsets);

// Phase 3: Build connected components over all primes in the sieve domain.
// Uses backward-offset neighbor scan + union-find with path halving.
//
// bitmap: prime bitmap (BITMAP_WORDS words)
// prefix: prefix popcount table from compact phase
// prime_pos: dense prime positions from compact phase
// prime_count: number of primes
// parent: output, uint16_t[prime_count], flattened component roots
void build_components(const uint32_t* bitmap, const uint32_t* prefix,
                      const uint32_t* prime_pos, int prime_count,
                      uint16_t* parent);
```

### `src/union_find.cpp`
Implementation of union-find and component building:

**Union-Find (spec Section 6.2):**
```
CRITICAL: No rank array. Tie-break by smaller index. Path halving (not full compression).
This is a deliberate spec decision for CUDA portability — no rank saves shared memory.

find(parent, x):
    while parent[x] != x:
        parent[x] = parent[parent[x]]   // path halving
        x = parent[x]
    return x

union_sets(parent, x, y):
    rx = find(parent, x)
    ry = find(parent, y)
    if rx == ry: return
    if rx > ry: swap(rx, ry)
    parent[ry] = rx                      // smaller index becomes root
```

**Backward Offset Table (spec Appendix A):**
```
All (dr, dc) where dr^2 + dc^2 <= 40 and (dr < 0 or (dr == 0 and dc < 0)):

dr = -6: dc in [-2, 2]     →  5 offsets
dr = -5: dc in [-3, 3]     →  7 offsets
dr = -4: dc in [-4, 4]     →  9 offsets
dr = -3: dc in [-5, 5]     → 11 offsets
dr = -2: dc in [-6, 6]     → 13 offsets
dr = -1: dc in [-6, 6]     → 13 offsets
dr =  0: dc in [-6, -1]    →  6 offsets
                              -------
                              64 total
```

Generate this programmatically in `init_backward_offsets()`, don't hardcode.
Verify the count is exactly 64.

**Component Computation (spec Section 6.3):**
```
// Initialize: each prime is its own root
for i in 0..prime_count:
    parent[i] = i

// Scan all primes, check backward neighbors
for i in 0..prime_count:
    pos = prime_pos[i]
    row = pos / SIDE_EXP
    col = pos % SIDE_EXP

    for (dr, dc) in backward_offsets:
        nr = row + dr
        nc = col + dc
        if nr < 0 or nr >= SIDE_EXP or nc < 0 or nc >= SIDE_EXP:
            continue
        npos = nr * SIDE_EXP + nc
        if bitmap bit at npos is set:
            j = bitmap_pos_to_uf_index(npos, bitmap, prefix)
            union_sets(parent, i, j)

// Flatten: compress all paths
for i in 0..prime_count:
    parent[i] = find(parent, i)
```

**bitmap_test helper:**
```cpp
inline bool bitmap_test(const uint32_t* bitmap, uint32_t pos) {
    return (bitmap[pos >> 5] >> (pos & 31)) & 1U;
}
```

### `tests/test_compact_uf.cpp` (optional but recommended)
A standalone test:
1. Create a synthetic bitmap with known prime positions
2. Run compact_primes, verify prime_count and positions
3. Create a small graph (a few primes within distance sqrt(40))
4. Run build_components, verify connected components are correct
5. Verify backward offsets count is 64

Build standalone:
```bash
cd tiles-maxxing/tile-cpp
g++ -std=c++17 -O2 -Wall -Iinclude src/compact.cpp src/union_find.cpp tests/test_compact_uf.cpp -o test_compact_uf && ./test_compact_uf
```

## Critical Details

- `prime_pos[i]` is `uint32_t` because 271*271 = 73,441 > 65,535 (u16 max)
- `parent[i]` is `uint16_t` because prime_count < 8,192 (fits in u16)
- `bitmap_pos_to_uf_index` must use the prefix table for O(1) lookup
- The backward offsets INCLUDE the boundary: dr²+dc² <= 40 (not < 40)
  - (6, 2): 36+4=40 YES. (6, 3): 36+9=45 NO.
- Union-find MUST use smaller-index-wins (not rank). This is the spec.

## Style

- C++17, no CUDA headers, no STL containers in hot paths
- `static` or anonymous namespace for internal helpers
- All integer types explicit
- Stack arrays for fixed-size structures
