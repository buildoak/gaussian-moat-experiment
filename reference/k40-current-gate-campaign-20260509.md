# K40 Current-Gate Campaign - 2026-05-09

## Scope

This records the current-gate `K_SQ=40`, `W=32768` campaign that followed the
K36/K39 lower-island transfer hypothesis and the archived K40 high bracket.

Generated bundles:

```text
results/k40-lower-scout-20260509/
results/k40-tier2-sparse-scout-20260509/
results/k40-979m-midpoint-20260509/
results/k40-980m-current-gate-20260509/
results/k40-high-zone-semisparse-20260509/
results/k40-widew-sentinels-20260510/
results/k40-widew-early-scout-20260510/
results/k40-widew-simple-scout-20260510/
results/k40-w49152-full-audit-20260510/
results/k40-w49152-refine-20260510/
results/k40-widew-early-exit-scout-20260510/
results/k40-widew-x8-early-exit-scout-20260510/
```

Remote sources:

```text
/workspace/runs/k40-lower-scout-20260509
/workspace/runs/k40-tier2-sparse-scout-20260509
/workspace/runs/k40-979m-midpoint-20260509
/workspace/runs/k40-980m-current-gate-20260509
/workspace/runs/k40-high-zone-semisparse-20260509
/workspace/runs/k40-widew-sentinels-20260510
/workspace/runs/k40-widew-early-scout-20260510
/workspace/runs/k40-widew-simple-scout-20260510
/workspace/runs/k40-w49152-full-audit-20260510
/workspace/runs/k40-w49152-refine-20260510
/workspace/runs/k40-widew-early-exit-scout-20260510
/workspace/runs/k40-widew-x8-early-exit-scout-20260510
```

Claim semantics:

- `SPANNING` = `ANY-SPAN` from `geo_I` to `geo_O` in the tested static annulus.
- `MOAT` = `ANY-SHELL-MOAT` in the tested static annulus.
- These are diagnostic static-annulus rows, not origin-component or global
  Tsuchimura-style proofs.

## Generator

```text
initial_campaign_commit=fc52edce6f48d312220f1f4082fc17dee332be56
high_zone_commit=0dd7e3219f3965f7e18de1fb3c08ba4bae5a4242
GPU=NVIDIA GeForce RTX 4090, driver 535.230.02
K_SQ=40, S=256, C=6
primary_width=32768
larger_width_follow_up=49152
region=full-octant
chunk_size=200000
early_exit=disabled
telemetry=audit
tile_sample_count=512
```

Runtime CUDA BZ exactness is not an accepted proof surface for non-square K.
Rows used `--allow-uncertified-boundary-band`, and each row separately ran
external `bz_check.py --k-sq 40 --r-inner <R> --r-outer <R+W>`.

## W32768 Gates Run

All 33 current-gate rows passed:

```text
external bz_check.py rc=0
run_rc=0
sample_check_rc=0
produced == ingested == active
overflow_total=0
emitted_overflow=0
sample_checked=512
status=diagnostic_full_clean
```

Row inventory:

```text
SPANNING rows: 31
MOAT rows: 2
bad rows: 0
```

## Main Findings

K40 erased the refined K36/K37-K39 lower static-annulus moat island at
`W=32768`. The K36 moat rows at `72,739,560`, `72,739,688`, and `72,740,648`
all became clean `SPANNING` rows under K40. The older K36/K38 moat sentinels at
`72,875,000`, `73,359,375`, and `73,437,500` also became `SPANNING`.

The sparse upward sentinels:

```text
90M, 110M, 140M, 180M, 230M, 300M, 390M, 500M, 650M, 780M
```

were all clean `SPANNING`.

The initial high bracket had current-gate evidence:

```text
979,000,000 SPANNING -> 980,000,000 MOAT
```

The follow-up high-zone semi-sparse ladder then tested:

```text
850M, 900M, 930M, 950M, 960M, 970M, 975M, 978M, 979.5M
```

Rows through `978,000,000` were clean `SPANNING`; `979,500,000` was the first
clean `MOAT`. All high-zone rows are full-ingest, zero-overflow,
external-BZ-clean, and passed manifested 512-tile independent sample checks.

The current W32768 high bracket is therefore:

```text
978,000,000 SPANNING -> 979,500,000 MOAT
```

## Larger Width Follow-Up

The requested `32k x2` / `32k x4` probes were attempted first:

```text
W=65,536   full-audit first row at 850M rejected with run_rc=137
W=65,536   early/profile scout variants rejected with run_rc=137
W=131,072  early scout rows rejected with run_rc=137
```

