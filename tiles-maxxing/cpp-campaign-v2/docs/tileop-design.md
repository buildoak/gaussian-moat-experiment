---
title: TileOp Pipeline Design (Phase 2 W5 Spec)
date: 2026-04-20
engine: coordinator
type: design-note
status: complete
refs:
  - methodology/lemmas_v2/campaign-blueprint.md
  - methodology/lemmas_v2/tile-operator-definition-v-claude.md
  - methodology/lemmas_v2/cpp-campaign-v2-execution-plan.md
---

# TileOp Pipeline Design

Binding specification for the Phase 2 `process_tile` / TileOp encoder
worker (plan M4). All sections below are load-bearing: a deviation
without a blueprint-grounded argument is a correctness bug. Blueprint
section numbers are cited inline.

## 1. Pipeline Overview

For each active `TileCoord`, the worker executes this sequence,
producing exactly one 256-byte `TileOp` record:

1. **Sieve** — `sieve_tile(coord, constants)` returns a `vector<Prime>`
   over the halo-expanded `SIDE_EXP × SIDE_EXP` region (octant-clipped).
2. **Local UF** — build `DSU(primes.size())`; for every pair of primes
   with squared lattice distance `(Δa)² + (Δb)² ≤ K_SQ`, call
   `unite()`. Smaller-root-wins tiebreak (`union_find.h` docblock);
   determinism is load-bearing (plan risk R2).
3. **Geo flags per prime** — for each prime, compute `is_inner_prime` and
   `is_outer_prime` via the two-stage test (blueprint §2).
4. **Dense-remap** — compact raw DSU roots into a dense `[0, max_label)`
   label space (§2 below). Overflow trips `OVERFLOW_BIT`.
5. **Accumulate group flags** — OR each prime's geo bits into its dense
   label's `inner_flags` / `outer_flags` bit.
6. **Face-strip UF** — for each face `F ∈ {I, O, L, R}`, run a
   sub-DSU over the F-strip prime subset using the same `G_full` edges
   (§3 below). Each sub-DSU component is one port.
7. **Canonical positional port sort** — order ports within each face by
   lex `(h, p⊥)` primary, `(p⊥, h)` secondary (§4 below).
8. **256 B encode** — fixed-offset pack per `tileop.h` layout (§5).

## 2. Dense-Remap Algorithm

Raw DSU roots are integers in `[0, primes.size())` ⊆ `[0, MAX_PRIMES_GPU)`.
Wire labels must live in `[1, 128]`. Dense-remap (blueprint §6.2,
Amendment 1):

```
dense_label: map<int, uint8_t>       // raw_root -> dense label
next_label = 0
for (prime_idx = 0; prime_idx < primes.size(); ++prime_idx) {
    raw_root = dsu.find(prime_idx)
    if (raw_root not in dense_label) {
        if (next_label >= 128) {
            // Group-count overflow — blueprint §10
            emit_overflow_and_return()
        }
        dense_label[raw_root] = next_label + 1   // 1-indexed
        next_label += 1
    }
}
max_label = next_label
```

