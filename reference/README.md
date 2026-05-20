# Gaussian Moat Reference Map

This directory is for compact operational references. Historical plans, campaign
ledgers, and long evidence reports live under `reference/archive/` so they can
be cited without competing with the current verification doctrine.

First-read order:

1. `../AGENTS.md` - repo authority, claim semantics, compact gate spine.
2. `current-verification-spine.md` - executable gate spine and proof-status vocabulary.
3. `../agents-directives/experiment-contract.md` - how to run and report
   campaign experiments.
4. `../verification/README.md` and `../verification/postflight/README.md` -
   independent verifier tools and post-flight runner usage.

## Live References

| File | Role |
|---|---|
| `current-verification-spine.md` | Active compact verification gates, demoted tools, and sample policy. |
| `attached-static-annulus-moats.md` | Attached lower-K36 and local K34 static-annulus moat evidence, with proof-status cautions. |
| `k34-current-campaign-plan-20260507.md` | Current-gate rerun plan for attaching K34 local static-annulus moat rows. |
| `k34-tsuchimura-campaign-comparison-20260507.md` | Computational comparison between Tsuchimura's K34 campaign and the local CUDA K34 effective-width campaign. |
| `k26-static-annulus-diagnostics.md` | K26 Tsuchimura-endpoint static-annulus diagnostics, hard negatives, and next lower-K target logic. |
| `k36-lower-boundary-campaign-20260507.md` | K36 W32768 lower-boundary campaign that found the 72.875M sentinel moat and hard negatives to 60M. |
| `k36-lowest-moat-refinement-20260508.md` | K36 W32768 refinement of the 72.875M sentinel into a 64-gap island near 72.740M, plus lower hard negatives to 60M. |
| `k37-k39-k36-lowest-moat-scout-20260508.md` | Diagnostic K37-K39 full-ingest scout at the refined K36 lowest moat island. |
| `k40-lower-scout-20260509.md` | Diagnostic K40 full-ingest scout showing the K36/K39 lower island does not survive at W32768. |
| `k40-current-gate-campaign-20260509.md` | Current-gate K40 campaign: W32768 bracket `978M SPAN -> 979.5M MOAT`, larger-width W49152 bracket `937.5M SPAN -> 940.625M MOAT`, and profile-free early-exit W65536/W131072/W262144 scout branch points. |
| `k40-w786432-840m-candidate-and-w720896-clean-run-20260518.md` | W786432 `840M` timeout branch and W720896 no-timeout follow-up, which eventually found a late clean span after ~19h51m. |
| `k26-k36-telemetry-calibration-20260508.md` | Lower-K telemetry calibration pilot for K40 moat-search triage. |
| `agentic-optimization-workflow.md` | Branch/report workflow for long optimization work. |
| `optimization-safety-checklist.md` | Do-not-break checklist for math/TileOp/port/verdict changes. |
| `performance-report-template.md` | Performance report shape. |

## Archive

| Directory | Contents | Authority |
|---|---|---|
| `archive/campaign-ledgers/` | May 2026 K34/K36/K38/K40 campaign notes and evidence indexes. | Provenance only. |
| `archive/implemented-plans/` | Verification/postflight plans and evidence reports that have been implemented or superseded. | Historical context only. |
| `archive/performance-and-handoffs/` | Old optimization plans, wave reports, and worker handoffs. | Provenance only. |
| `archive/pre-push-secret-check.md` | Historical pre-push credential scan runbook. | Use only when pushing/publishing is requested. |
| `archive/heavy-history-cleanup-plan.md` | Historical large-history cleanup plan. | Use only when history cleanup is explicitly authorized. |

Archived files may contain obsolete sample budgets, gate ladders, branch names,
and proof language. Use them as evidence pointers, not as current instructions.
