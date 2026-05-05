# Static-Annulus Evidence Index - 2026-05-04

This is the compact first-read index for the May 4 K34/K36/K38/K40 campaign
evidence. It points to raw run artifacts instead of duplicating profiles and
logs.

## Semantics

Current campaign verdicts are local static-annulus detector results:

| Term | Current output | Implemented | Meaning |
|---|---|---|---|
| `ANY-SPAN` | `SPANNING` | yes | Some component connects the geometric inner and outer bands inside the tested static annulus. |
| `ANY-SHELL-MOAT` | full-ingest `MOAT` | yes | No component spans the tested static annulus. |
| `SOURCE-SPAN` / `WIRED-SPAN` | none | no | Future mode: spanning by a component connected to a chosen source or wired frontier. |
| `ORIGIN-SPAN` | none | no | Future mode: exact origin-component reachability. |

Do not report current K38/K40 results as origin-component moat proofs. Static
`MOAT` is one-sided useful under correct boundary/BZ/region conventions; static
`SPANNING` may be an orphan spanning component.

## Evidence Roots

Raw evidence roots:

- `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-4090-results`
- `/Users/otonashi/thinking/pratchett-os/data/pratchett-os/research-assets/gaussian-moat-cuda/2026-05-04-k36-below-k38-calibration`

Raw evidence is immutable provenance. Do not rename, normalize, or copy run
payloads into the repo.

## Acceptance Rules

Accepted campaign rows here are full-octant static-annulus rows. Full-ingest
`MOAT` rows must use `--no-early-exit`. `SPANNING` rows may use early exit only
when the row is explicitly a span witness, and diagnostic endpoint rows should
prefer no-early runs plus reconstructed path evidence when available. Overflow
counters must be zero. BZ status is currently enforced by wrapper/log/validator
discipline, not inside `campaign_main_cuda`.

## Accepted Rows

| claim_id | mode | K | geometry | verdict / bracket | acceptance | residual risk | primary evidence |
|---|---|---:|---|---|---|---|---|
| `K34-R24289452-shell-spanning` | `ANY-SPAN` | 34 | `24,289,452..24,297,644`, W8192, full-octant | `SPANNING` | regression/manual audit; zero overflow; CPU/CUDA parity gates in K34 gauntlet | semantic caution only; not origin/source evidence | `2026-05-04-4090-results/remote/k34-gauntlet-20260504T010708Z/profiles/R24289452_shell_spanning.profile.json` |
| `K36-main-gate-80M` | `ANY-SPAN` / `ANY-SHELL-MOAT` | 36 | `R_inner=80,000,000`, Tsuchimura endpoints | `80,015,782 SPANNING`; `80,015,790 MOAT` | external known-answer gate; CPU/CUDA CTest and snapshot smoke in same bundle; zero overflow | strongest executable truth gate, but still implementation-level evidence | `2026-05-04-4090-results/remote/main-b2776f3-k36-gate-20260504T000533Z/logs/verdict-summary.txt` |
| `K36-below-K38-W32768` | `ANY-SPAN` / `ANY-SHELL-MOAT` | 36 | W32768, full-octant | `73,339,843..73,372,611 SPANNING`; `73,359,375..73,392,143 MOAT` | strict `validate_campaign_run.py --expected-k 36` for final SPANNING path row and MOAT row; zero overflow | validator checks bundle consistency, not independent math | `2026-05-04-k36-below-k38-calibration/remote/k36-below-k38-span-diag3-20260504T201620Z/run-index.tsv`; `.../k36-below-k38-refine3-20260504T192006Z/run-index.tsv` |
| `K38-W32768-endpoint` | `ANY-SPAN` / `ANY-SHELL-MOAT` | 38 | W32768, full-octant | `71,875,000..71,907,768 SPANNING`; `73,437,500..73,470,268 MOAT` | no-early diagnostic; BZ-clean; zero overflow; SPANNING endpoint has reconstructed path | local static-annulus evidence, not origin or exact-threshold proof | `2026-05-04-4090-results/remote/k38-bracket-diag-20260504T163900Z/run-index.tsv` |
| `K40-W32768-endpoint` | `ANY-SPAN` / `ANY-SHELL-MOAT` | 40 | W32768, full-octant | `978,000,000..978,032,768 SPANNING`; `980,000,000..980,032,768 MOAT` | BZ-clean; zero overflow; R980M MOAT full-ingest diagnostic without path reconstruction | local static-annulus evidence, not origin or exact-threshold proof | `2026-05-04-4090-results/remote/k40-radius-refine2-20260504T073415Z/run-index.tsv`; `.../k40-980m-diag2-20260504T140207Z/run-index.tsv` |
| `K36-same-annulus-control` | monotonicity sentinel | 36 | same annulus as K38 MOAT endpoint: `73,437,500..73,470,268` | `MOAT` | BZ-clean; zero overflow; full ingest | sentinel, not a threshold claim | `2026-05-04-4090-results/remote/k36-same-annulus-control-20260504T165239Z/run-index.tsv` |
| `K40-same-annulus-control` | monotonicity sentinel | 40 | same annulus as K38 SPANNING endpoint: `71,875,000..71,907,768` | `SPANNING` | BZ-clean; zero overflow; full ingest | sentinel, not a threshold claim | `2026-05-04-4090-results/remote/k40-same-annulus-control-20260504T170020Z/run-index.tsv` |

