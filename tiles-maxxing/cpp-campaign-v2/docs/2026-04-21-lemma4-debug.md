# Lemma 4 Port-Count Mismatch Debug (2026-04-21)

## Failure

```
build-k36/campaign_main --k-sq=36 --r-inner=80000000 --r-outer=80008192 \
    --region /tmp/grid-opt-check/region-100.json --out /tmp/ignore.bin --threads=1
→ Error in compositor: Lemma 4 port-count mismatch on I/O
```

Column distribution `{0: 33, 1: 33, 2: 33, 3: 1}`, all contiguous in j within each column.

## Phase 1 — Failure site

Temporary stderr logging added at `src/compositor.cpp` `match_io` before the
`require_port_count_equal` call printed:

```
[DBG] I/O mismatch: A=(0,312508) n[O]=27 flags=0x0 ; B=(0,312509) n[I]=0 flags=0x1
```

- **Tile pair:** `A = (0, 312508)` ↔ `B = (0, 312509)`.
- **Face:** I/O (within-column vertical stitch between tile at `j` and `j+1`).
- **Throw site:** `src/compositor.cpp:41` inside `require_port_count_equal`, reached from `State::match_io` at `src/compositor.cpp:189`.
- **Decisive evidence:** `B.tile_flags = 0x1 = OVERFLOW_BIT` (per `include/campaign/constants.h:140`). The overflow tile carries a zero-payload TileOp (all `n[f] = 0`) but its non-overflow neighbor `(0, 312508)` carries `n[O] = 27`. Equal-count check fails.

Confirmation probes:
- Single-tile region `{(0, 312509)}` → VERDICT SPANNING (tile is overflow).
- Single-tile region `{(0, 312508)}` → VERDICT MOAT (tile is normal).
- Two-tile region `{(0, 312509), (0, 312510)}` → VERDICT SPANNING (both overflow, zero-vs-zero I/O count matches).
- Twenty-one-tile column region `j ∈ [312500..312520]` → same I/O mismatch at the same site.

## Phase 2 — Protocol understanding

Per-tile encoder (`src/tileop.cpp:151-155, 238-240, 260-262, 268-270`) returns
`overflow_tileop()` — a zero-initialized `TileOp` with only `tile_flags =
OVERFLOW_BIT`. All `n[face] = 0`, all `face_groups[] = 0`.

Compositor ingest in `src/compositor.cpp`:
1. `mark_tile_ports` (line 149-154) treats `OVERFLOW_BIT` tiles as a special
   case: it sets `spanning_detected = true` and calls `mark_all_present_ports`
   with `kReachBoth`. Because the payload is zero, the inner loop in
   `mark_all_present_ports` iterates nothing — the **spanning latch itself**
   is what captures the verdict.
2. `match_io` / `match_lr` (lines 183-216) are called *after* `mark_tile_ports`
   to stitch ports. They call `require_port_count_equal(a.n[...], b.n[...])`
   with **no awareness** of `OVERFLOW_BIT` on either side.

Design intent (docs/tileop-design.md:155-157 and docs/final-math-audit.md:157-159):

> Downstream: the compositor treats an OVERFLOW_BIT tile as conservatively
> SPANNING (forces its root to REACH_BOTH, blueprint §10). Never as empty.
> Never as "skip".

> Because overflow TileOps have zero `n[]` and zero `face_groups`,
> `mark_all_present_ports` iterates nothing — the `spanning_detected = true`
> is what locks the verdict.

The documented intent is that an overflow tile's stitching against normal
neighbors *should not participate in port-ordinal matching* at all — the
spanning flag is already set. The implementation missed that step: `match_io`
/ `match_lr` still invoke the equal-count guard, which spuriously throws for
any overflow ↔ normal adjacency.

## Phase 3 — Root cause

`Compositor::State::match_io` (`src/compositor.cpp:183-196`) and
`Compositor::State::match_lr` (`src/compositor.cpp:198-216`) enforce the
Lemma 4 equal-port-count predicate via `require_port_count_equal` without
checking `OVERFLOW_BIT` on either the A or B side. When a per-tile encoder
emits an overflow tile (e.g., `(0, 312509)` at R=80M, K=36 trips the
`max_label > 128`, `n[f] > 255`, or `sum(n) > 192` budget), that tile's
`n[I] = n[O] = n[L] = n[R] = 0` no longer matches the port count of any
non-overflow neighbor. The overflow already latched `spanning_detected` in
`mark_tile_ports`, but the subsequent stitching calls throw before the
verdict can be returned.

This never surfaced in existing coverage:
- **Golden 5-tile regions** (`goldens/5tile-spec.json`,
  `goldens/5tile-k40-spec.json`) pick disconnected tiles — no adjacency
  stitching at all.
- **`OverflowTileConservativelySpans` test** (`tests/test_compositor.cpp:140`)
  uses a single-tile grid — no neighbor to match against.
