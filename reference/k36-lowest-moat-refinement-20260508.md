# K36 Lowest Moat Refinement - 2026-05-08

## Scope

This note records the follow-up `K_SQ=36`, `W=32768` static-annulus campaign
that refined the lower sentinel moat found in the 2026-05-07 K36 boundary run.

Local generated bundle:

```text
results/k36-lowest-moat-refine-12h-20260508/
```

Remote source:

```text
/workspace/runs/k36-lowest-moat-refine-12h-20260508
```

Claim semantics:

- `SPANNING` = `ANY-SPAN` from `geo_I` to `geo_O` in the tested annulus.
- `MOAT` = `ANY-SHELL-MOAT` in the tested annulus.
- This is not an origin-component or global Tsuchimura-style proof.

## Generator

```text
commit=fc52edce6f48d312220f1f4082fc17dee332be56
GPU=NVIDIA GeForce RTX 4090, driver 535.230.02
K_SQ=36, S=256, C=6
W=32768
region=full-octant
chunk_size=200000
early_exit=disabled
telemetry=audit
tile_sample_count=512
```

The row contract ran `bz_exact_check`, CUDA full ingest, and independent
`tile_sample_check` for each row.

## Gates Run

All 79 data rows passed:

```text
bz_rc=0
run_rc=0
sample_check_rc=0
produced == ingested == active
overflow_total=0
emitted_overflow=0
sample_checked=512
status=clean
```

Row inventory:

```text
SPANNING rows: 74
MOAT rows: 5
bad rows: 0
total measured row runtime: 6.624h
```

## Main Finding

The previous `R_inner=72,875,000` sentinel moat reproduced cleanly, then the
campaign found and refined a lower static-annulus moat island:

| Role | R_inner | R_outer | Verdict | Gap |
|---|---:|---:|---|---:|
| last span before island | 72,739,496 | 72,772,264 | `SPANNING` | |
| first moat | 72,739,560 | 72,772,328 | `MOAT` | 64 |
| last moat | 72,740,648 | 72,773,416 | `MOAT` | |
| first span after island | 72,740,712 | 72,773,480 | `SPANNING` | 64 |

So the currently refined lower `W=32768` K36 static-annulus moat island is
observed from `R_inner=72,739,560` through `R_inner=72,740,648`, with both
entry and exit edges bracketed to 64 units.

This lowers the first observed clean `W=32768` K36 moat row by:

```text
72,875,000 - 72,739,560 = 135,440
```

relative to the earlier `72,875,000` sentinel row.

## Lower Hard Negatives

The same bundle includes selected hard-negative probes from `R_inner=72,250,000`
down to `60,000,000`. All of those lower dense/tail probes were clean
`SPANNING`; no lower sampled `MOAT` was found.

The exact `60,000,000` row was added as a same-parameter tail probe because the
scripted grid naturally ended at `60,250,000`.

## Certificate Caveat

These rows satisfy the clean detector/profile/BZ/sample-audit gate, but the
large `W=32768` `SPANNING` rows do not yet have independently checkable span
certificates. Treat the bundle as `TILE_SAMPLE_AUDIT_PASS` detector evidence,
not `SPAN_PROOF_PASS`, until the certificate path is repaired for large rows.

## Next Gates

1. Produce large-row span certificates for the bracketing SPANNING endpoints:
   `R_inner=72,739,496` and `R_inner=72,740,712`.
2. Repeat the island refinement with a second host/configuration if we want
   independent operational replication.
3. Continue lower K36 hunting below `60M` only after deciding whether the next
   campaign should be sparse sentinel search or telemetry-guided local
   refinement.
