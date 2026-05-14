# K36 Lower Boundary Campaign - 2026-05-07

## Scope

This records the `K_SQ=36`, `W=32768` static-annulus lower-boundary campaign
run on 2026-05-07. It is detector evidence for shell connectivity:

- `SPANNING` = `ANY-SPAN` from `geo_I` to `geo_O` in the tested annulus.
- `MOAT` = `ANY-SHELL-MOAT` in the tested annulus.
- It is not an origin-component or global Tsuchimura-style proof.

Local generated bundle:

```text
results/k36-lower-boundary-24h-20260507/
```

Remote source:

```text
/workspace/runs/k36-lower-boundary-24h-20260507
```

Generator:

```text
commit=fc52edce6f48d312220f1f4082fc17dee332be56
GPU=NVIDIA GeForce RTX 4090, driver 535.230.02
K_SQ=36, S=256, C=6
```

## Gates Run

All 60 primary rows passed:

```text
bz_rc=0
run_rc=0
sample_check_rc=0
produced == ingested == active
overflow_total=0
emitted_overflow=0
sample_checked=512
```

The run emitted full profiles, BZ logs, CUDA logs, sample manifests, sample
JSONL, and independent sample-check logs for each primary row.

## Main Findings

The old lower K36 bracket was rechecked and tightened:

| Role | R_inner | R_outer | Verdict | Gap |
|---|---:|---:|---|---:|
| last span | 73,355,635 | 73,388,403 | `SPANNING` | |
| first moat | 73,355,651 | 73,388,419 | `MOAT` | 16 |

The campaign then found and refined a lower bracket:

| Role | R_inner | R_outer | Verdict | Gap |
|---|---:|---:|---|---:|
| last span | 73,311,840 | 73,344,608 | `SPANNING` | |
| first moat | 73,311,904 | 73,344,672 | `MOAT` | 64 |

This lowers the first `W=32768` static-annulus moat row found in this campaign
by `43,747` in `R_inner` relative to the earlier 16-gap first moat at
`73,355,651`.

The tail sentinel addendum continued down to `60M`. It found two more clean
sentinel `MOAT` rows:

| R_inner | R_outer | Verdict | Notes |
|---:|---:|---|---|
| 73,250,000 | 73,282,768 | `SPANNING` | hard-negative sentinel |
| 73,203,125 | 73,235,893 | `MOAT` | lower sentinel moat candidate |
| 73,150,000 | 73,182,768 | `SPANNING` | hard-negative sentinel |
| 72,875,000 | 72,907,768 | `MOAT` | lower sentinel moat candidate |
| 72,625,000 | 72,657,768 | `SPANNING` | hard-negative sentinel |

All tail sentinels from `72,375,000` down to `60,000,000` were clean
`SPANNING`.

## Certificate Caveat

The `80M`, `W=15782` control SPAN certificate passed:

```text
cert_points=10332
status=clean
```

Large `W=32768` SPAN certificate attempts at 73M failed on this host with
`run_rc=137` before a usable certificate was emitted:

```text
cert_k36_lower_span_73339843
cert_pin16_73355635
cert_lower_refine64_73311840
```

So the 73M `SPANNING` rows have clean detector/sample evidence but do not reach
`SPAN_PROOF_PASS` in this bundle. They should be treated as
`TILE_SAMPLE_AUDIT_PASS` until a certificate is produced on a host/configuration
that can complete trace emission.

## Next Gates

1. Reproduce and refine the two lower sentinel moat candidates:
   `73,203,125` and `72,875,000`.
2. Obtain a large-row SPAN certificate for the refined SPAN endpoints, likely
   on a higher-memory host or with a lower-memory certificate path.
3. Decide whether the next K36 campaign should prioritize dense refinement
   around the lower sentinel moats or a wider sentinel sweep below `60M`.
