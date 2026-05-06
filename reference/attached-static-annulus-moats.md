# Attached Static-Annulus Moats

Updated: 2026-05-06.

This file records local static-annulus moat evidence that is useful for the
current campaign, but must not be confused with origin-component proofs.

Current claim semantics still apply:

- `SPANNING` means `ANY-SPAN` inside the tested static annulus.
- `MOAT` means `ANY-SHELL-MOAT` inside the tested static annulus.
- Neither status proves Tsuchimura-style origin-component reachability.

## K36 Lower Attachment

The lower K36 bracket is attached as a zero-overflow discovered moat candidate.
It has a 2026-05-05 evidence bundle with exact BZ, zero overflow, legacy
persisted tile samples, and a SPANNING coordinate cert for the positive
endpoint. The legacy samples predate the current strict post-flight sample
schema, so the row should be rerun before it is counted as current accepted
MOAT-hardening evidence.

Evidence root:

```text
/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-05-k36-lower-verification
```

| Role | K | R_inner | R_outer | Width | Verdict | Active/produced/ingested | Overflow |
|---|---:|---:|---:|---:|---|---:|---:|
| Positive endpoint | 36 | 73,339,843 | 73,372,611 | 32,768 | `SPANNING` | 29,093,487 / 29,093,487 / 29,093,487 | 0 |
| Lower moat | 36 | 73,359,375 | 73,392,143 | 32,768 | `MOAT` | 29,101,312 / 29,101,312 / 29,101,312 | 0 |

Tracked rerun manifests:

- `verification/manifests/k36-lower-span-73339843-w32768.json`
- `verification/manifests/k36-lower-moat-73359375-w32768.json`

Archive report:

- `reference/archive/implemented-plans/k36-lower-verification-evidence-2026-05-05.md`

## K34 Local Moat Attachments

K34 is attached only as local static-annulus evidence. It remains unsuitable as
an external Tsuchimura `sqrt(34)` truth gate because Tsuchimura's published
statement is about the origin-connected component, while this implementation
tests annular shell connectivity.

Evidence root:

```text
/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results/remote/k34-k36-centered-sweep-20260504T023422Z
```

The centered K34 diagnostic confirmations found these first local moat rows:

| Center | K | R_inner | R_outer | Width | Verdict | Active/produced/ingested | Overflow |
|---:|---:|---:|---:|---:|---|---:|---:|
| 30,000,000 | 34 | 29,991,808 | 30,008,192 | 16,384 | `MOAT` | 6,007,739 / 6,007,739 / 6,007,739 | 0 |
| 40,000,000 | 34 | 39,995,904 | 40,004,096 | 8,192 | `MOAT` | 4,083,366 / 4,083,366 / 4,083,366 | 0 |
| 50,000,000 | 34 | 49,997,952 | 50,002,048 | 4,096 | `MOAT` | 2,649,535 / 2,649,535 / 2,649,535 | 0 |
| 80,000,000 | 34 | 79,998,976 | 80,001,024 | 2,048 | `MOAT` | 2,275,976 / 2,275,976 / 2,275,976 | 0 |

Tracked rerun manifests:

- `verification/manifests/k34-centered-moat-c30000000-w16384.json`
- `verification/manifests/k34-centered-moat-c40000000-w8192.json`
- `verification/manifests/k34-centered-moat-c50000000-w4096.json`
- `verification/manifests/k34-centered-moat-c80000000-w2048.json`

These K34 rows should be rerun under the current post-flight shape before being
used as accepted MOAT-hardening evidence: audit telemetry, persisted
512-sample tile audit, exact BZ, zero overflow, and full ingest.

## Current Rerun Gate

For every attached `MOAT` row, current acceptance requires:

```text
campaign_main_cuda --no-early-exit --telemetry audit \
  --tile-sample-count 512 \
  --sample-manifest verification/manifests/<claim>.json \
  --tile-sample-out RUN/samples/<claim>.tiles.jsonl \
  --profile RUN/profiles/<claim>.profile.json

verification/postflight/postflight_orchestrate.py \
  --profiles RUN/profiles/<claim>.profile.json \
  --sample-manifest verification/manifests/<claim>.json \
  --samples RUN/samples/<claim>.tiles.jsonl \
  --out-dir RUN/postflight/<claim> \
  --row-class audit \
  --telemetry-level audit \
  --fail-on-reject
```

For attached `SPANNING` rows, add `--emit-span-cert` to the campaign command
and pass `--span-cert` to post-flight.

Archive reports:

- `reference/archive/campaign-ledgers/sqrt34-gate-feasibility.md`
- `reference/archive/campaign-ledgers/k34-k36-centered-annulus-sweep-2026-05-04.md`
