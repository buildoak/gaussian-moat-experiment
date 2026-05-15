# K40 Radical Wide Probe Plan - 2026-05-15

## Question

Can wider static annuli move the K40 detector moat frontier substantially below
the current `W=262144` bracket `860M SPANNING -> 870M MOAT`?

## Canon And Claim Semantics

Authority and gates follow `AGENTS.md`,
`agents-directives/experiment-contract.md`, and
`reference/current-verification-spine.md`.

Current campaign verdicts are static-annulus detector results:

- `SPANNING` means `ANY-SPAN` from `geo_I` to `geo_O` inside the tested
  annulus.
- `MOAT` means no component connects `geo_I` to `geo_O` inside the tested
  annulus.
- Scout rows are not acceptance rows. Timeout rows are branch points, not moat
  claims.

Accepted/profile rows require exact profile evidence, BZ-clean boundaries,
zero overflow, manifested tile samples, and postflight. Accepted `SPANNING`
also requires a span certificate. `MOAT_PROOF_PASS` remains reserved.

## Existing K40 Evidence

- `W=32768`: `978,000,000 SPANNING -> 979,500,000 MOAT`.
- `W=49152`: `937,500,000 SPANNING -> 940,625,000 MOAT`.
- `W=262144`: early scout spans through `780M`; long correction found
  `850M SPANNING`, `860M SPANNING`, and `870M MOAT`.
- `W=262144, R=870M`: rerun on commit
  `7ddbc3f25d7fe268399cf0a9f7ee7f55c378c950` with audit telemetry,
  `512` samples, full ingest, and postflight status `TILE_SAMPLE_AUDIT_PASS`.

## 2026-05-15 Scout Plan

Remote instance:

```text
vast_instance=36747212
ssh=ssh -p 27212 root@ssh8.vast.ai
repo=/workspace/gaussian-moat-cuda
branch=repair/k40-w262144-telemetry-audit
commit=7ddbc3f25d7fe268399cf0a9f7ee7f55c378c950
```

Run directory:

```text
/workspace/runs/k40-radical-wide-scout-20260515
```

Tmux session:

```text
k40_radical_wide_20260515
```

Mode:

```text
K_SQ=40
region=full-octant
chunk_size=200000
telemetry=none
profile=disabled
samples=disabled
span_cert=disabled
external_bz=required
early_exit=enabled
```

Families:

```text
W=262144 pressure sweep:
  R_inner = 800M, 820M, 835M, 845M, 855M, 865M
  row timeouts = 4h for 800M-845M, 8h for 855M-865M
  stop family on first non-early-span row

W=524288 lower radical scout:
  R_inner = 600M, 650M, 700M, 750M
  row timeout = 2h
  stop family on first non-early-span row
```

## Gate

For each scout row:

- `bz_rc=0`;
- `run_rc=0` and `VERDICT=SPANNING` with early exit taken -> `early_span_clean`;
- `run_rc=124` -> `late_timeout_candidate`, not a moat claim;
- zero overflow counters and zero emitted-overflow bits for clean spans.

Success for this scout is a compact branch map:

- where `W=262144` stops finding quick spans below the audited `870M` moat;
- whether `W=524288` produces any lower timeout/candidate signal around
  `600M-750M`.

## Pre-Mortem

Plan: launch a bounded tmux scout on the existing Vast 4090 and document the
remote run artifacts.

Top risks and inoculations:

- Spending too much cloud time on non-acceptance rows.
  - Cap row timeouts and stop each family on the first non-early-span signal.
- Accidentally treating timeout as a moat.
  - Summary status uses `late_timeout_candidate`; this document states timeouts
    are branch points only.
- Losing provenance.
  - Write `GENERATOR.txt`, `run-matrix.tsv`, `summary.tsv`, `driver.log`,
    per-row stdout/stderr, BZ logs, git commit, and GPU metadata in the run
    directory before/while running.

Decision: proceed with the reduced scout. Full audit/postflight rows are a
separate follow-up after the scout identifies a branch point.

## Launch Log

Launched at `2026-05-15T08:50:28Z` on Vast instance `36747212`.

Status check command:

```bash
ssh -p 27212 root@ssh8.vast.ai \
  'RUN=/workspace/runs/k40-radical-wide-scout-20260515;
   tmux ls;
   nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu --format=csv,noheader,nounits;
   cat "$RUN/summary.tsv";
   tail -40 "$RUN/driver.log"'
```

First checkpoint:

```text
W=262144
R_inner=800,000,000
R_outer=800,262,144
bz_rc=0
run_rc=0
verdict=SPANNING
early_exit_taken=1
active=2,516,812,662
produced=199,876
ingested=165,026
overflow_total=0
emitted_overflow=0
elapsed_s=7
status=early_span_clean
```

Additional early checkpoints:

```text
W=262144, R_inner=820,000,000:
  bz_rc=0, run_rc=0, verdict=SPANNING, early_exit_taken=1
  active=2,579,721,994
  produced=399,751
  ingested=305,451
  overflow_total=0
  emitted_overflow=0
  elapsed_s=8
  status=early_span_clean

W=262144, R_inner=835,000,000:
  bz_rc=0, run_rc=0, verdict=SPANNING, early_exit_taken=1
  active=2,626,905,168
  produced=1,599,000
  ingested=1,418,600
  overflow_total=0
  emitted_overflow=0
  elapsed_s=23
  status=early_span_clean

W=262144, R_inner=845,000,000:
  bz_rc=0, run_rc=0, verdict=SPANNING, early_exit_taken=1
  active=2,658,359,419
  produced=29,182,242
  ingested=29,006,962
  overflow_total=0
  emitted_overflow=0
  elapsed_s=356
  status=early_span_clean
```

At `2026-05-15T09:24:36Z`, the script was running
`W=262144, R_inner=855,000,000` under tmux with GPU work active.
This row has an `8h` timeout and is the first row in the batch showing
substantial late-span pressure.
