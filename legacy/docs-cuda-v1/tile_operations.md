# Tile Operations Spec v2 — Fused Tile Kernel Pipeline

> Status: **draft** (v2, 2026-04-09: TileOp v2 dynamic layout)
> Companion to: `tile_spec.md` (TileOp format & matching), `grid_spec.md` (tower geometry)
> Scope: the algorithm that takes tile coordinates and produces a 128-byte TileOp

## 1. Overview

This spec defines the **tile kernel** — the function that processes a single tile
and emits one TileOp. It is the inner loop of the entire Gaussian moat search:
73M tiles, each producing 128 bytes that the compositor wires together.

**Input:** tile coordinates `(a_lo, b_lo)` and parameters `S=256, K_SQ (step distance squared), COLLAR = ceil(sqrt(K_SQ))`.

**Output:** one `TileOp` (128 bytes) encoding how boundary primes connect through
the tile interior.

**Pipeline:**

```
tile coordinates
    |
    v
Phase 1: SIEVE        row-by-row prime enumeration          --> bitmap
Phase 2: COMPACT       prefix-popcount + bit extraction      --> dense prime list
Phase 3: UNION-FIND    neighbor scan + component merging     --> component labels
Phase 4: FACE EXTRACT  boundary scan + port clustering       --> face port groups
Phase 5: ENCODE        pack groups, h1, dead-end prune       --> TileOp (128 B)
```

**Design principle:** the implementation path is C/C++ first (sequential, debuggable,
comparable against Python validator), CUDA kernel second. The algorithm is identical
in both — only the parallelism model differs.


## 2. Mathematical Foundation

### 2.1 Gaussian Primes

A Gaussian integer `z = a + bi` (with `a, b` in Z) has norm `N(z) = a^2 + b^2`.

Classification of Gaussian primes:

| Condition | Gaussian prime iff |
|-----------|--------------------|
| `a != 0, b != 0` (off-axis) | `a^2 + b^2` is a rational prime |
| `b = 0` (real axis) | `|a|` is a rational prime and `|a| = 3 mod 4` |
| `a = 0` (imaginary axis) | `|b|` is a rational prime and `|b| = 3 mod 4` |
| special | `1 + i` (and associates) — the prime above 2 |

At our operating point (`R >= 800,000,000`, tiles far from origin), virtually
all primes are off-axis. The test reduces to: **compute `n = a^2 + b^2`, check
if `n` is a rational prime.**

### 2.2 The Row-Sieve Principle

For a fixed row (fixed `a`), we scan columns `b` asking: is `a^2 + b^2` prime?

