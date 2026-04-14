---
title: Exact Raw-h1 Encoding for TileOp V2 via L/R Group-Bit Steal
date: 2026-04-10
engine: codex
type: design-note
status: complete
refs: [docs/tile_spec.md, docs/tile_operations.md, docs/supportive/2026-04-10-v2-cross-validation.md, docs/supportive/2026-04-09-port-overflow-census.md]
---

# 1. Empirical analysis

## First correction: the stated `0..270/271` range is not the real L/R `h1` domain

Per the current specs, L/R face ports are extracted only from **tile-proper** points, not halo points:

- `tile_operations.md` S3/S7: L/R face primes come from tile cols `0..6` and `250..256`, with `h = tile_row`
- `tile_spec.md` S3: `h1` for L/R is the along-face anchor measured from the tile origin corner

Therefore for L/R ports:

- `h1 = tile_row`
- `tile_row in [0, 256]`
- so legal L/R `h1` values are **exactly `0..256`**

`257..271` are not legal L/R anchors under the authoritative specs. The collar affects **depth**, not the along-face coordinate. If any code or note assumes `h1` can be `270/271` on L/R, that assumption conflicts with the specs and should be treated as wrong unless the spec is changed first.

## What the supplied docs say

- The parity-compression scheme is broken: `2026-04-10-v2-cross-validation.md` reports **42.4%** L/R round-trip failure because anchor parity is not face-constant once collar depth varies.
- Group-space is extremely sparse: `2026-04-09-port-overflow-census.md` reports **group count mean 9.8, max 21** over **100,000** operating-point tiles. Overflow is port-budget driven, not group-count driven.

## Additional operating-point measurement

I sampled 64 operating-point tiles with the Python validator at two compliant angles:

- 32 tiles near `45 deg` starting at `(601040640, 601040640)`
- 32 tiles near `30 deg` starting at `(736121088, 424999936)`

Observed L/R anchor distribution:

- total L/R ports: **1648**
- `h1 > 255`: **3 ports** = **0.182%**
- max observed `h1`: **256**
- observed counts near the top:
  - `250: 4`
  - `251: 9`
  - `252: 8`
  - `253: 18`
  - `254: 7`
  - `255: 14`
  - `256: 3`

All three `256` cases were on the **L face** and were legitimate tile-proper anchors at `tile_row = 256`. No sample produced `257+`, which matches the spec argument above.

Conclusion: the true problem is not “fit `0..271` into one byte.” The real problem is “fit **one extra bit** for the rare `h1 = 256` case, while also fixing the parity-decode bug.”

# 2. The proposed encoding

## Decision

Use the **high bit of each L/R group byte** as the ninth bit of raw `h1`, and store the low 8 bits of raw `h1` in the existing L/R `h1` byte.

That is:

- O/I sections: unchanged
- L/R `h1` byte: store `h1_lo = h1 & 0xFF`
- L/R group byte:
  - bits `0..6` = `group_id` in `1..127`
  - bit `7` = `h1_hi` (`1` iff `h1 >= 256`, else `0`)

This keeps TileOp at **128 bytes**, removes the broken parity decode entirely, and turns L/R `h1` into an exact 9-bit value.

## Exact byte layout

Header and payload section boundaries stay the same:

- Byte 0: `off_I`
- Byte 1: `off_L`
- Byte 2: `off_R`
- Payload order remains:
  - O groups
  - I groups
  - L groups
  - R groups
  - L raw-`h1` low bytes
  - R raw-`h1` low bytes

Only the interpretation of **L/R group bytes** changes.

### L/R group-byte format

For each live L or R port:

```text
stored_group = ((h1 >> 8) << 7) | group_id
             = (h1_hi << 7) | group_id
```

with:

- `group_id in [1, 127]`
- `h1_hi in {0, 1}`

The accompanying L/R `h1` byte is:

```text
stored_h1 = h1 & 0xFF
```

## Encode pseudocode

```text
for each L/R port:
    assert 0 <= h1 <= 256
    assert 1 <= group_id <= 127

    h1_hi   = h1 >> 8         // 0 or 1
    h1_lo   = h1 & 0xFF
    g_store = (h1_hi << 7) | group_id

    write g_store into the existing L/R group slot
    write h1_lo  into the existing L/R h1 slot
```

## Decode pseudocode

```text
for each L/R port:
    g_store  = read L/R group byte
    h1_lo    = read L/R h1 byte

    group_id = g_store & 0x7F
    h1_hi    = g_store >> 7
    h1       = (h1_hi << 8) | h1_lo
```

For O/I ports:

- decode exactly as today
- group bytes remain plain `u8 group_id`

# 3. Why it works

## Structural argument

The current format already allocates **2 bytes per L/R port**:

- 1 byte for group
- 1 byte for `h1`

The parity scheme tried to compress a 9-bit geometric field into 8 bits by assuming a missing bit was derivable. The cross-validation report shows that assumption is false.

The clean fix is to stop pretending the missing bit is derivable and instead store it explicitly in the only per-port field that has ample headroom: the L/R group byte.