Those rows are capacity evidence only. They produced no accepted K40
mathematical verdicts on the 31 GiB RAM instance.

The viable larger-width fallback was `W=49,152`. It used the same accepted
diagnostic row gate: full-octant, early exit disabled, external BZ rc 0,
run rc 0, full ingest, zero overflow, and 512 manifested sample checks.

Initial W49152 sentinels:

```text
850,000,000  SPANNING
900,000,000  SPANNING
950,000,000  MOAT
```

Four bisection rows then refined the larger-width bracket:

```text
925,000,000  SPANNING
937,500,000  SPANNING
943,750,000  MOAT
940,625,000  MOAT
```

The current W49152 larger-width bracket is:

```text
937,500,000 SPANNING -> 940,625,000 MOAT
```

This is `38,875,000` units lower than the first clean W32768 moat at
`979,500,000`.

## Profile-Free Early-Exit Wide-W Scout

The initial W65536/W131072 attempts failed because profile finalization
materialized enormous active-tile data while computing the profile grid hash.
The follow-up scout therefore disabled profile output, samples, traces, and
certs, and kept only early-exit timing plus external BZ logs. These rows are
scout evidence only:

```text
early_span_clean       = clean early ANY-SPAN witness before timeout
late_timeout_candidate = no early span found before timeout; not a MOAT claim
```

Primary scout:

```text
remote_run=/workspace/runs/k40-widew-early-exit-scout-20260510
commit=0dd7e3219f3965f7e18de1fb3c08ba4bae5a4242
row_timeout_s=900
rows=22
count_early_span_clean=19
count_late_timeout_candidate=3
session_rc=0
```

Branch points:

```text
W=49,152   925,000,000 SPANNING -> 937,500,000 late_timeout_candidate
W=65,536   875,000,000 SPANNING -> 900,000,000 late_timeout_candidate
W=131,072  850,000,000 SPANNING -> 900,000,000 late_timeout_candidate
```

The W49152 timeout at `937,500,000` is intentionally interpreted as
calibration: that same row is an accepted full-audit `SPANNING` row in
`k40-w49152-refine-20260510/`, so the early-exit predicate is conservative near
the accepted W49152 transition.

The `32k x8` addendum:

```text
remote_run=/workspace/runs/k40-widew-x8-early-exit-scout-20260510
commit=0dd7e3219f3965f7e18de1fb3c08ba4bae5a4242
row_timeout_s=600
rows=10
count_early_span_clean=9
count_late_timeout_candidate=1
session_rc=0
```

Branch point:

```text
W=262,144  780,000,000 SPANNING -> 850,000,000 late_timeout_candidate
```

All scout rows had external `bz_check.py` rc 0. All early-span rows had zero
CUDA overflow counters and zero emitted-overflow TileOps. Timeout rows record
`run_rc=124` and no detector verdict.

## W262144 Long-Span Correction

The `W=262,144` timeout scout was too aggressive near the boundary. The first
long row at `850,000,000` showed why:

```text
850,000,000  SPANNING  elapsed=3,609s
```

The original 2700s pivot then timed out at `860,000,000`, but that timeout was
not trustworthy. A corrected 10h-per-row ladder reran `860,000,000` and found a
very late span:

```text
860,000,000  SPANNING  elapsed=26,174s
produced=2,411,307,566
ingested=2,411,168,137
active=2,705,542,477
overflow_total=0
emitted_overflow=0
external_bz_rc=0
```

The next corrected long row reached a full-ingest detector moat:

```text
870,000,000  MOAT  elapsed=29,765s
produced=2,736,997,077
ingested=2,736,997,077
active=2,736,997,077
overflow_total=0
emitted_overflow=0
external_bz_rc=0
```

The initial profile-free W262144 diagnostic bracket was therefore:

```text
860,000,000 SPANNING -> 870,000,000 MOAT
```

Two later audit runs hardened the `MOAT` side of this surface.

First, `W=262144, R_inner=870000000` was rerun with audit telemetry and
manifested tile samples:

```text
remote_run=/workspace/runs/k40-w262144-870m-audit-20260514-r2
local_artifacts=/Users/otonashi/thinking/pratchett-os/data/vast/instance-36747212-k40-w262144-870m-audit-20260514-r2/
commit=7ddbc3f25d7fe268399cf0a9f7ee7f55c378c950
R_outer=870262144
verdict=MOAT
run_rc=0
active=2,736,997,077
produced=2,736,997,077
ingested=2,736,997,077
early_exit_taken=0
total_s=33,062.774
cuda_k1_k5_s=25,189.212
compositor_s=6,216.621
overflow_total=0
emitted_overflow=0
tile_sample_check=PASS checked=512
postflight_status=TILE_SAMPLE_AUDIT_PASS
```

