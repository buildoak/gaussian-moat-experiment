# k²=40 Fat-Stripe Experiment — 2026-03-25/26

## Summary

Fat-stripe annular probes on k²=40 Gaussian integers reveal a wide blocked region starting between R=800M and R=850M, extending past R=1.3B with no reconnection observed. The moat is isotropic — all tested angles (5° through 45°) show identical blocking at R=1.05B. This is the first k² value where the blocked annulus persists across such a large radial range without an upper boundary in sight.

## Method

- **Tool:** fat-stripe crate (`tile-probe/crates/fat-stripe`)
- **Spanning detection:** tile-based union-find with horizontal/vertical composition
- **Probe geometry:** 128K × 128K annular strips (64×64 tiles, `tile_width=2000`)
- **Spanning verdict:** radius-based — component has face ports at both `r_inner` and `r_outer`
- **Off-axis fix (commit bda7580):** geometric radii for spanning thresholds when `b_min > 0`
- **k²=40:** collar=7, 64 backward offsets at moat radius

## Hardware

| Platform | CPU | RAM | Notes |
|----------|-----|-----|-------|
| Mac Mini | Apple M-series | 24 GB | ~76 ms/tile at R~1B |
| Jetson Orin Nano | ARM A78AE 6-core | 7.4 GB | ~328 ms/tile at R~1B |

## Calibration Probes (Mac, session e74d3eb5)

| k² | R | Verdict | Tiles | Wall time | Spanning | Degree (total) |
|----|---|---------|-------|-----------|----------|----------------|
| 26 | 1.02M | NOT BLOCKED | 1530 | 1.6s | 41 components | 2.0 |
| 32 | 2.82M | BLOCKED | 449 | 34s | 0 | 3.92 |
| 36 | 80M | BLOCKED | 2373 | 4m 5s | 0 | 3.97 |
| 40 | 550K | NOT BLOCKED | — | — | 1 | — |
| 100 | 1.05B | NOT BLOCKED | — | — | 1 | — |

k²=26 confirms reachability below the moat. k²=32 and k²=36 confirm blocking at their known moat radii. k²=40 at R=550K is well below the moat — connected as expected. k²=100 at R=1.05B confirms the region is only blocked for k²=40, not universally.

## k²=40 Discovery Probes (Mac, session e74d3eb5)

### Radial probes (θ=0°)

All probes at `b_max=128K`, 4096 tiles.

| R | Verdict | Spanning | Wall time |
|---|---------|----------|-----------|
| 1.03B | BLOCKED | 0 | 5m 15s |
| 1.04B | BLOCKED | 0 | 5m 15s |
| 1.05B | BLOCKED | 0 | 5m 12–13s |
| 1.07B | BLOCKED | 0 | 5m 12s |
| 1.09B | BLOCKED | 0 | 5m 9s |

### Angular probes (θ>0°)

All at R≈1.05B.

| θ | b_center | Verdict | Spanning |
|---|----------|---------|----------|
| 2.7° | 50M | BLOCKED | 0 |
| 5.5° | 100M | BLOCKED | 0 |
| 11.0° | 200M | BLOCKED | 0 |
| 22.4° | 400M | BLOCKED | 0 |

**Note:** Mac angular probes used the pre-fix spanning logic. Off-axis verdicts were "BLOCKED, 0 spanning" but this was BEFORE the geometric threshold fix (bda7580). The degree stats were valid; the spanning verdicts were artifacts of the off-axis bug. The Jetson campaign (below) re-verified these angles with the corrected code.

## Verification Campaign (Jetson, k40-verify-campaign.py)

All probes: 4096 tiles, k²=40.

### Phase 1 — Sanity

| Probe | R | Verdict | Spanning | Wall time |
|-------|---|---------|----------|-----------|
| P1-01 | 600M | CONNECTED | 1 | 1346s |
| P1-02 | 800M | CONNECTED | 1 | 1353s |
| P1-03 | 1.05B | BLOCKED | 0 | 1334s |

All matched expectations: connected below moat, blocked at moat.

### Phase 2 — Lower boundary

| Probe | R | Verdict | Spanning | Wall time |
|-------|---|---------|----------|-----------|
| P2-01 | 1.00B | BLOCKED | 0 | 1336s |
| P2-02 | 0.98B | BLOCKED | 0 | 1335s |
| P2-03 | 0.96B | BLOCKED | 0 | 1336s |
| P2-04 | 0.94B | BLOCKED | 0 | 1345s |
| P2-05 | 0.92B | BLOCKED | 0 | 1343s |
| P2-06 | 0.90B | BLOCKED | 0 | 1337s |
| P2-07 | 0.85B | BLOCKED | 0 | 1346s |
| P2-08 | 0.80B | CONNECTED | 1 | 1357s |

**Transition: CONNECTED at R=800M, BLOCKED at R=850M.**

### Phase 3 — Off-axis isotropy (R=1.05B)

| Probe | θ | Verdict | Spanning | Wall time |
|-------|---|---------|----------|-----------|
| P3-01 | 5° | BLOCKED | 0 | 1339s |
| P3-02 | 11° | BLOCKED | 0 | 1335s |
| P3-03 | 22° | BLOCKED | 0 | 1336s |
| P3-04 | 33° | BLOCKED | 0 | 1332s |
| P3-05 | 40° | BLOCKED | 0 | 1333s |
| P3-06 | 45° | BLOCKED | 0 | 1322s |

All BLOCKED. First valid off-axis spanning verdicts (post-fix). The moat is isotropic at R=1.05B — no angular escape route.