- **R=1M full-octant** has few enough primes per tile that no tile trips the
  192-port / 128-group budget.

R=80M K=36 tiles near `j ≈ 312509` in column `i=0` push past the 128-group
cap. The sparse region-100 is the first configuration that both (a) lands
on an overflow-producing tile and (b) asks the compositor to stitch that
tile with a non-overflow neighbor. Full-octant at R=80M would hit the same
bug — it was never attempted.

## Phase 4 — Fix decision

**Small.** 10 net lines in a single file (`src/compositor.cpp`), two symmetric
guards at the top of `match_io` and `match_lr`. No API change, no header
touch, no algorithmic rework.

## Phase 5 — Fix + validation

### Diff summary (`src/compositor.cpp`)

```diff
  void match_io(std::int64_t a_idx, std::int64_t b_idx,
                const TileOp& a, const TileOp& b) {
+   // Overflow tiles carry a zero-payload TileOp and have already latched
+   // `spanning_detected` in `mark_tile_ports`. Skip port-ordinal matching
+   // with either neighbor — the conservative SPANNING verdict is already
+   // recorded, and an overflow tile's zero n[] would otherwise spuriously
+   // fail the Lemma 4 equal-count check against a normal neighbor
+   // (blueprint §10: OVERFLOW_BIT forces SPANNING, never participates in
+   // stitching).
+   if ((a.tile_flags & OVERFLOW_BIT) || (b.tile_flags & OVERFLOW_BIT)) {
+     return;
+   }
    const int face_o = face_index(Face::O);
    ...

  void match_lr(...) {
    assert_not_side_exposed_lr_input(grid, a_coord, Face::R);
    assert_not_side_exposed_lr_input(grid, b_coord, Face::L);
+   // See `match_io` for the overflow skip rationale (blueprint §10).
+   if ((a.tile_flags & OVERFLOW_BIT) || (b.tile_flags & OVERFLOW_BIT)) {
+     return;
+   }
    const int face_r = face_index(Face::R);
    ...
```

### Soundness argument

Skipping the stitching for overflow adjacencies cannot produce a false MOAT:

1. The overflow tile's `mark_tile_ports` has already set
   `spanning_detected = true`.
2. `Compositor::finalize()` returns `kSpanning` whenever
   `spanning_detected` is true, regardless of per-group reach.
3. Therefore any region containing an overflow tile emits SPANNING. The
   skipped unions only *fail to widen* reach bits into overflow-adjacent
   roots — but those roots are already globally latched to SPANNING via
   the flag. This is exactly the "conservative SPANNING" contract the
   blueprint and final-math-audit §27 name as the intended behavior.

The `Compositor.PortCountMismatchFails` test (which constructs two
non-overflow tiles with mismatched port counts) still passes — the guard
only triggers when at least one side carries `OVERFLOW_BIT`.

### Gate results

| Gate | Result |
|---|---|
| 1. 5-tile K=36 golden byte-identical | YES (5/5 tiles identical) |
| 2. 5-tile K=40 golden byte-identical | YES (5/5 tiles identical) |
| 3. `ctest build-k36-tests` (excl. Pass A's in-flight geo_tests) | 88/88 pass |
| 4. N=100 region-100 (K=36, 1T) | Exit 0, VERDICT SPANNING |
| 5. 1T vs 12T byte-identical (N=100, N=1000) | YES (100/100 and 1000/1000 identical) |
| 6. N=100 + N=1000 timing | measured (below) |

Note on gate 3: two `GeoTests` tests (`InnerBandCorners`, `OuterBandCorners`,
and the skipped `NonSquareKUsesCeilBoundary`) fail in the current working
tree. These are caused by Pass A's in-flight edit to `src/geo_tests.cpp`
and `include/campaign/geo_tests.h` which narrows the predicate from the
`ceil_isqrt(K)`-band to the spec norm-form — the test fixtures in
`tests/test_geo_tests.cpp` still expect the old band semantics. This Pass B
debug is forbidden from touching `geo_tests.{cpp,h}` or `tests/*`, so the
stale test fixtures must be updated as part of Pass A, not here. Confirmed
by `git diff` that the failing tests do not exercise compositor code paths.

### Timing (post-fix)

| Config | t_tile_loop | ms/tile |
|---|---|---|
| N=100, 1T | 1.496 s | 14.96 ms/tile |
| N=100, 12T | 0.990 s | 9.90 ms/tile |
| N=1000, 1T | 15.301 s | 15.30 ms/tile |
| N=1000, 12T | 2.452 s | 2.45 ms/tile |

N=1000 at 12T hits 6.24x speedup vs 1T — reasonable dynamic OpenMP scaling
at this tile count. N=100 at 12T only reaches 1.51x (schedule overhead
dominates at small counts).

### Files touched

- `src/compositor.cpp` — 10 net lines, two symmetric overflow guards in
  `State::match_io` and `State::match_lr`.

No other files modified. Temporary stderr debug logging added during
Phase 1 has been fully removed.