For a small rational prime `p = 1 mod 4`, Fermat's theorem guarantees that `-1`
is a quadratic residue mod `p`. Let `r = sqrt(-1) mod p` (computed via
Cornacchia's algorithm). Then:

```
p divides (a^2 + b^2)   iff   b = +/- a*r  (mod p)
```

**Proof sketch:** `a^2 + b^2 = 0 (mod p)` implies `(a/b)^2 = -1 (mod p)`,
so `a/b = +/- r (mod p)`, so `b = +/- a * r^{-1} (mod p)`. Since `r^2 = -1`,
we have `r^{-1} = -r = p - r`, so `b = +/- a*r (mod p)`.

This means: for each sieve prime `p` and each row `a`, we can mark ALL composite
columns with a simple stride loop. No per-point arithmetic needed. The cost is
`O(side_exp / p)` marks per prime per row.

For inert primes (`p = 3 mod 4`): `p | (a^2 + b^2)` requires both `p | a` and
`p | b`. So we only sieve column `b = 0 mod p` when `p | a` — which is rare
(probability `1/p` per prime per row).


## 3. Tile Geometry

```
Parameters:
  S       = 256                         tile side length (lattice units, 256 segments = 257 points)
  K_SQ    = step distance squared       connectivity threshold (two primes connect iff dist^2 <= K_SQ)
  COLLAR  = ceil(sqrt(K_SQ))           halo depth for neighbor connectivity
  side_exp = (S + 1) + 2*COLLAR        expanded side (sieve domain per axis)

  Default: K_SQ=40, COLLAR=7. For K_SQ=36, COLLAR=6.

Example (K_SQ=40, COLLAR=7):
  side_exp = 257 + 14 = 271
  Sieve domain: [a_lo - 7, a_lo + S + 7] x [b_lo - 7, b_lo + S + 7]
              = 271 x 271 = 73,441 lattice points (0-indexed: rows 0..270, cols 0..270)
```

Tile proper is the closed box `[a_lo, a_lo + S] x [b_lo, b_lo + S]`, so it
contains `(S + 1) x (S + 1) = 257 x 257 = 66,049` lattice points. Adjacent
tiles share the boundary row/column at offset `S`. (updated 2026-04-09: 257x257 shared boundary convention)

Face boundaries within the sieve domain (relative to sieve origin):

```
Face I (inner):  rows 0..6                  (halo region, a < a_lo)
                 but face primes are in rows 7..13    (tile rows 0..6)
Face O (outer):  rows 264..270              (halo region, a > a_lo + S)
                 but face primes are in rows 257..263 (tile rows 250..256)
Face L (left):   cols 0..6   / face primes in cols 7..13    (tile cols 0..6)
Face R (right):  cols 264..270 / face primes in cols 257..263 (tile cols 250..256)
```

Halo primes participate in union-find (they bridge connections near tile edges)
but are NOT extracted as face primes — they belong to adjacent tiles.

**Face primes** are primes in the tile proper (not halo) within `collar` distance
of the tile boundary. Under the shared-boundary convention, all four faces cover
exactly `collar` = 7 tile rows/cols: Face I and Face L cover `0..6`, Face O and
Face R cover `250..256` (`S - collar + 1 .. S` = `250 .. 256`). These are the
points that appear in the TileOp. (updated 2026-04-09: 257x257 shared boundary convention)


## 4. Phase 1: Prime Sieve

**Input:** tile coordinates `(a_lo, b_lo)`, sieve tables (precomputed).
**Output:** bitmap of Gaussian primes in the sieve domain.

The bitmap uses 1 = prime, 0 = not prime (inverted from the sieve working bitmap
where 1 = composite). The final output bitmap is what downstream phases consume.

### 4.1 Sieve Precomputation (Once, at Init)

For each rational prime `p < L` (sieve limit, currently `L = 10,000`):

- If `p = 1 mod 4` (split prime): compute `r = sqrt(-1) mod p` via `fast_sqrt_neg1(p)`.
  Pack as `u32`: `(r << 16) | p`. Store in `SIEVE_TABLE[]`.
  Count: 609 split primes below 10,000.

- If `p = 3 mod 4` (inert prime): store `p` in `MOD3_PRIMES[]`.
  Count: 619 inert primes below 10,000.

Total precomputed data: 609 * 4B + 619 * 2B = 3,674 B.

### 4.2 Per-Row Sieve Algorithm

For each row `r` in `[0, side_exp)`:

```
a = a_lo - collar + r                    // absolute real coordinate

// --- Working bitmap: 1 = marked composite, 0 = candidate ---
// ceil(side_exp / 32) words = ceil(271/32) = 9 words = 36 bytes per row
sieve[0..8] = 0                          // all candidates initially

// Step 1: Parity elimination
// If a and b have the same parity, a^2 + b^2 is even -> composite (at norms >> 2)
for col in 0..side_exp:
    b = b_lo - collar + col
    if (a ^ b) & 1 == 0:                 // same parity
        sieve[col >> 5] |= 1 << (col & 31)

// Step 2: Split prime sieve (p = 1 mod 4)
for each (r_root, p) in SIEVE_TABLE:
    residue = (euclidean_mod(a, p) * r_root) % p
    mark_residue_class(sieve, side_exp, b_start, p, residue)
    neg_residue = euclidean_mod(-residue, p)
    if neg_residue != residue:
        mark_residue_class(sieve, side_exp, b_start, p, neg_residue)

// Step 3: Inert prime sieve (p = 3 mod 4)
for each p in MOD3_PRIMES:
    if euclidean_mod(a, p) == 0:         // p divides a (rare: ~1/p)
        mark_residue_class(sieve, side_exp, b_start, p, 0)

// Step 4: Small-norm rescue (only near origin, |a| <= sqrt(L))
if |a| <= 100:
    for col in 0..side_exp where sieve is marked:
        b = b_start + col
        norm = a*a + b*b
        if norm >= 2 and norm <= L and is_prime(norm):
            sieve[col >> 5] &= ~(1 << (col & 31))   // un-mark (was false positive)

// Step 5: Miller-Rabin confirmation
for col in 0..side_exp:
    b = b_start + col
    if a == 0 or b == 0:                 // axis point — special rule
        if is_gaussian_prime_axis(a, b):
            output_bitmap[row * side_exp + col] = 1
    else if not sieve_is_marked(col):    // survived all sieve rounds
        n = a*a + b*b
        if is_prime(n):                  // deterministic MR
            output_bitmap[row * side_exp + col] = 1
```

Where `mark_residue_class` is:
```
mark_residue_class(sieve, width, b_start, p, residue):
    offset = euclidean_mod(residue - euclidean_mod(b_start, p), p)
    for idx in range(offset, width, p):
        sieve[idx >> 5] |= 1 << (idx & 31)
```

### 4.3 Miller-Rabin at This Norm Range

At operating radii `R >= 800,000,000`, norms are in the `~10^18` range, not the
`~10^9` range. For a representative `45 deg` tile at `R = 850,000,000`, with
`a_lo = b_lo = 601,040,640` and sieve-domain offset `S + COLLAR = 263`, the
largest coordinate tested is `601,040,903`, so

`n_max = 601,040,903^2 + 601,040,903^2 ~= 7.22 x 10^17`.

This is far above `u32` but well within `u64`. Norms fit in `u64`, and modular
arithmetic must use 64-bit multiply-high operations for correctness. The
operating path is the Montgomery multiplication path (`mont_mul` / `mont_powmod`
/ `miller_rabin_witness`), which avoids all 128-bit division. See Section 4.5
for details.

Witness selection (deterministic, Jim Sinclair bounds):

| Norm bound | Witnesses | Count |
|------------|-----------|-------|
| `n < 25,326,001` | {2, 3, 5} | 3 |
| `n < 3,215,031,751` | {2, 3, 5, 7} | 4 |
| `n < 4,294,967,296` | {2, 3, 5, 7, 11} | 5 |
| `n <= ~7.22 x 10^17` (operating tiles) | {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37} | 12 |

At our operating point: **12 witnesses, deterministic over the intended norm
range.**

> **Note:** The CUDA kernel uses FJ64_262k 2-round MR (deterministic for n < 2^64), which replaces the multi-witness approach. The 12-witness table remains the C++ reference path.

Per-witness cost: `powmod64(a, d, n)` with ~60 iterations of
square-and-multiply. Each iteration uses one `u64 x u64 -> __int128` multiply
followed by modular reduction. Total per MR test: ~720 multiply-mod operations
for the 12-witness operating-point path.

Pre-filter: trial division by 25 primes (3 through 97) eliminates ~76% of
composites before any modular exponentiation.

### 4.4 Montgomery Multiplication (added 2026-04-09, commit 9a4d5ce)

The original `mulmod64` implementation used `__int128` to compute `(a * b) % m`:
a 64x64 -> 128-bit multiply followed by 128-bit modular reduction. On ARM64,
this compiled to calls to `__udivmodti4` in libcompiler_rt -- a **software**
128-bit integer division routine. Profiling showed this single function consumed
84% of total pipeline runtime. Compiler flags (`-O3 -march=native -flto`) had
zero effect because the bottleneck was in a precompiled system library, not in
the project's own code.

**Montgomery form** eliminates all 128-bit division by replacing `(a * b) % m`
with multiply-high extraction. The key identity: in Montgomery space, modular
multiplication reduces to `(a * b) >> 64` plus a conditional subtraction --
no division at all.

**`MontCtx` structure** (precomputed once per modulus `m`):

```c
struct MontCtx {
    uint64_t m;       // the modulus (odd)
    uint64_t m_inv;   // -m^{-1} mod 2^64 (via Newton iteration)
    uint64_t r2;      // R^2 mod m, where R = 2^64 (for converting to Montgomery form)
};
```

**Integration into the Miller-Rabin pipeline:**

1. Before testing witnesses, construct `MontCtx` for the candidate norm `n`.
   This is a one-time cost per candidate (~3 multiplications for `m_inv` via
   Newton iteration, one `mont_reduce` for `r2`).
2. Convert each witness base `a` to Montgomery form: `a_mont = mont_mul(a, ctx.r2, ctx)`.
3. All modular exponentiations in `powmod64` run entirely in Montgomery domain
   using `mont_mul` -- each call is one `uint64_t x uint64_t -> uint128_t`
   multiply, extract upper 64 bits, conditional subtract. On ARM64 this is
   the `UMULH` instruction (1-cycle latency). On x86-64 it is `MULQ` (same cost).
4. Convert the final result back from Montgomery form with one additional
   `mont_reduce` before comparison.

**Before/after profiling (N=2000 tiles, R ~ 850M, M4 Pro single-threaded):**

```
                   Before (mulmod64)     After (Montgomery)     Speedup
Sieve (Phase 1):   26,479 us median      7,993 us median        3.31x
Total per-tile:    26,647 us median      8,148 us median        3.27x
Throughput:        37.4 tiles/sec        120.8 tiles/sec        3.23x
```

**Phase breakdown after Montgomery (N=2000, R ~ 850M):**

```
Sieve          98.1%  (8.0 ms)   -- now hardware multiplies, not software division
Union-Find      1.6%  (0.13 ms)
Compact         0.1%  (0.01 ms)
Face Extract    0.2%  (0.01 ms)
Prune+Encode    0.0%  (0.001 ms)
```

`__udivmodti4` is completely eliminated from the binary after this change.
All tests remain bit-identical -- Montgomery multiplication is mathematically
exact (it computes the same modular arithmetic, just in a different
representation).

**CUDA equivalence:** The ARM64 `UMULH` instruction maps directly to the CUDA
`__umul64hi()` intrinsic. The Montgomery implementation ports 1:1 to CUDA
kernels -- the C++ code is effectively a prototype of the CUDA kernel's inner
loop for the Miller-Rabin witness test.

**Throughput projection at 8 ms/tile:**

```
32 tiles/tower          = ~260 ms/tower
1,000 towers            ~ 4.3 min
73.4M tiles (full octant) ~ 163 CPU-hours (vs ~540 CPU-hours before Montgomery)
```


### 4.5 Sieve Statistics

Per tile at representative operating radii (`R ~ 850,000,000`, expanded-domain
norms up to `~7.22 x 10^17`):

```
73,441 lattice points (271 x 271)
  -> parity elimination:     ~50%  killed  ->  ~36,720 candidates
  -> split + inert sieve:    ~86%  killed  ->  ~5,140 candidates
  -> trial division:         ~76%  killed  ->  ~1,225 reach full MR
  -> MR confirmation:                      ->  ~1,791 confirmed primes
```

Density: ~24.5 primes per 1000 lattice points. Sparse enough that the union-find
parent array (u16) fits comfortably in 48 KB shared memory alongside the bitmap.


## 5. Phase 2: Prime Compaction

**Input:** output bitmap from Phase 1 (73,441 bits = ~9.0 KB).
**Output:** `prime_count`, dense array `prime_pos[i]` mapping UF index to bitmap position.

### 5.1 Prefix Popcount Table

Compute the exclusive prefix sum of popcount over bitmap words:

```
// ceil(73441 / 32) = 2,296 words
for w in 0..2296:
    count[w] = popcount(bitmap[w])

prefix[0] = 0
for w in 1..2296:
    prefix[w] = prefix[w-1] + count[w-1]

prime_count = prefix[2295] + count[2295]
```

The prefix table enables O(1) mapping from bitmap position to UF index:

```
bitmap_pos_to_uf_index(pos):
    word = pos >> 5
    bit  = pos & 31
    return prefix[word] + popcount(bitmap[word] & ((1 << bit) - 1))
```

### 5.2 Bit Extraction

Build the dense position array:

```
for w in 0..2296:
    word = bitmap[w]
    idx = prefix[w]
    while word != 0:
        bit = ctz(word)                  // count trailing zeros
        prime_pos[idx] = w * 32 + bit
        idx++
        word &= word - 1                // clear lowest set bit
    // Invariant: idx == prefix[w] + count[w]
```

`prime_pos[i]` stores the flat bitmap position of the i-th prime. To recover
coordinates: `row = pos / side_exp`, `col = pos % side_exp`.


## 6. Phase 3: Connected Components

**Input:** bitmap, prefix table, prime_pos array.
**Output:** `component[i]` — root label for each prime `i`.

### 6.1 Neighbor Offsets

Two primes are connected iff their Euclidean distance squared is `<= K_SQ`.
The backward offset table contains all `(dr, dc)` with `dr^2 + dc^2 <= K_SQ` and
`(dr < 0) or (dr == 0 and dc < 0)`. Each pair is processed once (the forward
direction is handled symmetrically by the other prime's backward scan).

```
Offset set: all (dr, dc) where dr^2 + dc^2 <= K_SQ, dr < 0 or (dr == 0, dc < 0)
Count: 64 offsets (for K_SQ=40)

Maximum reach (K_SQ=40): dr in [-6, 0], dc in [-6, 6] (since 7^2 = 49 > 40, 6^2 = 36 <= 40)
```

These offsets are precomputed once and stored as a constant table.

### 6.2 Union-Find Data Structure

```
parent: array of u16, length prime_count     // parent[i] = i initially (self-root)
```

No rank array. Tie-breaking by index: the smaller-index root becomes parent.
This is deterministic — same tile always produces the same component structure.

Operations:

```
find(x):
    while parent[x] != x:
        parent[x] = parent[parent[x]]     // path halving (one-pass compression)
        x = parent[x]
    return x

union(x, y):
    rx = find(x)
    ry = find(y)
    if rx == ry: return
    if rx > ry: swap(rx, ry)
    parent[ry] = rx                        // smaller index becomes root
```

### 6.3 Component Computation

```
for i in 0..prime_count:
    pos = prime_pos[i]
    row = pos / side_exp
    col = pos % side_exp

    for (dr, dc) in BACKWARD_OFFSETS:
        nr = row + dr
        nc = col + dc
        if nr < 0 or nr >= side_exp or nc < 0 or nc >= side_exp:
            continue                       // out of sieve domain
        npos = nr * side_exp + nc
        if bitmap_test(npos):              // neighbor is prime
            j = bitmap_pos_to_uf_index(npos)
            union(i, j)
```

### 6.4 Flatten

After all unions, compress all paths to roots:

```
for i in 0..prime_count:
    parent[i] = find(i)                    // now parent[i] is the root
```


## 7. Phase 4: Face Extraction & Port Classification

**Input:** bitmap, prime_pos, flattened component labels.
**Output:** face port lists with group labels and h1 offsets.

### 7.1 Face Membership

A prime at sieve coordinates `(row, col)` is a **face prime** if it is:
1. Inside the tile proper (not in the halo), AND
2. Within `collar` distance of a tile boundary.

```
tile_row = row - collar                    // tile-relative row (0..S)
tile_col = col - collar

in_tile = (0 <= tile_row <= S) and (0 <= tile_col <= S)

face_I = in_tile and tile_row < collar                  // rows 0..6 of tile
face_O = in_tile and tile_row > S - collar              // rows 250..256
face_L = in_tile and tile_col < collar                  // cols 0..6
face_R = in_tile and tile_col > S - collar              // cols 250..256
```

Equivalently, `in_tile` may be written as `0 <= tile_row < S + 1` and
`0 <= tile_col < S + 1`. All four faces are exactly `collar` = 7 rows/cols deep:
`face_O` starts at `S - collar + 1 = 250` (not 249), giving `250..256` = 7 rows,
matching `face_I`'s `0..6` = 7 rows. (updated 2026-04-09: 257x257 shared boundary convention)

A prime can belong to multiple faces (corner regions belong to two faces).

### 7.2 Port Identification

Ports are maximal contiguous clusters of face primes on the same face, where
consecutive primes are within `sqrt(k)` distance along the face axis.

For each face, collect face primes sorted by their along-face coordinate `h`:

```
Face I/O: h = tile_col       (along the row, 0..256)
Face L/R: h = tile_row       (along the column, 0..256)
```

Port clustering: two adjacent face primes (sorted by h) are in the same port iff
their squared distance `<= K_SQ`. Since face primes span at most `COLLAR` depth,
the maximum perpendicular distance is COLLAR. For same-h primes, distance = depth
difference. For adjacent-h primes, `dist^2 = dh^2 + dd^2 <= K_SQ`.

### 7.3 Group Assignment

Each port inherits its **group label** from the interior component it belongs to.
Group labels are assigned sequentially (1, 2, 3, ...) in order of first
appearance, scanning faces I -> O -> L -> R, ports within each face by ascending h.

```
group_map: component_root -> group_label
next_group = 1

for face in [I, O, L, R]:
    for port in face.ports (sorted by h):
        root = find(port.any_member)
        if root not in group_map:
            group_map[root] = next_group
            next_group++
        port.group = group_map[root]
```

Two ports on the same tile with the same group label are connected through the
tile interior. This is the fundamental invariant that the compositor relies on.

### 7.4 Dead-End Pruning

A group that appears on exactly one face, in exactly one port, is a **dead end**.
It cannot participate in any spanning path (a path must enter and exit the tile
through different faces). These groups are pruned — their ports are removed
from the TileOp.

```
for group in all_groups:
    faces_touched = set of faces where group appears
    if len(faces_touched) == 1:
        ports_on_that_face = count of ports with this group on that face
        if ports_on_that_face == 1:
            mark group as dead-end, remove its ports
```

At R ~ 850M, dead-end pruning eliminates 70-80% of ports (empirically validated).
This is the primary compression that drives tiles under the TileOp v2 packed
data budget.

### 7.5 h1 Offset Computation

For L/R face ports, the `h1` value is the along-face coordinate of the port's
first (minimum-h) prime:

```
port.h1 = min(prime.tile_row for prime in port)     // for L/R faces
```

h1 is still the geometric along-face anchor. TileOp v2 stores each L/R port as:

```
group_byte = ((h1 >> 8) << 7) | (group_id & 0x7F)
h1_byte    = h1 & 0xFF
```

Decode is:

```
group_id = group_byte & 0x7F
h1       = ((group_byte >> 7) << 8) | h1_byte
```

Because `h1` ranges only over tile-proper rows `0..256`, this is a 1-bit
overflow case only. The global group cap is therefore 127 per tile.

I/O faces do not store h1 — adjacent radial tiles share the exact boundary row,
so composition is by shared-prime identity on that duplicated row; deterministic
ordering makes slotwise extraction sufficient as an implementation shortcut.
(updated 2026-04-09: 257x257 shared boundary convention)


## 8. Phase 5: TileOp Encoding

**Input:** face port lists with group labels, h1 offsets, dead-end pruning applied.
**Output:** 128-byte TileOp.

### 8.1 Layout (per tile_spec.md v5)

```
Header:
  tileop[0] = off_I
  tileop[1] = off_L
  tileop[2] = off_R

Payload order:
  tileop[3 .. off_I)                      = Face O groups
  tileop[off_I .. off_L)                  = Face I groups
  tileop[off_L .. off_R)                  = Face L group bytes
  tileop[off_R .. off_R + r_cnt)          = Face R group bytes
  tileop[h_start .. h_start + l_cnt)      = Face L h1 bytes
  tileop[h_start + l_cnt .. 128 - pad)    = Face R h1 bytes
  tileop[127] = 0                         // optional 1-byte pad only
```

Count derivation:

```
o_cnt = off_I - 3
i_cnt = off_L - off_I
l_cnt = off_R - off_L
r_cnt = (125 - o_cnt - i_cnt - 2*l_cnt) >> 1
h_start = off_R + r_cnt
```

Packing order within each face remains ascending h. The face ordering in the
payload is **O-I-L-R** so that within-tower I/O matching data sits at the
lowest byte offsets. Group labels are still assigned by first appearance in the
scan order **I -> O -> L -> R**.

### 8.2 Overflow Sentinel

If the packed-data constraint

```
o_cnt + i_cnt + 2*l_cnt + 2*r_cnt <= 125
```

fails after dead-end pruning, or if total groups per tile exceed 127, the
entire 128-byte TileOp is filled with `0xFF` — all bytes, not just
one field. The whole TileOp is poisoned.

Overflow tiles are detected by `tileop[0] == 0xFF` (structurally
impossible for valid tiles -- see tile_spec.md S4.3). Overflow tiles do NOT
reach the compositor. They are reprocessed by the C++ host path into 256-byte
extended TileOps (TileOp_wide, see tile_spec.md S4.7 Tier 2) and re-injected
before composition. Both conditions are astronomically rare at the operating
point.

### 8.3 Encoding Pseudocode

```
tileop = [0u8; 128]

o = len(O.ports_after_pruning)
i = len(I.ports_after_pruning)
l = len(L.ports_after_pruning)
r = len(R.ports_after_pruning)

// Overflow check: if the packed budget fails, or total groups > 127,
// poison the entire TileOp
if o + i + 2*l + 2*r > 125:
    tileop = [0xFF; 128]
    return tileop
if next_group - 1 > 127:
    tileop = [0xFF; 128]
    return tileop

off_I = 3 + o
off_L = 3 + o + i
off_R = 3 + o + i + l

tileop[0] = off_I
tileop[1] = off_L
tileop[2] = off_R

cursor = 3
for port in O.ports_after_pruning:
    tileop[cursor] = port.group
    cursor++
for port in I.ports_after_pruning:
    tileop[cursor] = port.group
    cursor++
for port in L.ports_after_pruning:
    tileop[cursor] = ((port.h1 >> 8) << 7) | (port.group & 0x7F)
    cursor++
for port in R.ports_after_pruning:
    tileop[cursor] = ((port.h1 >> 8) << 7) | (port.group & 0x7F)
    cursor++

for port in L.ports_after_pruning:
    tileop[cursor] = port.h1 & 0xFF
    cursor++
for port in R.ports_after_pruning:
    tileop[cursor] = port.h1 & 0xFF
    cursor++

if cursor < 128:
    tileop[127] = 0   // optional pad byte when residual budget is odd
```


## 9. Data Structures & Memory Budget

### 9.1 Bitmap

```
Type:   packed u32 words, 1 bit per lattice point
Size:   ceil(73441 / 32) = 2,296 words = 9,184 bytes (~9.0 KB)
Access: read-heavy during UF neighbor lookup (Phase 3)
```

### 9.2 Prefix Popcount Table

```
Type:   u32 per bitmap word (exclusive prefix sum of popcount)
Size:   2,296 * 4 = 9,184 bytes (~9.0 KB)
Access: read during bitmap_pos_to_uf_index (Phase 3)
```

### 9.3 Union-Find Parent Array

```
Type:   u16 per prime
Size:   prime_count * 2 bytes
        typical: ~1,791 primes * 2 = 3,582 bytes (~3.5 KB)
        max observed at R >= 800M: ~2,310 primes * 2 = 4,620 bytes (~4.5 KB)
Access: read-write during union/find (Phase 3), read during flatten (Phase 3)
```

### 9.4 Prime Position Array

```
Type:   u32 per prime (flat bitmap position, max 73441 > u16 range)
Size:   prime_count * 4 bytes
        typical: ~1,791 primes * 4 = 7,164 bytes (~7 KB)
        max observed at R >= 800M: ~2,310 primes * 4 = 9,240 bytes (~9.0 KB)
Access: read during UF neighbor scan (Phase 3) and face extraction (Phase 4)
```

Capacity rules (updated 2026-04-09: 257x257 shared boundary convention):

- **Lattice-point buffers** (`BITMAP_BITS`, `BITMAP_WORDS`, sieve-domain
  bitmaps): size by lattice points, not by primes. The expanded sieve domain is
  `271 x 271 = 73,441` points. Any tile-proper point-count structure under the
  shared-boundary convention must cover `257 x 257 = 66,049` points.
- **Prime-count buffers** (`prime_pos`, UF parent arrays, face-prime staging):
  size by the maximum expected prime count, not by lattice points. At
  `R >= 800M`, observed peaks are ~2,310 primes in the `271 x 271` expanded
  domain and ~2,055 primes in the `257 x 257` tile proper. A crude density
  estimate at `N ~ 1.4e18` is `1 / (2 ln N) ~= 1 / 84`, which would predict
  only ~874 primes over 73,441 lattice points in expectation. The observed
  counts are higher because of sieve-domain geometry, but still far below
  `16,384`. A fixed `MAX_PRIMES = 16,384` is therefore a safe capacity target:
  about `7x` headroom over the observed expanded-domain peak and about `19x`
  over the crude density estimate.
- **CUDA shared-memory constraint:** in the hot pipeline, each prime consumes
  about 6 bytes of prime-count storage (`u32 prime_pos` + `u16 parent`). At
  `MAX_PRIMES = 16,384`, those two buffers total ~96 KB. At `66,049`, they
  would total ~384 KB. The latter is impossible to keep resident in 48 KB shared
  memory and is not a valid blanket requirement for prime-count buffers.
- **Port-count buffers** (face-port scratch, TileOp encoding staging): size by
  ports, not by primes or lattice points. Observed peaks are ~75 ports before
  pruning and ~64 after pruning, so `MAX_PORTS = 2,048` remains a conservative
  safe bound.

### 9.5 Total Memory Budget

```
                    Typical         Max observed at R >= 800M
Bitmap:             9.0 KB          9.0 KB
Prefix table:       9.0 KB          9.0 KB
UF parents (u16):   3.5 KB          4.5 KB
Prime positions:    7.0 KB          9.0 KB
                   -------         -------
Total:             28.5 KB         31.5 KB
```

For the CUDA kernel (future): the observed working set fits within the default
48 KB shared-memory budget. However, a static shared-memory allocation at
`MAX_PRIMES = 16,384` would consume ~96 KB for `prime_pos + parent` alone, so
CUDA implementations must not treat `MAX_PRIMES` as an always-resident
shared-memory footprint. For the C/C++ reference implementation: heap-allocated,
no constraint.


## 10. Implementation Strategy

### 10.1 Phase 1: C/C++ Reference Implementation

Write a single-threaded C/C++ implementation of the full pipeline:

```
struct TileResult {
    uint8_t  tileop[128];
    uint32_t prime_count;
    uint32_t group_count;
    uint32_t ports_before_pruning;
    uint32_t ports_after_pruning;
};

TileResult process_tile(int64_t a_lo, int64_t b_lo,
                        const SieveTables& tables);
```

**Subcomponents:**

1. `sieve_tile(a_lo, b_lo, tables) -> bitmap`
   - Row-by-row sieve + MR, sequential
   - Reuse existing `is_prime()` and sieve logic from CUDA headers (they are
     `__host__ __device__`, already compile on CPU)

2. `compact_bitmap(bitmap) -> (prime_count, prefix, prime_pos)`
   - Sequential prefix sum, bit extraction

3. `build_components(bitmap, prefix, prime_pos) -> parent[]`
   - Sequential UF with path halving
   - Backward-offset neighbor scan

4. `extract_faces(bitmap, prime_pos, parent, a_lo, b_lo) -> FaceData`
   - Scan for face primes, cluster into ports, assign groups

5. `encode_tileop(face_data) -> TileOp`
   - Dead-end pruning, overflow check, pack 128 bytes

**Build:** compile with the project's existing CMake. The CUDA headers
(`miller_rabin.cuh`, `modular_arith.cuh`, `cornacchia.cuh`) already provide
`__host__` implementations of all arithmetic — no GPU required.

### 10.2 Phase 2: Validation Against Python

The Python validator (`tiles-maxxing/tile-validator/`) provides ground-truth
implementations of every stage. Validation protocol:

1. **Sieve agreement:** for a set of test tiles (small, medium, large norms),
   compare prime bitmaps between C++ and Python. Must match exactly.

2. **Component agreement:** compare connected component structure. Components
   may have different root labels but must partition primes identically. Compare
   via canonical labeling (sort component members, assign labels lexicographically).

3. **TileOp agreement:** compare final TileOp bytes. Must match after
   canonicalization (group labels depend on assignment order, which must be
   I -> O -> L -> R as specified).

Test tiles:

```
Tier 1 (operating pt): (692820224, 400000000) — off-axis, ~30 degrees, R >= 800M
Tier 2 (operating pt): (565685248, 565685248) — off-axis, ~45 degrees, R >= 800M
Tier 3 (offset pair):  (692820480, 400000256) — same angular sector, shifted by TILE_SIDE
Tier 4 (offset pair):  (565685504, 565685504) — diagonal operating point, shifted by TILE_SIDE
Tier 5 (stress):       (799999744, 123904512) — off-axis, high radius anisotropy check
```

All test coordinates are multiples of `TILE_SIDE = 256`, off-axis, and satisfy
`sqrt(a_lo^2 + b_lo^2) >= 800,000,000`. Low-radius placeholder tiles are not
meaningful validation for this pipeline. (updated 2026-04-09: 257x257 shared boundary convention)

### 10.3 Phase 3: CUDA Kernel (Future)

After the C/C++ implementation is validated, port to a fused CUDA kernel:

- One block per tile, 256 threads
- Row sieve in registers (each thread owns its row's 9-word bitmap)
- Bitmap in shared memory (written by each thread after sieve)
- UF parent array in shared memory (u16, fits in 48 KB for typical tiles)
- TileOp written to global memory (128 bytes, one coalesced write)

The CUDA kernel is a strict port of the C/C++ algorithm — same phases, same
data structures, same invariants. The only changes are parallelism (threads
divide rows) and memory mapping (heap -> shared memory).


## 11. Performance Model

### 11.1 C/C++ Reference (Single-Threaded)

Per tile, dominant costs (after Montgomery optimization, commit 9a4d5ce):

```
Phase 1 (sieve):
  Residue computation:  271 rows * (609 + 619) primes * ~15ns per mul-hi = ~5.0 ms
  MR confirmation:      ~1,225 full tests * ~300ns per test               = ~0.37 ms
  Total sieve:          ~8.0 ms per tile (was ~26.5 ms before Montgomery)

Phase 3 (UF):
  Neighbor scan:        ~1,791 primes * 63 offsets * ~20ns per lookup     = ~0.13 ms

Phase 4-5 (face + encode):
  Port scan + encode:   ~80 face primes, negligible                      = ~0.01 ms

Total:                  ~8.1 ms per tile (CPU, single-threaded, M4 Pro)
```

The sieve dominates at 98.1% of total pipeline time. All remaining phases are
sub-millisecond. At 8.1 ms/tile, processing 1000 test tiles takes ~8 seconds.

**Historical (pre-Montgomery):** The original `mulmod64` path used `__int128`
modular arithmetic, compiling to `__udivmodti4` (software 128-bit division in
libcompiler_rt). That path ran at ~26.5 ms/tile, with the software division
consuming 84% of runtime. See Section 4.5 for the full Montgomery analysis.

### 11.2 CUDA Fused Kernel (Future Target)

```
Phase 1 (sieve):     ~130K cycles    (256 threads, ~1 row/thread, register bitmaps)
Phase 2 (compact):     ~2K cycles
Phase 3 (UF):         ~50K cycles    (shared memory, 20-cycle access)
Phase 4-5 (extract):   ~2K cycles
                      --------
Total:               ~184K cycles = ~123 us @ 1.5 GHz

Throughput: ~8,130 tiles/sec per SM
Full campaign (73M tiles, 108 SMs): ~73M / (8130 * 108) = ~83 seconds
```


## 12. Constants & Parameters

```c
// Tile geometry
static const int32_t  TILE_SIDE      = 256;
static const int32_t  COLLAR         = 7;
static const int32_t  SIDE_EXP       = (TILE_SIDE + 1) + 2 * COLLAR;  // 271 (updated 2026-04-09: 257x257 shared boundary convention)
static const int32_t  K_SQ           = 40;      // connectivity threshold

// Sieve parameters
static const uint16_t SIEVE_LIMIT    = 10000;   // sieve primes up to this
static const uint16_t SIEVE_SQRT     = 100;     // sqrt(SIEVE_LIMIT)
static const int      SPLIT_PRIMES   = 609;     // p = 1 mod 4 below SIEVE_LIMIT
static const int      INERT_PRIMES   = 619;     // p = 3 mod 4 below SIEVE_LIMIT

// Bitmap dimensions
static const int      BITMAP_BITS    = SIDE_EXP * SIDE_EXP;      // 73,441
static const int      BITMAP_WORDS   = (BITMAP_BITS + 31) / 32;  // 2,296

// TileOp
static const int      TILEOP_SIZE         = 128;     // bytes
static const int      TILEOP_HEADER_BYTES = 3;       // off_I, off_L, off_R
static const int      TILEOP_DATA_BYTES   = 125;     // packed payload
static const uint8_t  OVERFLOW_SENTINEL   = 0xFF;

// MR witness sets (deterministic bounds)
static const uint64_t MR_WITNESSES_3[] = {2, 3, 5};
static const uint64_t MR_WITNESSES_4[] = {2, 3, 5, 7};
static const uint64_t MR_WITNESSES_5[] = {2, 3, 5, 7, 11};
```


## Appendix A: Backward Offset Table

All `(dr, dc)` with `dr^2 + dc^2 <= 40` and `(dr < 0) or (dr == 0 and dc < 0)`:

```
dr = -6: dc in [-2, 2]     (5 offsets)    6^2 + 2^2 = 40
dr = -5: dc in [-3, 3]     (7 offsets)    5^2 + 3^2 = 34
dr = -4: dc in [-4, 4]     (9 offsets)    4^2 + 4^2 = 32
dr = -3: dc in [-5, 5]     (11 offsets)   3^2 + 5^2 = 34
dr = -2: dc in [-6, 6]     (13 offsets)   2^2 + 6^2 = 40
dr = -1: dc in [-6, 6]     (13 offsets)   1^2 + 6^2 = 37
dr =  0: dc in [-6, -1]    (6 offsets)    0^2 + 6^2 = 36
                            -----
                            64 offsets total
```

The exact count depends on the boundary `dr^2 + dc^2 <= 40` (not `< 40`).
The offset `(0, 0)` is excluded (self). Forward offsets (symmetric) are handled
by the other prime's backward scan.