**No bit-stealing** (plan §6.3 rule #2). The 128-group budget is the
wire-format hard cap; if `max_label > 128` we set `OVERFLOW_BIT` and zero
every payload byte. Iteration order is `prime_idx` ascending — this is
the canonical enumeration order from the sieve (lex `(a, b)`) and
guarantees identical dense-label assignment across threads and runs.

## 3. Face-Strip UF

For each face `F ∈ {I, O, L, R}`, extract the subset of primes whose
`p⊥ ∈ [0, C]` (blueprint §5.4 — depth of the face strip equals the
collar). Build a sub-DSU over exactly those primes:

```
face_primes_F = [p for p in primes if on_face_strip(p, F)]
face_dsu = DSU(face_primes_F.size())
for (i, pi) in face_primes_F:
    for (j, pj) in face_primes_F where j > i:
        if (pi.a - pj.a)^2 + (pi.b - pj.b)^2 <= K_SQ:
            face_dsu.unite(i, j)
```

Face-strip components are ports. `n[F] = number of distinct face_dsu
roots`. Consecutive-pair greedy clustering is forbidden (plan §6.3 rule
#3, inherited from v1's broken 1-D scan, `face_extract.cpp:127–162`).

**Port → global-UF label mapping.** For each port, read any member
prime's dense label (via step 4 above). All primes in a face-strip
component belong to the same `G_tile` UF component (face edges are a
subset of full edges), so the dense label is identical for every member.
That dense label is the byte written into `face_groups`.

## 4. Canonical Positional Port Sort

Blueprint §5.4. Per face, compute each port's representative `(h, p⊥)` —
use the member prime with the smallest `(h, p⊥)` lex pair. Then sort
ports:

```
sort(ports, compare(a, b) {
    if (a.h != b.h) return a.h < b.h;        // primary: h ascending
    if (a.p_perp != b.p_perp) return a.p_perp < b.p_perp;  // secondary
    // Chain-order independent tiebreak. Two ports with identical
    // (h, p_perp) pair must compare equal AT THE PORT LEVEL. This is
    // structurally impossible in a connected graph (two distinct
    // components cannot share a representative prime). If it occurs,
    // the upstream UF is broken — assert(false).
})
```

`(p⊥, h)` acts as the documented secondary tiebreak (BACKLOG B9). In
practice the primary `h` already fully orders ports within a face; the
secondary exists only for adversarial test cases where two ports share
the same `h` slice. Chain-order independence is verified by the
determinism-by-repetition test (plan §5, Amendment 6).

## 5. 256 B Packed Encode

Byte layout is **locked** (`include/campaign/tileop.h` `static_assert`s):

| Offset | Size | Field | Semantics |
|---|---|---|---|
| 0   | 4   | `n[4]` | `n[0]=N_I, n[1]=N_O, n[2]=N_L, n[3]=N_R` |
| 4   | 192 | `face_groups[192]` | Dense labels per port, face order I,O,L,R |
| 196 | 16  | `inner_flags[16]`  | Bit `(g-1)` = inner flag for dense label `g` |
| 212 | 16  | `outer_flags[16]`  | Bit `(g-1)` = outer flag for dense label `g` |
| 228 | 1   | `tile_flags`       | bit0 OVERFLOW, bit1 EMPTY, bit2 TOWER_CLOSING |
| 229 | 27  | `reserved[27]`     | MUST be zero-init |

Encode order: zero the entire struct first, then fill. Per-face ports
are written contiguously via the prefix-sum offset (`face_offset()`
inline in `tileop.h`). `static_assert(sizeof(TileOp) == 256)` is the
parity anchor.

## 6. Overflow Semantics

Exactly one overflow path (plan §6.3 rule #4 — no empty-TileOp
substitution). Trigger if **either**:
* `sum(n) > MAX_PORTS_PER_TILE (192)`, or
* `max_label > MAX_GROUPS_PER_TILE (128)`.

On trigger:

```
memset(out, 0, sizeof(TileOp))
out->tile_flags = OVERFLOW_BIT
return
```

Downstream: the compositor treats an `OVERFLOW_BIT` tile as
conservatively **SPANNING** (forces its root to `REACH_BOTH`, blueprint
§10). Never as empty. Never as "skip".

## 7. Determinism Invariants

Same input bytes → same output bytes across:
* any OpenMP thread count (per-tile independence, plan Q2),
* any run of the same binary,
* any compiler / libc that satisfies standard-layout POD + LP64.

Test gate: `OMP_NUM_THREADS ∈ {1, 12}` × N=3 repetitions produce
bit-identical snapshot payload bytes (plan M6 gate, Amendment 6).
Chain-order independence for ties in the port sort is exercised by a
synthetic tied-`(h, p⊥)` input in `test_tileop.cpp`. UF smaller-root-wins
plus iteration in `prime_idx` ascending order is the single source of
determinism — any optimization that reorders primes must preserve this
invariant.