Second, a BZ-clean replacement for the invalid `855000000` pressure scout was
run as a full audit row:

```text
remote_run=/workspace/runs/k40-w524288-fine-and-w262144-855000001-audit-20260516
local_artifacts=/Users/otonashi/thinking/pratchett-os/data/vast/instance-36747212-k40-w524288-fine-and-w262144-855000001-audit-20260516/
commit=7ddbc3f25d7fe268399cf0a9f7ee7f55c378c950
R_inner=855000001
R_outer=855262145
verdict=MOAT
bz_rc=0
run_rc=0
active=2,689,814,835
produced=2,689,814,835
ingested=2,689,814,835
early_exit_taken=0
total_s=32,931.8
cuda_s=25,007.0
compositor_s=6,297.73
overflow_total=0
emitted_overflow=0
tile_samples_written=512
tile_sample_check=PASS
postflight_status=TILE_SAMPLE_AUDIT_PASS
```

The current W262144 detector/audit surface is:

```text
845,000,000 SPANNING scout -> 855,000,001 MOAT audit
```

The `MOAT` side is BZ-clean, full-ingest, sample-audited static-annulus
detector evidence. The `SPANNING` side remains scout evidence unless rerun with
an accepted SPANNING certificate. This is not `MOAT_PROOF_PASS`.

## W524288 Fine Scout Addendum

The `2026-05-15` radical scout tested `W=524288` at `600M`, `650M`, `700M`,
and `750M`; all rows were BZ-clean early `SPANNING` rows in `<=8s`.

The `2026-05-16` fine scout then tested:

```text
760,000,000 SPANNING elapsed=9s  bz_rc=0 overflow=0
770,000,000 SPANNING elapsed=8s  bz_rc=0 overflow=0
780,000,000 SPANNING elapsed=9s  bz_rc=0 overflow=0
790,000,000 SPANNING elapsed=11s bz_rc=0 overflow=0
795,000,000 SPANNING elapsed=8s  bz_rc=0 overflow=0
799,000,000 SPANNING elapsed=11s bz_rc=0 overflow=0
```

This width remains trivially spanning through `799M`. The below-850M moat hunt
should therefore build an adaptive low-band map starting at `400M-500M`, then
probe the `805M-849.5M` pressure ladder before spending full audit time.

## Interpretation

This is now the best current static-annulus K40 bracket at `W=32768`, with a
lower accepted larger-width bracket at `W=49152`.

The campaign does not prove there are no lower K40 static-annulus moat islands:
the lower search below 780M is sparse outside the dense K36-transfer region.
What it does establish is:

- the known K36/K39 lower island does not transfer to K40;
- all tested K40 sentinels from 60M through 780M span;
- high-zone sentinels at 850M through 978M span;
- the archived high bracket survives current sample-audited rerun and is refined
  to a `1,500,000`-unit gap in `R_inner`.
- W49152 produces an accepted lower moat bracket:
  `937,500,000 SPANNING -> 940,625,000 MOAT`.
- profile-free early-exit wider probes can now run without the old
  profile-finalization OOM, with scout branch points at W65536/W131072/W262144
  between `780M` and `900M`.
- corrected W262144 long rows found a diagnostic bracket:
  `860,000,000 SPANNING -> 870,000,000 MOAT`.
- W262144 `870M` and `855000001` were later hardened as full-ingest,
  sample-audited `MOAT` detector rows with `TILE_SAMPLE_AUDIT_PASS`.
- W524288 remains BZ-clean early `SPANNING` through `799M`; the next scout is
  an adaptive below-850M mesh starting at `400M-500M`.

## Next Gates

1. If the goal is W32768 bracket precision, bisect/refine between `978M` and
   `979.5M`.
2. If the goal is W49152 bracket precision, continue bisection between
   `937.5M` and `940.625M`.
3. If the goal is below-850M wider-annulus hunting, run the bounded W524288
   adaptive early-exit scout documented in
   `reference/k40-below-850-fine-probe-plan-20260516.md`.
4. Confirm any new below-850M timeout/non-early-span candidate with a BZ-clean
   full-ingest audit row before calling it a detector moat.
5. Promote SPANNING certificate support for large K40 rows before presenting
   these as more than detector/sample evidence.
