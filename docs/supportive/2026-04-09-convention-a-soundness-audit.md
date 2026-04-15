---
title: Convention A Soundness Audit
date: 2026-04-09
engine: codex
type: audit
status: complete
refs: [docs/tile_spec.md, docs/tile_operations.md, docs/compositor_spec.md]
---

Convention A is **not sound** for moat disconnection proofs as currently specified.

The critical flaw is on aligned I/O boundaries. The specs claim positional matching
is exact because both tiles "see the same set of primes in the 7-deep collar" and
therefore produce identical port lists. Under half-open ownership, that premise is
false.

Let tile A own rows `a_lo..a_lo+255` and tile B own rows `a_lo+256..a_lo+511`.
Then:

- A.O face primes come from tile-proper rows `a_lo+249..a_lo+255`.
- B.I face primes come from tile-proper rows `a_lo+256..a_lo+262`.

These are disjoint 7-row strips. The tiles do share halo visibility on the
15-row overlap `a_lo+249..a_lo+263`, and union-find runs on that whole overlap,
but face extraction does **not**. Therefore A and B do not, in general, emit the
same face-port partition.

Concrete counterexample in the shared 15-row slab (write coordinates relative to
the boundary, with `x` perpendicular to the boundary and `y` along it):

- `P1 = (255, 0)`
- `P2 = (255, 12)`
- `R  = (256, 6)`

Assume these are Gaussian primes and there are no other primes in the relevant
slab. Then:

- `dist^2(P1,R) = 1^2 + 6^2 = 37 <= 40`
- `dist^2(P2,R) = 1^2 + 6^2 = 37 <= 40`
- `dist^2(P1,P2) = 12^2 = 144 > 40`

So in the real Gaussian-prime graph, `P1` and `P2` are connected across the
boundary through `R`.

What each tile computes:

- Tile A: `P1` and `P2` are face primes on A.O. `R` is halo-only, but UF sees it,
  so A gets **two O-face ports** with the **same group label**.
- Tile B: `R` is a face prime on B.I. `P1` and `P2` are halo-only, so B gets
  **one I-face port**.

Thus A.O has 2 ports and B.I has 1 port. The positional-equality claim in
`tile_spec.md` S5.2 is false. The two tiles do not produce identical ordered
port lists.

This already invalidates the correctness argument for aligned I/O matching. It
also produces a direct soundness failure once dead-end pruning is applied:

- On B, that group touches exactly one face (I) in exactly one port, so
  `tile_operations.md` S7.4 prunes it.
- On A, the same connected component touches one face (O) but in two ports, so it
  survives.

After pruning, the real cross-boundary connection still exists, but B contributes
no matching I-port. The compositor therefore has no way to encode the A<->B
connection. A spanning path can be lost.

Answers to the posed questions:

1. Direct edge `P1=(255,y1)` to `P2=(256,y2)` can be detected in simple cases,
   because both tiles' UF sees both primes. But detection is **not guaranteed**
   by the current face/port protocol.
2. Transitive connections through bridge primes are exactly where the failure
   occurs. The counterexample above is a minimal transitive bridge.
3. Port-slot matching is not sound on aligned I/O faces. The tiles compute ports
   independently from different tile-proper strips, so counts and slot order need
   not agree.
4. Therefore Convention A, with half-open tiles plus the current face extraction
   and positional I/O compositor, is unsound for moat proofs.

Minimal repair direction: for every shared boundary, both tiles must export a
representation of the **same overlap object** before composition. For example,
define boundary ports on the common overlap slab (or store I/O h1 and match by an
actual shared geometric predicate over a canonical boundary representation).
