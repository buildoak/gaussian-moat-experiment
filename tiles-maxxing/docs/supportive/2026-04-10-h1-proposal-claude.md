---
date: 2026-04-10
engine: claude
status: complete
refs: [docs/tile_spec.md, docs/tile_operations.md, docs/supportive/2026-04-10-v2-cross-validation.md, docs/supportive/2026-04-09-port-overflow-census.md]
---

# Proposal: Group-Bit Steal for Exact 9-Bit L/R h1

## 1. Empirical Analysis

### The actual h1 domain is 0..256, not 0..271

The problem statement says h1 ranges 0--270 (or 271). This is wrong, and the
correction changes the character of the problem entirely.

L/R face primes are extracted from tile-proper points only (not halo points).
The face extraction code in both C++ and Python is unambiguous:

- Face L: `h = tile_row`, where `tile_row in [0, S]` = `[0, 256]`
- Face R: `h = tile_row`, where `tile_row in [0, S]` = `[0, 256]`

The 271-unit side is the *sieve domain* (tile proper + collar on each side).
The collar contributes *depth*, not along-face extent. Halo primes participate
in union-find but are explicitly excluded from face extraction. So:

- Legal L/R h1 values: **0 to 256 inclusive** (257 possible values)
- The overflow is **exactly 1 bit** -- only h1 = 256 does not fit in u8

### How often does h1 = 256 occur?

From the Codex proposal's 64-tile sample: 3 out of 1648 L/R ports had
h1 = 256, a rate of 0.18%. All were on Face L at tile_row = 256 (the shared
boundary). This is the only value that overflows u8.

From the cross-validation report's 15 diverse tiles: with ~375 L/R ports
checked, the overflow rate is consistent. Ports near the shared boundary
at tile_row = 256 are structurally expected -- the boundary row always exists
and can contain primes.

### What about the h1 >> 1 parity scheme?

Dead. The cross-validation proved 42.4% decode failure (159/375 L/R ports).
The parity argument in spec Section 3.1 holds for primes *on the face line
itself* (depth = 0, fixed cross-face coordinate), but port anchors live at
collar depths 0--6 where the cross-face coordinate varies. The same face
produces ports with both even and odd h1 values.

The parity scheme cannot be repaired. Any "fix" would require either changing
what a port anchor means (breaking compositor semantics) or storing per-port
parity bits (burning a bit per port anyway). The approach must be abandoned.

### Group ID headroom

The 100K-tile census at R = 860M shows:

- Group count: mean 9.8, median 10, max 21
- After dead-end pruning (which eliminates ~80% of groups): max surviving ~9

Group IDs are 1-based, sequential, assigned in scan order. The u8 range
(1--255) is utilized at most to value 21. The upper 234 values are never
used. The headroom is not tight -- it is cavernous.

## 2. The Proposed Encoding

### Core idea

Steal bit 7 of each L/R group byte to carry the 9th bit of raw h1. Store
the low 8 bits of raw h1 in the existing L/R h1 byte. Group IDs are capped
at 127 (7 bits).

### Byte layout

The TileOp layout is unchanged at the structural level:

```
Header (3 bytes):
  Byte 0: off_I
  Byte 1: off_L
  Byte 2: off_R

Payload (125 bytes), same order:
  O groups      (o_cnt bytes, group ID in bits 0..7, unchanged)
  I groups      (i_cnt bytes, group ID in bits 0..7, unchanged)
  L groups      (l_cnt bytes, NEW: bits 0..6 = group_id, bit 7 = h1_hi)
  R groups      (r_cnt bytes, NEW: bits 0..6 = group_id, bit 7 = h1_hi)
  L h1 bytes    (l_cnt bytes, NEW: raw h1 & 0xFF instead of h1 >> 1)
  R h1 bytes    (r_cnt bytes, NEW: raw h1 & 0xFF instead of h1 >> 1)
```

For O/I group bytes: no change. Full u8 group ID.

For L/R group bytes:
```
stored_group = (h1_hi << 7) | group_id
where:
  h1_hi    = h1 >> 8          // 0 or 1 (since h1 max = 256)
  group_id = port.group       // 1..127
```

For L/R h1 bytes:
```
stored_h1 = h1 & 0xFF        // low 8 bits of raw h1
```

### Encode pseudocode

