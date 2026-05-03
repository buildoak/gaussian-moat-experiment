# AGENTS.md - gaussian-moat-cuda

Read this before touching Gaussian Moat code. This repo is back in a clean-start mode: preserve useful prior work, but do not let old execution become authority.

## Authority Chain

1. **User direction in the current session** - especially corrections about canon, archive, and scope.
2. **Strongest math canon:** `methodology/tile-operator-definition-v-claude.md`.
3. **Current derived implementation:** `tiles-maxxing/`, treated as a high-signal implementation hypothesis, about 95% trusted, not proof.
4. **Tracked prior art:** `legacy/`, educational and occasionally useful for engineering ideas.
5. **Local archive:** `_archive/`, untracked/local-only archaeology. Do not use unless explicitly asked or unless you are doing a named archaeology task.

If these disagree, follow the stronger layer and report the conflict. Code is evidence, not canon.

## Current Project Posture

The project is restarting from the mathematical methodology plus a heavily reviewed current implementation surface. The old CUDA v1 documents and campaigns were technically serious, but mathematically incomplete or unsound. They can inspire optimizations and implementation patterns; they cannot justify correctness claims.

`tiles-maxxing/` is the active code surface derived from the methodology. It may have operational drift and unlikely-but-possible logic drift. Before extending it, read the relevant methodology section and audit the code path against the definitions and lemmas.

## Directory Roles

| Path | Role |
|------|------|
| `agents-directives/` | Single home for operational instructions created for agents. Every file here must be referenced from this `AGENTS.md`. |
| `agents-directives/experiment-contract.md` | Operational contract for CUDA experiments, validation gates, golden usage, and performance reports. |
| `reference/` | Current operational reference docs for gates, optimization workflow, pre-push checks, and history cleanup. Every durable workflow doc here should be named from the job it gates. |
| `reference/current-gate-board.md` | Current baseline/verified commits, exact required gates, Tsuchimura commands, and baseline performance numbers. Read before optimization work. |
| `reference/sqrt34-gate-feasibility.md` | Feasibility note for the rejected K34 annulus gate and what would be required for a real K34 external truth gate. |
| `reference/agentic-optimization-workflow.md` | Long-running agent workflow for optimization branches: preflight, post-correctness, and post-performance reporting. |
| `reference/optimization-safety-checklist.md` | Do-not-break checklist for math/TileOp/port/verdict semantics during optimization. |
| `reference/performance-report-template.md` | Required report shape for before/after profile JSON and timing evidence. |
| `reference/pre-push-secret-check.md` | Pre-push credential scan commands for current tree and history. |
| `reference/heavy-history-cleanup-plan.md` | Minimal safe plan for removing historical blobs over GitHub's file-size limit, with backup and bundle steps. |
| `methodology/tile-operator-definition-v-claude.md` | Strongest TileOp/connectivity canon. Start here for math, semantics, and proof obligations. |
| `methodology/BACKLOG.md` | Math and spec backlog. Useful, not stronger than the main methodology file. |
| `methodology/supportive/` | Canonical staging area for timestamped audit, explainer, poster-source, and understanding-improvement artifacts. Name files `YYYY-MM-DD-slug.md`. |
| `tiles-maxxing/cpp-campaign-v2/` | Current C++ reference implementation surface. Derived from canon; review before trusting. |
| `tiles-maxxing/cuda-campaign-v2-sqrt-36/` | Current CUDA implementation surface. Derived from canon; review before trusting. |
| `tiles-maxxing/vast-ai/` | Remote GPU deployment helpers. Operational only. |
| `legacy/docs-cuda-v1/` | Tracked CUDA v1 engineering documents. Technically useful, mathematically unsound/incomplete. |
| `legacy/campaign-sqrt-40/` | Tracked CUDA v1-era implementation corresponding to `docs-cuda-v1`. Engineering compass only. |
| `_archive/` | Local-only archive dump. Must stay untracked. |

Do not recreate root-level `docs/`, `artifacts/`, `results/`, or old campaign folders as authority surfaces. If a new durable document is needed, first decide whether it belongs in `methodology/`, beside the code it audits, or in the Pratchett project document.

## Current Gate Board

Before any optimization branch, read `reference/current-gate-board.md`.
It records the current verified baseline, the exact CPU/CUDA/Tsuchimura gates,
and the RTX 4090 performance baseline. If it disagrees with a newer verified
run, update the gate board in the same commit as the new verification note.

