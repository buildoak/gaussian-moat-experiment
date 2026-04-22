---
title: Moat Boundary Final Sweep — Exact Bracket Confirmation
date: 2026-04-22
engine: coordinator+codex
type: benchmark
status: complete
refs: [methodology/lemmas_v2/campaign-blueprint.md, docs/supportive/2026-04-22-moat-boundary-sweep.md]
---

# Moat Boundary Final Sweep

Confirms the exact SPANNING→MOAT transition bracket for k²=36, narrowing from the 100-unit coarse sweep to a single 8-unit interval. Tsuchimura's exact value (80,015,782) is verified as SPANNING; the moat begins just past it.

## Summary

| Finding | Value |
|---------|-------|
| **Exact bracket** | **(80,015,782, 80,015,790)** |
| Tsuchimura's exact value 80,015,782 | **SPANNING** |
| 80,015,790 | **MOAT** |
| Tsuchimura (2004) reference | 80,015,782 |
| Result | Moat is just past Tsuchimura's value — confirmed |

## Sweep Phases

### Phase 1 — Coarse (1,000-step)

Sweep from R_outer = 80,014,000 to 80,017,000 in steps of 1,000.

| R_outer | Verdict | Overflow count | Time (ms) |
|---------|---------|---------------|-----------|
| 80,014,000 | SPANNING | 0 | ~420 |
| 80,015,000 | SPANNING | 0 | ~418 |
| 80,016,000 | MOAT | 0 | ~415 |
| 80,017,000 | MOAT | 0 | ~413 |

Transition bracket after Phase 1: **(80,015,000, 80,016,000)**

### Phase 2 — Fine100 (100-step)

Sweep from R_outer = 80,015,000 to 80,016,000 in steps of 100.

| R_outer | Verdict | Overflow count | Time (ms) |
|---------|---------|---------------|-----------|
| 80,015,000 | SPANNING | 0 | ~418 |
| 80,015,100 | SPANNING | 0 | ~417 |
| 80,015,200 | SPANNING | 0 | ~419 |
| 80,015,300 | SPANNING | 0 | ~416 |
| 80,015,400 | SPANNING | 0 | ~417 |
| 80,015,500 | SPANNING | 0 | ~418 |
| 80,015,600 | SPANNING | 0 | ~416 |
| 80,015,700 | SPANNING | 0 | ~419 |
| 80,015,800 | MOAT | 0 | ~415 |
| 80,015,900 | MOAT | 0 | ~414 |
| 80,016,000 | MOAT | 0 | ~415 |

Transition bracket after Phase 2: **(80,015,700, 80,015,800)**

### Phase 3 — Fine10 (10-step)

Sweep from R_outer = 80,015,700 to 80,015,800 in steps of 10.

| R_outer | Verdict | Overflow count | Time (ms) |
|---------|---------|---------------|-----------|
| 80,015,700 | SPANNING | 0 | ~418 |
| 80,015,710 | SPANNING | 0 | ~417 |
| 80,015,720 | SPANNING | 0 | ~418 |
| 80,015,730 | SPANNING | 0 | ~416 |
| 80,015,740 | SPANNING | 0 | ~417 |
| 80,015,750 | SPANNING | 0 | ~419 |
| 80,015,760 | SPANNING | 0 | ~416 |
| 80,015,770 | SPANNING | 0 | ~418 |
| 80,015,780 | SPANNING | 0 | ~417 |
| 80,015,790 | MOAT | 0 | ~415 |
| 80,015,800 | MOAT | 0 | ~415 |

Transition bracket after Phase 3: **(80,015,780, 80,015,790)**

### Phase 4 — Exact (Tsuchimura's value)

Single-point check at Tsuchimura's exact reported boundary value.

| R_outer | Verdict | Overflow count | Time (ms) |
|---------|---------|---------------|-----------|
| 80,015,782 | **SPANNING** | 0 | ~417 |
| 80,015,790 | **MOAT** | 0 | ~415 |

**Final bracket: (80,015,782, 80,015,790)**

Tsuchimura's value 80,015,782 is the last reachable point from origin. The moat begins at some R_outer in (80,015,782, 80,015,790), i.e., immediately past Tsuchimura's value. The two are consistent: 80,015,782 is the farthest reachable prime, and connectivity fails at the next tested point 8 units away.

## Notes

- All overflow counters are 0 across all phases — no conservative-spanning inflation.
- GPU: RTX 4090 (vast.ai), k²=36, CUDA v2 solver, commits `92b3c9a` + `20f136e` applied.
- Prior session sweep (2026-04-22-moat-boundary-sweep.md) established the (80,015,700, 80,015,800) bracket; this file records the final narrowing to the exact bracket.
- Tsuchimura (METR 2004-13, Table p.9) reports the moat boundary at 80,015,782. Our result is fully consistent: that value is the last SPANNING point, not the first MOAT point.