```
// -- Overflow checks --
if group_count > 127:
    return make_overflow_tileop()    // poison: group ID won't fit in 7 bits
if o + i + 2*l + 2*r > 125:
    return make_overflow_tileop()    // poison: payload budget exceeded

// -- Header --
tileop[0] = 3 + o
tileop[1] = 3 + o + i
tileop[2] = 3 + o + i + l

// -- O/I groups: unchanged --
cursor = 3
for port in O.ports: tileop[cursor++] = port.group
for port in I.ports: tileop[cursor++] = port.group

// -- L/R groups: bit 7 = h1_hi --
for port in L.ports:
    h1_hi = port.h1 >> 8
    tileop[cursor++] = (h1_hi << 7) | port.group
for port in R.ports:
    h1_hi = port.h1 >> 8
    tileop[cursor++] = (h1_hi << 7) | port.group

// -- Skip to h_start (zero-padded R slots already 0) --
cursor = h_start

// -- L/R h1: raw low 8 bits --
for port in L.ports: tileop[cursor++] = port.h1 & 0xFF
for port in R.ports: tileop[cursor++] = port.h1 & 0xFF
```

### Decode pseudocode

```
// Parse header and derive counts (unchanged)
o_cnt = off_I - 3
i_cnt = off_L - off_I
l_cnt = off_R - off_L
r_cnt = (128 - off_R - l_cnt) / 2
h_start = off_R + r_cnt

// O/I: group bytes are plain group IDs (unchanged)
for i in 0..o_cnt: o_groups[i] = tileop[3 + i]
for i in 0..i_cnt: i_groups[i] = tileop[off_I + i]

// L face: extract group and reconstruct h1
for i in 0..l_cnt:
    g_byte  = tileop[off_L + i]
    h1_byte = tileop[h_start + i]
    l_groups[i] = g_byte & 0x7F          // 7-bit group ID
    l_h1[i]     = ((g_byte >> 7) << 8) | h1_byte   // 9-bit exact h1

// R face: same
for i in 0..r_cnt:
    g_byte  = tileop[off_R + i]
    h1_byte = tileop[h_start + l_cnt + i]
    r_groups[i] = g_byte & 0x7F
    r_h1[i]     = ((g_byte >> 7) << 8) | h1_byte
```

### Matching (compositor)

L/R matching changes from:

```rust
let h1 = 2 * stored_h1 + face_parity;   // BROKEN: 42.4% error
```

to:

```rust
let g_byte  = tileop[group_offset + i];
let h1_byte = tileop[h1_offset + i];
let group   = g_byte & 0x7F;
let h1      = ((g_byte as u16 >> 7) << 8) | h1_byte as u16;
```

The matching predicate `A.h1 == B.h1 + delta_h` is unchanged. Only the
decode path changes. No tile origin coordinates needed -- the decode is
purely local to the two bytes.

## 3. Why It Works

### Mathematical argument: the bit is free

The scheme rests on one structural observation: **group IDs and h1 overflow
occupy non-overlapping domains within the same byte**.

Group IDs at the operating point use values 1--21 (empirical max). The
theoretical max is bounded by the number of distinct connected components
that survive dead-end pruning and touch at least one L/R face. With ~2000
primes per tile and mean ~10 groups, the group count is structurally bounded
by the ratio of face perimeter to interior area times the component structure
of random lattice percolation. The 100K-tile census establishes 21 as the
practical ceiling; 127 provides a 6x safety margin.

Meanwhile, h1 = 256 contributes exactly one possible value to bit 7.
The two uses cannot collide because group IDs are capped at 127 (bit 7 = 0)
while h1_hi is either 0 (h1 <= 255) or 1 (h1 = 256).

### Why the parity scheme failed and this doesn't

The parity scheme assumed all face primes share a single h1 parity,
determined by the fixed face coordinate. This holds at depth = 0 (on the
face boundary line) because for `a^2 + b^2` to be odd (necessary for an
odd prime), `a` and `b` must have opposite parity. At depth = 0, the
cross-face coordinate is the face constant, so the along-face parity is
determined.

But port anchors can sit at depths 1--6, where the cross-face coordinate
varies. At each depth, the parity constraint still holds per-row, but
different depths can produce different along-face parities. Since a port
cluster spans multiple depths, its minimum-h anchor can end up at either
parity. The 42.4% failure rate reflects this: ports whose anchor happens
to sit at a depth where the parity differs from depth-0 decode incorrectly.

