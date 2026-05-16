# K40 Below-850M Fine Probe Plan - 2026-05-16

Updated: 2026-05-17 after user requested finer probes starting around
`400M-500M` and a git push before any new remote compute.

## Question

Can wider static annuli find a `K_SQ=40` static-annulus detector moat with
`R_inner < 850,000,000`?

This plan interprets "most below 850M" as "moats below 850M". The immediate
target is not acceptance proof. It is a BZ-clean branch map that identifies the
first below-850M row where a quick early span stops appearing.

## Current Evidence

Authority and gates follow `AGENTS.md`,
`agents-directives/experiment-contract.md`, and
`reference/current-verification-spine.md`.

Known rows relevant to this plan:

```text
W=262144, R=845000000: SPANNING scout, elapsed=356s, bz_rc=0, overflow=0
W=262144, R=850000000: SPANNING long diagnostic, elapsed=3609s, bz_rc=0
W=262144, R=855000001: MOAT audit, TILE_SAMPLE_AUDIT_PASS
W=262144, R=870000000: MOAT audit, TILE_SAMPLE_AUDIT_PASS

W=524288, R=600M/650M/700M/750M: early SPANNING in <=8s
W=524288, R=760M/770M/780M/790M/795M/799M: early SPANNING in <=11s
```

Interpretation:

- `W=262144` has pressure around `845M-855M`, with a hardened detector moat at
  `855000001`, but this does not yet give a below-850M moat.
- `W=524288` remains trivially spanning through `799M`, but there is no fine
  low-band map starting from `400M-500M`.
- Scout `SPANNING` rows are diagnostic unless a SPANNING certificate is emitted
  and independently checked.
- Timeout/non-early-span scout rows are candidates only; `MOAT` needs
  full-ingest audit and sample postflight.

## Operational Precondition

Before launching remote compute, push the current documentation branch:

```text
branch=repair/k40-w262144-telemetry-audit
remote=origin
```

This keeps the campaign plan and already-completed audit documentation visible
before spending more Vast time.

## Phase A - Primary W524288 Adaptive Mesh

Run a bounded early-exit scout:

```text
K_SQ=40
W=524288
region=full-octant
R_outer=R_inner+524288
telemetry=none
profile=disabled
samples=disabled
span_cert=disabled
early_exit=enabled
external_bz=required before each row
chunk_size=200000
```

Nominal `R_inner` mesh:

```text
lower anchors:
400,000,000
425,000,000
450,000,000
475,000,000
500,000,000

low-band fill:
525,000,000
550,000,000
575,000,000

known anchors, usually not rerun unless continuity is desired:
600,000,000
650,000,000
700,000,000
750,000,000
760,000,000
770,000,000
780,000,000
790,000,000
795,000,000
799,000,000

new bridge probes:
625,000,000
675,000,000
725,000,000
775,000,000

upper pressure ladder:
805,000,000
810,000,000
815,000,000
820,000,000
825,000,000
830,000,000
835,000,000
840,000,000
845,000,000
848,000,000
849,000,000
849,500,000
```

Run order:

1. Lower anchors: `400M-500M`.
2. Low-band fill: `525M-575M`.
3. New bridge probes: `625M`, `675M`, `725M`, `775M`.
4. Upper pressure ladder: `805M-849.5M`.

Do not rerun the known anchors by default; use them as documented continuity
points. Rerun only if the new script needs calibration rows in the same run
bundle.

Timeout schedule:

```text
400M-775M: 900s per row
805M-830M: 1200s per row
835M-840M: 2400s per row
845M-849.5M: 5400s per row
```

BZ rule:

- run external BZ check before CUDA;
- if a nominal row is BZ-invalid, replace it with the smallest upward clean
  offset in `[1, 128]`;
- if no clean offset keeps `R_inner < 850000000`, skip the row and document it;
- never report an invalid row as evidence.

Stop rule:

