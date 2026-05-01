---
title: Ports-After Mismatch Investigation
date: 2026-04-09
engine: codex
type: investigation
status: complete
refs: [docs/tile_spec.md, docs/tile_operations.md, tile-cpp/src/face_extract.cpp, tile-validator/ports.py]
---

## Verdict

The hypothesis is **confirmed**.

- C++ `cluster_face_ports()` in `tile-cpp/src/face_extract.cpp` clusters by
  comparing each sorted face prime only against the immediately previous prime.
- Python `_cluster_face_primes()` in `tile-validator/ports.py` uses the stronger
  rule "new prime is in the current cluster if it connects to **any** earlier
  prime in that cluster within the `h <= 6` reverse scan window."
- Replacing only the Python clustering rule with the C++ consecutive rule
  reproduces the observed C++ `ports_before_pruning` and `ports_after_pruning`
  totals on all three operating-point tiles exactly.

So the measured mismatch is explained by clustering semantics, not by sieve or
union-find divergence.

## What The Code Does

### C++

`tile-cpp/src/face_extract.cpp` sorts face primes by `(h, depth, row, col)` and
then applies:

```cpp
dx = face_primes[i].tile_col - face_primes[i - 1].tile_col;
dy = face_primes[i].tile_row - face_primes[i - 1].tile_row;
if (dx*dx + dy*dy > K_SQ) start_new_port();
```

This computes connected components of the **path graph on consecutive sorted
primes**. It does **not** test whether the new prime connects to any earlier
member besides `i - 1`.

### Python

`tile-validator/ports.py` sorts by the same key shape, but `_connected_to_cluster`
scans backward through the whole current cluster:

- stop when `prime.h - other.h > floor(sqrt(k)) = 6`
- otherwise keep checking earlier members until one satisfies `dist^2 <= 40`

This allows:

- previous-prime edge to fail
- earlier-prime edge to succeed
- cluster to stay merged anyway

That is strictly stronger than the C++ rule and can only produce the same number
of ports or **fewer**.

## Spec Analysis

The authoritative specs currently side with the **C++** rule.

`docs/tile_spec.md` Section 3:

- "A port is a maximal connected cluster of Gaussian primes on a tile face,
  where two **consecutive face primes** (ordered by along-face position) belong
  to the same port iff their squared Euclidean distance `<= k`."

`docs/tile_operations.md` Section 7.2:

- "Ports are maximal contiguous clusters of face primes on the same face, where
  consecutive primes are within `sqrt(k)` distance along the face axis."
- "Port clustering: two **adjacent face primes** (sorted by `h`) are in the same
  port iff their squared distance `<= k`."

Per the authority chain, that means:

- the canonical port relation is defined on **adjacent/consecutive** sorted face
  primes
- the Python full-cluster scan is **not spec-compliant**
- the C++ implementation is the correct one under the current spec

Important nuance: the phrase "maximal connected cluster" is resolved by the next
sentence. The spec does **not** define ports as connected components of the full
2D face-prime proximity graph. It defines them by the consecutive-prime rule.

## Operating-Point Evidence

I ran:

- `python3 tile-validator/sample.py`
- `tile-cpp/build/run_tile <a_lo> <b_lo>` on the same three tiles
- a one-off Python analysis that reused the validator sieve + UF and swapped
  only the clustering rule between:
  - current Python full-cluster scan
  - C++ consecutive-only scan

### Total Counts

| Tile | Python before | C++ before | Delta | Python after | C++ after | Delta |
|------|---------------|------------|-------|--------------|-----------|-------|
| 45 deg `(601040640, 601040640)` | 68 | 70 | +2 | 52 | 54 | +2 |
| 30 deg `(736121088, 424999936)` | 72 | 75 | +3 | 61 | 64 | +3 |
| 15 deg `(820888320, 220000000)` | 56 | 59 | +3 | 48 | 51 | +3 |

The consecutive-only emulation matched the C++ totals exactly on all three
tiles.

### Per-Face Impact In Spec/C++ Face Names

These are the per-face counts after dead-end pruning, reported in the spec/C++
face convention (`I, O, L, R`):

| Tile | Python/spec-full | C++/consecutive | Extra ports from C++ |
|------|-------------------|-----------------|----------------------|
| 45 deg | `I=12 O=14 L=12 R=14` | `I=12 O=15 L=12 R=15` | `O:+1, R:+1` |
| 30 deg | `I=17 O=18 L=15 R=11` | `I=18 O=19 L=16 R=11` | `I:+1, O:+1, L:+1` |
| 15 deg | `I=13 O=12 L=13 R=10` | `I=14 O=12 L=14 R=11` | `I:+1, L:+1, R:+1` |

Notes:

- 30 deg overflows under **both** rules because at least one face still exceeds
  16 ports after pruning.