This proposal does not use parity at all. It stores raw h1 exactly, using
a bit from a field with proven excess capacity. The decode is arithmetic
(mask, shift, OR), not number-theoretic.

### Why no external state is needed

The parity scheme required the tile origin to compute `face_parity`. This
proposal decodes h1 from only the two bytes (group byte + h1 byte) that
are already allocated per L/R port. The compositor does not need to know
the tile's position in the grid to decode h1. This simplifies the CUDA
emit path (no coordinate dependency in the encode) and the compositor
read path (no GridDesc lookup for h1 decode).

## 4. Edge Cases and Failure Modes

### Group count exceeds 127

**Trigger:** A tile after dead-end pruning has > 127 surviving groups.

**Behavior:** The encoder emits the standard poison TileOp (all 0xFF).
The compositor treats it as a conservative bridge -- all adjacent ports
are unioned. This preserves search correctness (false connectivity only).

**Likelihood:** Effectively zero. The 100K census max was 21. The mean
is 9.8. Even the 1000-tile deep structural analysis shows the mega-group
phenomenon -- one large component absorbing half the tile -- which pushes
group counts *down*, not up. Reaching 128 would require a prime density
and connectivity pattern that does not exist at operating radii.

### Zero-padded R group slots

The parser derives `r_cnt` from the budget formula, which may exceed the
actual number of R ports. The excess R group slots are zero-filled (from
`make_empty_tileop()`). Under the new scheme:

- Padding group byte = 0x00
- Extracted group_id = 0x00 & 0x7F = 0 (the empty sentinel)
- Extracted h1_hi = 0x00 >> 7 = 0

This is correct: group 0 means unused slot, unchanged from today.

### Empty and dead tiles

Header semantics unchanged: `off_I = off_L = off_R = 3` with byte 3 = 0.
No L/R ports exist, so no bit-steal logic is exercised.

### Overflow sentinel collision

The overflow sentinel is all-0xFF. A maximally-packed L/R group byte
with group_id = 127 and h1_hi = 1 would be `(1 << 7) | 127 = 0xFF`.
However, this cannot appear as byte 0 of the TileOp because byte 0 is
`off_I` (a header byte), not a group byte. The overflow check is
`bytes[0] == 0xFF`, and `off_I` is always >= 3. No collision.

Within the payload, a value of 0xFF in an L/R group slot is legal data
(group 127, h1 >= 256). The parser distinguishes overflow from normal
tiles by byte 0, not by inspecting payload bytes.

### I/O group bytes

O/I ports use full 8-bit group IDs (range 1--255). This is unchanged.
But wait: can the same group appear on both an O/I face and an L/R face?
Yes -- multi-face groups are common (19.3% of all groups). So the group ID
must be consistent: the I/O byte stores the same group_id as the L/R byte's
lower 7 bits. Since group_id is capped at 127, the O/I byte will also
be in [1, 127]. The 7-bit cap applies globally, not just to L/R faces.

### h1 = 256 on the R face

L and R faces are symmetric. Face R primes have `h = tile_row`, same as
Face L. A port anchored at tile_row = 256 can occur on either face.
The encoding handles both identically.

### Compositor max_group_label scan

The `max_group_label()` function scans all group bytes to find the highest
group ID. It must be updated to mask off bit 7 when reading L/R group
bytes: `max_label = max(max_label, g_byte & 0x7F)` for L/R faces, or
unconditionally apply the mask (since O/I groups never set bit 7, the
mask is a no-op for them).

## 5. Poison/Overflow Rate Estimate

### From the bit-steal scheme itself

Additional poison from `group_count > 127`: **0 observed in 100,000 tiles**.
The max observed group count (21) is 6x below the new cap. Estimated
additional poison rate: **0.000%**.

### Compared to current h1 >> 1 scheme

The current scheme produces **0 poison** but **42.4% wrong h1 values**.
Wrong h1 means the compositor's matching predicate `A.h1 == B.h1 + delta_h`
uses a corrupted operand. For delta_h = 0 (aligned faces), the error cancels
on both sides and matching accidentally works. For delta_h != 0 (offset
faces), off-by-1 h1 errors cause missed matches (false disconnection) or
spurious matches (false connection), both of which corrupt the spanning
verdict.

This proposal eliminates all h1 decode errors.

### Compared to poisoning tiles with h1 = 256