- continue through clean early spans;
- stop Phase A on the first upper-band `late_timeout_candidate`,
  non-zero overflow, BZ failure without clean replacement, or non-zero
  unexpected run code;
- if a lower-band row below `775M` times out, stop immediately and promote that
  row as the most interesting unexpected candidate;
- preserve the exact row as a follow-up audit candidate.

Phase A success output:

```text
summary.tsv
run-matrix.tsv
per-row BZ logs
per-row stdout/stderr
driver.log
git commit/status
GPU metadata
local artifact mirror under pratchett-os/data/vast/
reference doc update with the final table
```

## Phase B - Wider Scout Only If Phase A Fully Spans

If `W=524288` is still clean early `SPANNING` through `849.5M`, then below-850M
moat hunting needs a more radical width. Do not audit yet; first scout one wider
family.

Preferred second family:

```text
W=786432
R_inner = 400M, 500M, 600M, 700M, 775M, 825M, 840M, 848M, 849.5M
timeouts = 1800s, then 5400s for rows >=840M
same BZ, artifact, and stop rules as Phase A
```

Escalation family only if `W=786432` also cleanly spans through `849.5M`:

```text
W=1048576
R_inner = 400M, 500M, 650M, 780M, 830M, 848M
timeouts = 3600s, then 7200s for rows >=830M
same BZ, artifact, and stop rules
```

Reason for this order: `W=524288` already has clean anchors through `799M`, so
it is the cheapest width for a low-band mesh. Wider families are useful only
after that mesh fails to produce pressure.

## Candidate Promotion

For the first below-850M row that does not quickly span:

1. Verify the row is BZ-clean.
2. Rerun the exact row as a no-early-exit audit:

```text
telemetry=audit
tile_sample_count=512
profile=enabled
samples=manifested
early_exit=disabled
timeout=54000s
postflight=tile_sample_check + postflight_orchestrate
```

3. Accept possible `MOAT` only as static-annulus detector/audit evidence with
   `TILE_SAMPLE_AUDIT_PASS`, not `MOAT_PROOF_PASS`.
4. If the row spans late, record the timing and tighten the next scout window
   above it.

## Gate

Scout row classifications:

```text
early_span_clean:
  bz_rc=0, run_rc=0, verdict=SPANNING, early_exit_taken=1,
  overflow_total=0, emitted_overflow=0

late_timeout_candidate:
  bz_rc=0, run_rc=124, no detector verdict; candidate only

reject:
  BZ invalid without clean replacement, overflow, unexpected run code, or
  malformed telemetry
```

Audit candidate gate:

```text
bz_rc=0
run_rc=0
full ingest for MOAT
early_exit_taken=0
overflow_total=0
emitted_overflow=0
tile_samples_written=512
tile_sample_check=PASS
postflight_status=TILE_SAMPLE_AUDIT_PASS
```

## Pre-Mortem

Failure modes and mitigations:

- The scout burns a full day on non-acceptance rows.
  - Use adaptive spacing, bounded row timeouts, and stop on the first candidate
    signal.
- A blind fine sweep wastes rows where the existing data already says the width
  is trivially spanning.
  - Reuse known anchors, fill missing low/bridge rows, and reserve dense spacing
    for the upper pressure ladder.
- A timeout gets misread as a moat.
  - Use `late_timeout_candidate` vocabulary until full-ingest audit passes.
- BZ-invalid rows pollute the map.
  - Pre-check BZ and shift/skip before CUDA.
- The branch is not pushed before compute, leaving the run plan only local.
  - Push `repair/k40-w262144-telemetry-audit` before launching the scout.
- Artifacts are lost after the Vast instance is stopped.
  - Mirror run directories locally before cleanup and document local paths.
- Wider widths produce very expensive full audits.
  - Do not audit Phase B rows until the scout identifies a single best
    candidate.

## Launch Decision

This is a plan only. No compute has been launched from this document yet.