## Why group-byte stealing is safe

Available evidence says group IDs are nowhere near 128:

- 100K-tile census: max group count **21**
- overflow cause: packed port budget, not group count

So capping live group IDs at `127` is not just acceptable; it is over 6x above the worst observed operating-point tile.

## Why this is better than the parity scheme

- It stores **raw `h1` exactly**
- It removes dependence on anchor depth parity
- It preserves deterministic decode with no external state beyond the TileOp itself
- It does not consume any new bytes

## Why this is better than reverse-range overloading

The project-owner wildcard idea was directionally right but too implicit. “Use the top of the group-byte range in reverse” introduces decode ambiguity and threshold rules.

This proposal is the disciplined version:

- reserve exactly **one bit**, not an ad hoc range
- decode is one mask and one shift
- no variable interpretation based on local group count
- no sentinel collisions

# 4. Edge cases and failure modes

## Group count exceeds 127

Failure mode:

- A tile needs a live group ID above `127`

Behavior:

- emit the existing poison/overflow TileOp

Why this is acceptable:

- 100K census observed max group count `21`
- no evidence of any operating-point tile remotely approaching `127`

This is the only new poison path introduced by the scheme.

## Padding in the derived R-group section

The parser already treats zero-filled derived R padding as unused. That remains valid:

- padding group byte = `0`
- padding low-byte = `0`

`group_id = 0` still means unused slot.

## Empty/dead tiles

Unchanged:

- empty/dead header semantics are unchanged
- no L/R live ports means no L/R bit-steal semantics are exercised

## Overflow sentinel

Unchanged:

- all-`0xFF` remains a distinct poison record
- no valid live L/R group byte uses `group_id = 127` plus `h1_hi = 1` as a sentinel; it is just data

## Future spec change that lets L/R `h1 > 256`

This plan assumes the authoritative specs remain correct that L/R ports are tile-proper and `h1 = tile_row in [0,256]`.

If the project later changes the L/R anchor definition to include halo rows or some different coordinate system, this encoding may no longer be sufficient. That is a real assumption, and it should be documented in the spec update.

# 5. Poison/overflow rate estimate

## From the new h1 rule itself

Estimated additional poison from `group_count > 127`:

- **0 observed in 100,000 operating-point tiles**
- observed max group count: **21**

Best current estimate: **effectively zero** at the operating point.

## Compared to the current parity scheme

Current parity scheme has:

- **0 byte cost**
- but **42.4% wrong L/R `h1` decodes**

That is not a tolerable “success rate”; it is semantic corruption.

This proposal trades that semantic corruption for a new poison condition that current data suggests is negligible.

## Compared to an `h1 >= 256` poison rule

From the 64-tile sample:

- `h1 = 256` occurred on **3 / 1648** L/R ports = **0.182%**

If tiles with any `h1 = 256` were poisoned, the tile-level poison rate would be small but nonzero and entirely unnecessary. This proposal preserves those tiles exactly at zero byte cost.

# 6. Comparison to alternatives considered

## A. Keep `h1 >> 1` parity compression

Rejected.

Killed by:

- cross-validation already proves it is wrong on **42.4%** of L/R ports
- the missing bit is not derivable from tile origin parity

## B. Store per-port parity bits in slack/pad bytes

Rejected.

Killed by:

- the packed layout does not guarantee enough slack on full tiles
- any scheme that depends on “usually there is padding” fails exactly where compression matters most

## C. Escape-code high `h1` using the top of group-byte space in reverse

Rejected in raw form.

Killed by:

- harder decode rules
- implicit dependence on group-count thresholds
- more room for encoder/parser disagreement

Kept in refined form as this proposal’s fixed **high-bit steal**.

## D. Encode `h1` relative to face midpoint, previous port, or group-local origin

Rejected.

Killed by:

- decode becomes stateful across ports or groups
- harder to make spec-tight and order-independent
- worst-case still needs an escape mechanism

This is complexity without a compensating byte win.

## E. Redefine `h1` to mean boundary-depth-0 anchor only

Rejected.

Killed by:

- changes the semantics of a port anchor
- risks breaking compositor identity across tiles
- tries to rescue the parity trick by changing the geometry instead of fixing the encoding

## F. Use the L/R group-byte high bit as `h1_hi`

Accepted.

Why it wins:

- exact raw `h1`
- no size increase
- local, stateless decode
- aligns with observed data: group IDs are cheap, `h1=256` is rare
- simplest reversible spec change with the smallest blast radius

# Recommendation

Adopt **raw 9-bit L/R `h1` encoded as `(group_hi_bit, h1_lo_byte)`**, and update the specs to say explicitly:

- L/R legal `h1` domain is `0..256`
- L/R group bytes use bit 7 as `h1_hi`
- L/R group IDs are therefore capped at `127`
- `group_count > 127` is a poison condition

This fixes the real bug, preserves the 128-byte TileOp, and makes the rare `h1 = 256` case exact instead of special-cased or poisoned.
