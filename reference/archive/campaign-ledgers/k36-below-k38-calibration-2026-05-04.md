# K36 Below-K38 Width-32768 Calibration - 2026-05-04

## Summary

This note records a K36 local static-annulus calibration run on the 4090.
All runs used width `32768`, `--region full-octant`, `--no-early-exit`,
`--overflow-diagnostics`, per-run BZ logs, stdout logs, JSON profiles, and
`run-index.tsv` rows.

This is local static-annulus evidence only. A `SPANNING` verdict means the
streamed annulus has an inner-to-outer crossing. A `MOAT` verdict means no such
local annular crossing was found in that annulus. These verdicts do not prove or
disprove origin-component connectivity and should not be described as
Tsuchimura-style global moat proof.

Primary outcome:

- From `R_inner=30M` through `R_inner=70M`, all sampled K36 width-32768
  annuli were `SPANNING`.
- Additional bisection found a strict local bracket:
  - `R_inner=73,339,843`, `R_outer=73,372,611`: `SPANNING`
  - `R_inner=73,359,375`, `R_outer=73,392,143`: `MOAT`
- Bracket gap by `R_inner`: `19,532`.

## Evidence Roots

Remote 4090 workspace:

- `/workspace/gaussian-moat-cuda-k34-gauntlet-20260504T010013Z`

Local mirror:

- `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-k36-below-k38-calibration/remote/`

Mirrored run directories:

- `k36-below-k38-coarse-20260504T183027Z`
- `k36-below-k38-refine-20260504T185327Z`
- `k36-below-k38-refine2-20260504T191312Z`
- `k36-below-k38-refine3-20260504T192006Z`
- `k36-below-k38-span-diag-20260504T192702Z`
- `k36-below-k38-bisect1-20260504T194420Z`
- `k36-below-k38-span-diag2-20260504T195150Z`
- `k36-below-k38-bisect2-20260504T200930Z`
- `k36-below-k38-span-diag3-20260504T201620Z`

The local mirror contains 36 files for this calibration bundle.

## Result Table

| R_inner | R_outer | mode | verdict | overflows | produced/ingested | validator status |
| ---: | ---: | --- | --- | --- | ---: | --- |
| 30,000,000 | 30,032,768 | coarse-noearly | SPANNING | `0:0:0:0:0` | 11,904,775 / 11,904,775 | relaxed |
| 40,000,000 | 40,032,768 | coarse-noearly | SPANNING | `0:0:0:0:0` | 15,870,872 / 15,870,872 | relaxed |
| 50,000,000 | 50,032,768 | coarse-noearly | SPANNING | `0:0:0:0:0` | 19,837,006 / 19,837,006 | relaxed |
| 60,000,000 | 60,032,768 | coarse-noearly | SPANNING | `0:0:0:0:0` | 23,802,929 / 23,802,929 | relaxed |
| 70,000,000 | 70,032,768 | coarse-noearly | SPANNING | `0:0:0:0:0` | 27,769,110 / 27,769,110 | relaxed |
| 72,000,000 | 72,032,768 | refine-noearly | SPANNING | `0:0:0:0:0` | 28,562,132 / 28,562,132 | relaxed |
| 72,750,000 | 72,782,768 | refine-noearly | SPANNING | `0:0:0:0:0` | 28,859,664 / 28,859,664 | relaxed |
| 73,125,000 | 73,157,768 | refine-noearly | SPANNING | `0:0:0:0:0` | 29,008,412 / 29,008,412 | relaxed |
| 73,281,250 | 73,314,018 | refine2-noearly | SPANNING | `0:0:0:0:0` | 29,070,274 / 29,070,274 | relaxed |
| 73,281,250 | 73,314,018 | span-path-diag | SPANNING | `0:0:0:0:0` | 29,070,274 / 29,070,274 | strict |
| 73,320,312 | 73,353,080 | bisect1-noearly | SPANNING | `0:0:0:0:0` | 29,085,729 / 29,085,729 | relaxed |
| 73,320,312 | 73,353,080 | span-path-diag | SPANNING | `0:0:0:0:0` | 29,085,729 / 29,085,729 | strict |
| 73,339,843 | 73,372,611 | bisect2-noearly | SPANNING | `0:0:0:0:0` | 29,093,487 / 29,093,487 | relaxed |
| 73,339,843 | 73,372,611 | span-path-diag | SPANNING | `0:0:0:0:0` | 29,093,487 / 29,093,487 | strict |
| 73,359,375 | 73,392,143 | refine3-noearly | MOAT | `0:0:0:0:0` | 29,101,312 / 29,101,312 | strict |

`relaxed` means accepted by:

```bash
uv run tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/validate_campaign_run.py \
  --expected-k 36 --allow-spanning-without-path <run-dir>
```

`strict` means accepted by:

```bash
uv run tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/validate_campaign_run.py \
  --expected-k 36 <run-dir>
```

Strict SPANNING rows include `--trace-spanning-path` evidence. The MOAT row does
not require a spanning path and was full-ingest/no-early.

Strict validator acceptances:

- `k36-below-k38-span-diag-20260504T192702Z`
- `k36-below-k38-span-diag2-20260504T195150Z`
- `k36-below-k38-span-diag3-20260504T201620Z`
- `k36-below-k38-refine3-20260504T192006Z`

## Relation To K38

The audited K38 width-32768 local bracket from the prior campaign was:

- `71,875,000`: `SPANNING`
- `73,437,500`: `MOAT`

The K36 strict local bracket here lies below the K38 MOAT endpoint:

- K36 `73,339,843`: `SPANNING`
- K36 `73,359,375`: `MOAT`

This is compatible with same-annulus monotonicity. Same-annulus implications are:

- K36 `SPANNING` implies K38 `SPANNING` on the same annulus.
- K38 `MOAT` implies K36 `MOAT` on the same annulus.

The converse implications do not hold. Comparing different annuli is calibration
evidence, not a monotonicity proof.

## Audit Surface

Subagents were used as independent context and evidence carriers:

- evidence scout: checked existing K36/K38 evidence and found no prior
  validator-compatible K36 below-71.875M bracket evidence
- runner auditor: reviewed command/runner contract and the need for
  `--trace-spanning-path` on SPANNING endpoints
- validator auditor: checked `validate_campaign_run.py` usage and strict versus
  relaxed acceptance rules
- synthesis auditor: restated same-annulus monotonicity and interpretation
  limits
- final evidence auditor: read the local mirror and confirmed the relaxed/strict
  evidence distinction before the final traced endpoints were completed

Residual risk:

- `validate_campaign_run.py` validates the evidence bundle and consistency
  surface; it does not independently recompute grid generation or active-tile
  enumeration.
- The independent verifier stream for paper-grade claims remains future work.
- These annuli are local static-annulus probes and are not origin-component
  moat certificates.

## Next Gate

The next verification-suite work should be an independently derived annulus
verifier that consumes a run directory and recomputes the verdict through a
separate code path. The most useful first target is the final strict K36 bracket:

- SPANNING side:
  `k36-below-k38-span-diag3-20260504T201620Z`
- MOAT side:
  `k36-below-k38-refine3-20260504T192006Z`