### Phase 4 — Upper boundary + degree profiling

| Probe | R | Type | Verdict | Spanning | Wall time |
|-------|---|------|---------|----------|-----------|
| P4-01 | 1.12B | radial | BLOCKED | 0 | 1353s |
| P4-02 | 1.15B | radial | BLOCKED | 0 | 1348s |
| P4-03 | 1.20B | radial | BLOCKED | 0 | 1331s |
| P4-04 | 1.30B | radial | BLOCKED | 0 | 1349s |
| P4-05 | 0.95B | degree | BLOCKED | 0 | 2377s |
| P4-06 | 1.00B | degree | BLOCKED | 0 | 2363s |
| P4-07 | 1.05B | degree | BLOCKED | 0 | 2390s |

All BLOCKED through R=1.30B. No upper boundary found.

**Note:** Degree stats probes ran with `--degree-stats` flag but degree values were not captured in campaign output (parsing gap). Wall time for degree probes ~40 min vs ~22 min for standard probes, confirming degree computation ran.

## Degree Stats (measured, Mac session)

| k² | R (moat) | Backward offsets | Mean backward degree | Mean total degree | Isolated % |
|----|----------|-----------------|---------------------|-------------------|------------|
| 32 | 2.82M | 50 | 1.96 | 3.92 | 1.3% |
| 36 | 80M | 56 | 1.99 | 3.97 | 1.3% |
| 40 | 1.05B | 64 | 2.02 | 4.03 | 1.3% |

Mean total degree crosses 4.0 at k²=40. Isolated fraction is constant across k² values. The degree trend is consistent with the blocking behavior becoming more persistent at higher k².

## Failed/Aborted Runs (Mac, documented for completeness)

- **First 5-probe batch:** `ulimit -v` not supported on macOS (exit code 1)
- **Second batch:** `b_max` clamping bug — orchestrator computed `b_max` from annular geometry (~16.2M), producing 515K tiles instead of 4K. Killed.
- **Third batch:** timed out (same bug)
- **First angular batch:** 0 tiles — annular clipping rejected all tiles when `b_min > 0`. Fixed in 2ce3b7f.
- **First Jetson campaign (misconfigured):** `RADIAL_HALF_WIDTH=1000` (1 tile radially) — all on-axis CONNECTED (false), all off-axis BLOCKED (artifact). Results discarded. Fixed in fc583d3.

## Moat Boundary Map

```
R (x10^9)  Verdict
0.60       CONNECTED  (Jetson, spanning=1)
0.80       CONNECTED  (Jetson, spanning=1)
---------- TRANSITION ----------
0.85       BLOCKED    (Jetson)
0.90       BLOCKED    (Jetson)
0.92       BLOCKED    (Jetson)
0.94       BLOCKED    (Jetson)
0.95       BLOCKED    (Jetson, degree probe)
0.96       BLOCKED    (Jetson)
0.98       BLOCKED    (Jetson)
1.00       BLOCKED    (Jetson + Mac)
1.03       BLOCKED    (Mac)
1.04       BLOCKED    (Mac)
1.05       BLOCKED    (Mac + Jetson, 6 angles)
1.07       BLOCKED    (Mac)
1.09       BLOCKED    (Mac)
1.12       BLOCKED    (Jetson)
1.15       BLOCKED    (Jetson)
1.20       BLOCKED    (Jetson)
1.30       BLOCKED    (Jetson)
```

**Lower edge:** between R=800M and R=850M
**Upper edge:** not found (still blocked at R=1.3B)

## Commits

All in `gaussian-moat-cuda` repository, chronological:

| Hash | Description |
|------|-------------|
| 09e385a | (early fat-stripe work) |
| 163ba81 | (early fat-stripe work) |
| 5502cf4 | (early fat-stripe work) |
| 293c18f | (early fat-stripe work) |
| bff7041 | (early fat-stripe work) |
| 3e4e953 | (early fat-stripe work) |
| 30ca398 | (early fat-stripe work) |
| a53ee9f | (early fat-stripe work) |
| 313a293 | (early fat-stripe work) |
| 97a18d8 | (early fat-stripe work) |
| 2ce3b7f | Fix: annular clipping for off-axis tiles (b_min > 0) |
| 6c50e23 | (post-fix work) |
| de361a1 | (post-fix work) |
| bda7580 | Fix: geometric radii for off-axis spanning thresholds |
| 9e08a8e | (post-fix work) |
| 3be2704 | (post-fix work) |
| fc583d3 | Fix: Jetson campaign RADIAL_HALF_WIDTH configuration |

## Key Files

- `tile-probe/crates/fat-stripe/` — core fat-stripe crate
- `tile-probe/crates/fat-stripe/src/lib.rs` — tile-based union-find spanning detection
- `scripts/k40-verify-campaign.py` — Jetson verification campaign orchestrator
- `research/2026-03-25-k40-fat-stripe-discovery.md` — earlier discovery notes

## Open Questions

1. **Exact lower boundary:** binary search between R=800M and R=850M
2. **Does the upper boundary exist?** Or is this permanent disconnection beyond R~825M?
3. **Degree stats across the transition:** need to re-run with proper output capture (Jetson degree probes ran but output wasn't parsed)
4. **Percolation interpretation:** is this a localized moat (annular gap) or a phase boundary (permanent disconnection)?
5. **Comparison with theoretical predictions:** how does the 800M–850M transition compare to the expected moat radius for k²=40?