- 45 deg and 15 deg do **not** overflow under either rule; the difference is
  purely extra surviving ports.

### Before-Pruning Per-Face Impact In Spec/C++ Face Names

| Tile | Python/spec-full | C++/consecutive | Extra ports from C++ |
|------|-------------------|-----------------|----------------------|
| 45 deg | `I=15 O=19 L=15 R=19` | `I=15 O=20 L=15 R=20` | `O:+1, R:+1` |
| 30 deg | `I=19 O=21 L=16 R=16` | `I=20 O=22 L=17 R=16` | `I:+1, O:+1, L:+1` |
| 15 deg | `I=16 O=13 L=15 R=12` | `I=17 O=13 L=16 R=13` | `I:+1, L:+1, R:+1` |

## Can The Counterexample Actually Occur At R ~ 850M?

Yes. It occurs on all three operating-point tiles.

Concrete witnesses, written in **tile-relative** coordinates `(tile_row, tile_col)`
with distances computed in the full 2D lattice metric:

### 45 deg, Face O

- `P1 = (256, 89)`
- `P2 = (250, 89)`
- `P3 = (254, 95)`

Distances:

- `dist^2(P1, P2) = 36`
- `dist^2(P1, P3) = 40`
- `dist^2(P2, P3) = 52`

Sorted by `(h, depth)` on Face O:

- `P1` then `P2` then `P3`

Result:

- Python keeps one cluster because `P3` connects back to `P1`
- C++ splits at `P2 -> P3` because only the immediately previous prime is tested

### 30 deg, Face I

- `P1 = (0, 91)`
- `P2 = (4, 91)`
- `P3 = (0, 97)`

Distances:

- `dist^2(P1, P2) = 16`
- `dist^2(P1, P3) = 36`
- `dist^2(P2, P3) = 52`

### 15 deg, Face I

- `P1 = (5, 216)`
- `P2 = (0, 217)`
- `P3 = (5, 222)`

Distances:

- `dist^2(P1, P2) = 26`
- `dist^2(P1, P3) = 36`
- `dist^2(P2, P3) = 50`

These are real operating-point witnesses, not synthetic examples.

## Which Implementation Should Win?

Adopt the **C++ consecutive rule** and change the Python validator to match it.

### Why

1. **Spec compliance**

   The spec explicitly says consecutive/adjacent face primes define the port
   partition. The Python rule contradicts that.

2. **Validator role**

   The validator exists to check the canonical pipeline, not to invent a stronger
   clustering semantics. A validator mismatch against spec-faithful C++ is a
   validator bug.

3. **Proof pipeline stability**

   The current analytic reasoning around port splitting is expressed in terms of
   consecutive gaps:

   - `docs/tile_spec.md` Section 3.1 says intra-port gaps are `2, 4, 6`
   - it then derives the minimum splitting gap and the per-face port bound from
     that consecutive-gap model

   Switching to the Python rule would require re-specifying ports as connected
   components of a richer face graph and then re-auditing the bound arguments.
   That may or may not be a good future design, but it is **not** the current one.

4. **Measured behavior**

   The only observed operating-point mismatch is extra ports on the C++ side,
   exactly as predicted by the consecutive rule. No evidence suggests the C++
   implementation is miscomputing its own intended semantics.

## Recommended Fix

Change `tile-validator/ports.py::_cluster_face_primes()` to match the C++
consecutive scan:

- keep the existing sort order
- start a new port on the first face prime
- for each later prime, compare only with the immediately previous sorted prime
- split the port iff `dist^2 > k`

Also update comments and add regression tests.

Recommended regression coverage:

- a direct unit test on the witness shape
  - example: `(0,91)`, `(4,91)`, `(0,97)` on Face I with `k=40`
  - expected: **two** ports under the canonical rule
- operating-point snapshot assertions for the three tiles above
  - expected `ports_after_pruning`: `54`, `64`, `51` in the C++/spec pipeline

## CUDA Implications

The consecutive rule is the better CUDA target.

- It is a single forward scan over already-sorted face primes.
- State per face is minimal: previous prime coordinates, current port anchor,
  current port count.
- It is deterministic and O(n).
- It avoids the variable-length reverse scan through the current cluster that the
  Python rule requires.

The Python full-cluster rule is noticeably less kernel-friendly:

- it needs dynamic cluster history
- worst-case work per prime grows with cluster length
- it complicates any warp-synchronous extraction path

So the spec-compliant choice is also the more straightforward CUDA choice.

## Additional Note

While doing this investigation I found that the current Python
`collect_face_primes()` face labels are transposed relative to the spec/C++
(`I <-> L`, `O <-> R`). That does not affect the aggregate port-count mismatch
analyzed here, but it does matter for any per-face comparison and should be
tracked separately.