## Ground Truth Gate - Tsuchimura k^2=36

Tsuchimura's published `k^2=36` boundary remains the known-answer gate:

| R_outer | Expected verdict | Semantics |
|---------|------------------|-----------|
| `80,015,782` | `SPANNING` | Full annulus anchored with `R_inner=80,000,000` |
| `80,015,790` | `MOAT` | Full annulus anchored with `R_inner=80,000,000` |

This gate is evidence that an implementation is on the right track. It does not make the implementation the source of truth. A passing known-answer gate plus methodology alignment is the minimum trust package.

## Rejected Candidate Gate - Tsuchimura k^2=34

Tsuchimura also reports `sqrt(34)` finite with farthest distance
`< 24,289,452` in METR 2004-13. This is an upper-bound result for the connected
component of the origin, not an exact annular SPANNING/MOAT boundary like the
K36 gate.

The naive full-octant shell probe
`K_SQ=34, R_inner=24,289,452, R_outer=24,297,644` was tested on the 4090 at
commit `9e69542` and returned `SPANNING` with zero overflow counters. Therefore
it is not a valid external truth gate. To make K34 into a strong gate, use an
algorithm that verifies Tsuchimura's origin-component upper bound directly, or
find and externally justify an exact annular boundary.

The executable K34 cross-K regression gate is
`tiles-maxxing/cuda-campaign-v2-sqrt-36/scripts/run_k34_regression_gate.sh`.
It runs K34 snapshot smoke, K34 CPU/CUDA diff, and the observed shell sentinel
(`SPANNING`, zero overflows). Treat it as implementation regression coverage,
not mathematical truth.

## Verification Stack

Use this order when judging campaign correctness:

1. **External truth:** Tsuchimura's two-case K36 known-answer gate above.
2. **Cross-K regression:** K34 regression gate above, when K34 support is in
   scope. This is not external truth; it catches K-dependent implementation
   drift and overflow pressure.
3. **Implementation equivalence:** CPU/CUDA snapshot parity for the same full-octant inputs, because verdict equality alone can hide wrong TileOps.
4. **Fault localization:** `cuda_vs_cpu_diff --m4 --verbose` and `cuda_vs_cpu_diff --k5 --verbose` to find the first divergent internal surface, tile, or byte.
5. **Regression tripwires:** CUDA golden JSON batches are cheap smoke checks. They are not mathematical proof, and because they are generated from CUDA debug output they must not outrank CPU parity, Tsuchimura, or the methodology.

If these layers disagree, stop and resolve the stronger layer first. Do not refresh goldens to bless a failing stronger gate.

## Optimization Workflow

For long-running optimization work, use:

1. `reference/agentic-optimization-workflow.md` for branch shape and reporting.
2. `reference/optimization-safety-checklist.md` before changing any CUDA/CPU hot path.
3. `reference/performance-report-template.md` for before/after evidence.

Optimization acceptance requires correctness first, performance second. A speedup
without Tsuchimura and CPU/CUDA parity evidence is not accepted.

### Chunk Size For Gates And Performance

CUDA `--chunk-size` changes both streaming pressure and throughput. Treat it as
part of the benchmark contract, not an incidental flag.

- Use the default Tsuchimura gate chunk size (`200000` unless the script changes)
  for conservative correctness gates and apples-to-apples script comparisons.
- For accepted performance baselines, measure the optimized full-pipeline mode
  separately. As of the 2026-05-03 zero-offset run on RTX 4090, that means
  `--overlap-compositor` plus a large chunk around `400000` to `500000`; the
  measured MOAT case was slightly faster at `400000`.
- Use small chunks such as `8192` for streaming stress/correctness tests. They
  create many more app batches and intentionally expose batching/early-exit
  issues, but they are not the performance baseline.
- When reporting throughput, always include `chunk-size`, `produced_tiles`,
  `ingested_tiles`, total wall time, CUDA K1-K5 time, compositor time, and
  whether early exit was enabled/taken.

## Default Shell-Probe Contract

The current default shell-probe convention is:

1. Agree on tile construction width before the job; the preferred width is `256 * 32 = 8192`.
2. Select `R_inner`.
3. Set `R_outer = R_inner + 8192`.
4. Compute the full octant spanning from the vertical Y axis right/down to the `y = x` diagonal.
5. Produce the verdict for that full-octant shell.