## Supporting Readiness Rows

| row | purpose | evidence |
|---|---|---|
| K38 preflight | K38 build/test/BZ/diff readiness | `2026-05-04-4090-results/remote/k38-preflight-20260504T033820Z/preflight-index.tsv` |
| K40 preflight | K40 build/test/BZ/diff readiness | `2026-05-04-4090-results/remote/k40-preflight-20260504T033150Z/preflight-index.tsv` |
| K40 golden fixture | current K40 R100M golden fixture, not a moat endpoint | `2026-05-04-4090-results/remote/k40-current-golden-20260504T033715Z/k40-r100m.json` |
| 4090 inventory | raw pull provenance and unpulled remote workspace list | `2026-05-04-4090-results/remote-inventory.txt` |

## Failed Or Superseded Rows

| artifact | status | reason |
|---|---|---|
| `k40-radius-refine-20260504T073258Z` | failed | empty index; runner had `R: unbound variable`; superseded by `k40-radius-refine2`. |
| `k40-980m-diag-20260504T124022Z` | failed | process was killed during path reconstruction; superseded by `k40-980m-diag2`. |
| `k40_wide_R1200000000_W32768_early` | aborted row | `rc=143`, `NO_VERDICT`; not evidence for a verdict. |
| `k40_width1b_R1000000000_W24576_early` | aborted row | `rc=143`, `NO_VERDICT`; BZ passed but no campaign verdict. |
| `k38-broad-bracket-20260504T105733Z` | superseded | broad context only; use `k38-bracket-diag` for endpoint evidence. |
| `k34-gauntlet-20260504T010013Z` | superseded | useful provenance, but `010708Z` adds CPU/CUDA CTest logs. |
| `profiles-k34-20260503-165257/R24289452_moat.profile.json` | naming hazard | filename says moat, profile verdict is `SPANNING`; avoid as first-read evidence row. |

## Non-Claims

- No row here is an origin-component moat proof.
- No row here proves exact K38 or K40 global thresholds.
- K34 remains regression and semantic caution evidence, not Tsuchimura external
  truth.
- K36/K37/K38/K39 edge-palette equivalence remains a future regression idea,
  not an accepted implemented gate.
- `validate_campaign_run.py` validates evidence-bundle consistency; it is not
  an independently derived mathematical verifier.

## Next Gates

1. Build an independent annulus verifier that recomputes grid/enumeration,
   TileOps or an equivalent connectivity surface, and final verdict through a
   separate code path.
2. Add an independent SPANNING path certificate checker for traced endpoint
   rows.
3. Design `SOURCE/WIRED-SPAN` before claiming Tsuchimura-style source-connected
   upper-bound reproduction for K34/K38/K40.
4. Keep BZ status in the acceptance layer, or move BZ enforcement into
   `campaign_main_cuda` before relying on ad hoc runner discipline.

## Supporting Repo References

- `k36-below-k38-calibration-2026-05-04.md`
- `k38-k40-correctness-audit-2026-05-04.md`
- `k38-k40-campaign-status-2026-05-04.md`
- `sqrt34-gate-feasibility.md`
- `k34-static-annulus-gauntlet-2026-05-04.md`
- `k34-deeper-static-annulus-2026-05-04.md`
- `k34-face-port-stitch-path-2026-05-04.md`
- `k34-k36-centered-annulus-sweep-2026-05-04.md`