If tiles containing any h1 = 256 port were poisoned instead of encoded,
the per-port rate is 0.18% but the per-tile rate depends on how many tiles
have at least one boundary port. At the shared boundary (tile_row = 256),
ports are common. The Codex sample found 3/64 tiles affected (~4.7%).
This is unnecessarily lossy; the bit-steal scheme handles these tiles
exactly at zero byte cost.

## 6. Comparison to Alternatives Considered

### A. h1 >> 1 parity compression (current spec)

**Rejected.** 42.4% round-trip failure rate. The parity assumption is
mathematically incorrect for multi-depth port anchors. Cannot be repaired
without changing port semantics.

### B. Per-port parity bit stored in slack bytes

**Rejected.** The packed layout guarantees no slack on full tiles (those
most likely to need every bit). Slack-dependent schemes fail at the exact
operating point where they matter most.

### C. Relative h1 encoding (delta from midpoint, previous port, etc.)

**Rejected.** Makes decode stateful -- each h1 depends on prior decoded
values or external reference points. Complicates the compositor hot path.
Worst case (port at h1 = 0 on a face with midpoint = 128) still needs a
full 8-bit delta, so it doesn't solve the 256 overflow. The fundamental
problem is 9 bits in 8, and delta encoding cannot squeeze 9 bits into 8
without an escape mechanism.

### D. Owner's wildcard: reverse-range group encoding

**Directionally correct.** The owner's intuition -- that the group byte has
unused capacity that can encode h1 overflow information -- is exactly right.
The bit-steal proposal is the mathematically precise realization:

- The wildcard suggested `255 - g` for "group g needs special h1." This is
  a subtraction-based signal in an ad hoc range.
- The bit-steal uses `(h1_hi << 7) | g`. This is a bitwise partition of the
  same byte into two fields with a fixed boundary.

The bit-steal is cleaner because:
1. The decode is a mask + shift, not a comparison + subtraction
2. The boundary between group space and h1 space is fixed (bit 7), not
   floating based on group count
3. No sentinel ambiguity -- the bit is either 0 or 1, not a range to check

### E. Increase TileOp to 192 bytes

**Rejected unless forced.** The 128-byte / 2-cache-line alignment is a
hard-won constraint. At 73.4M tiles, going to 192 bytes adds 4.7 GB of
GPU memory. The bit-steal scheme solves the problem at zero byte cost.

### F. Store full 16-bit h1

**Rejected.** Changes L/R port cost from 2 bytes to 3 bytes, reducing the
payload budget from `o+i+2l+2r <= 125` to `o+i+3l+3r <= 125`. At the
operating point with l + r ~ 25, this costs 25 extra bytes -- roughly
20% of the budget. Would increase the overflow/poison rate.

### G. Redefine h1 as boundary-depth-0 anchor

**Rejected.** This would "fix" the parity scheme by restricting port anchors
to the face boundary line (depth = 0), where parity is indeed constant.
But it changes what h1 means: ports whose minimum-h prime sits at depth > 0
would get a different h1. The compositor's matching predicate assumes h1
identifies the physical position of the port; changing its definition risks
silent matching errors across the shared boundary.

## Recommendation

Adopt group-bit steal: bit 7 of L/R group bytes carries h1_hi, bits 0--6
carry group_id. L/R h1 bytes store raw h1 & 0xFF. Group IDs capped at 127
globally (O/I and L/R alike).

Spec changes required:
1. Section 3.1: Remove the parity argument and half-step encoding. Replace
   with raw 9-bit h1 via group-bit steal.
2. Section 4.4: State that group labels are u7 (1--127), not u8 (1--255).
   Group count > 127 is a poison condition alongside the existing payload
   budget check.
3. Section 4.1: Update L/R group byte format to document the bit-7 semantics.
4. Section 11: Add `GROUP_LABEL_MAX = 127`.

Code changes required:
1. `encode.cpp`: `pack_h1()` becomes `h1 & 0xFF`. `append_face_groups()`
   for L/R ports ORs `(h1 >> 8) << 7` into the group byte. Overflow check
   adds `group_count > 127`.
2. `encode.cpp`: `face_h1()` and `decode_h1()` removed. Decode becomes
   `((g_byte >> 7) << 8) | h1_byte`.
3. `encode.cpp`: `max_group_label()` applies `& 0x7F` mask when reading L/R
   group bytes.
4. `tileop.py`: Corresponding Python changes.
5. `compose.py` / compositor: Remove `face_parity` / `decode_packed_h1`
   dependency on tile origin. Use the new 2-byte decode.