Keep the CLI explicit: do not add a `--vanilla` shortcut unless the user asks.
Use explicit `--r-inner`, `--r-outer`, and `--region full-octant` so custom
annular widths remain first-class experiment inputs.

Do not silently substitute a partial region, centered sample batch, wedge, or differently anchored run when the user asks for the default shell probe. If current code behavior differs from this contract, report the mismatch before trusting results.

## Streaming Early-Exit Semantics

Streaming early exit is allowed only for `SPANNING`. Once the compositor has a
component carrying both inner and outer reach bits, later tiles can add edges but
cannot disconnect that witness. `MOAT` remains a whole-region verdict and cannot
be returned until all required tiles for the region have been ingested.

The safe implementation shape is column-complete ingestion: GPU may process
batches internally, but the CPU compositor should ingest a column only after all
TileOps for that column are available, then check `has_spanning()`. Arbitrary
partial-tile ingestion needs a separate proof/implementation that missing
neighbor stitches are deferred correctly.

Snapshot parity runs must still compute and write all TileOps. Early-exit mode
is a verdict mode, not a full-snapshot mode.

## Implementation Rules

- Read `methodology/tile-operator-definition-v-claude.md` before changing grid, TileOp, port, stitching, boundary, or verdict logic.
- Treat `tiles-maxxing/` as reviewable derived code, not scripture.
- Preserve closed tile boundaries: a side `S` tile contains `S+1` lattice points per axis.
- Preserve snapped-grid assumptions unless the methodology is updated first.
- Preserve face/port ordering determinism; any optimization must keep byte-stable semantics where tests expect it.
- Performance changes require correctness gates first, then scoped benchmarks that name hardware, command, commit, and measurement scope.
- CUDA profiling is sequential. Do not run multiple performance-sensitive CUDA workloads on one GPU at the same time.

## Legacy Use

Use `legacy/` as prior art:

- Good uses: optimization ideas, CUDA implementation patterns, deployment lessons, historical bug clues.
- Bad uses: proof claims, geometry authority, moat verdict authority, source of direct porting without review.

When borrowing from `legacy/`, say what was borrowed and which methodology obligation it satisfies. If it only improves performance, prove that semantics did not move.

## Git Hygiene

- `_archive/` is local-only and ignored. Do not stage it.
- Do not add generated binaries, build directories, CUDA profiles, snapshots, `.bin`, `.gprf`, `target/`, or `census_output/`.
- Goldens under `tiles-maxxing/cpp-campaign-v2/goldens/*.bin` are intentionally tracked.
- Before staging, run `git status --short --untracked-files=all` and inspect untracked roots.
- Prefer exact-path staging. Avoid broad staging until `_archive/` and generated output are confirmed ignored.
- Before committing, run a staged large-file check. New large artifacts need an explicit reason.
- Before pushing or publishing rewritten history, run `reference/pre-push-secret-check.md`.
- GitHub currently rejects the local history because old commits contain large generated blobs. Do not attempt history surgery casually; follow `reference/heavy-history-cleanup-plan.md` only after explicit approval.

## Compute Workflow

The Mac Mini has no CUDA compiler. CUDA build/run work happens on remote CUDA hosts such as vast.ai or Jetson, depending on task size. Any command over a few seconds on remote compute should run in `tmux`, and performance runs must be isolated.

Do not destroy cloud instances, push branches, publish results, or mutate remote services unless explicitly asked.

## Backlog

Keep this list succinct and update it as work lands.

1. Audit `cpp-campaign-v2` and `cuda-campaign-v2-sqrt-36` campaign-running semantics against the Default Shell-Probe Contract above: tile width, `R_outer = R_inner + 8192`, full-octant region, and verdict meaning.
2. Re-establish correctness gates for the current `main`: Tsuchimura two-case known-answer checks, full-octant CPU/CUDA snapshot parity, targeted fault localization, CTest, and only then golden smoke validation on the appropriate CUDA host.
3. Reintroduce streaming early-exit campaign execution from the legacy CUDA campaign: GPU emits TileOps in batches, CPU compositor ingests complete columns as soon as available, and the run exits as soon as a `SPANNING` witness is latched.
4. After correctness and streaming semantics are stable, profile bottlenecks under the experiment contract and only then optimize MR, face encoding, memory layout, or host overlap.
