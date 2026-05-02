# CUDA Campaign Experiment Contract

This file defines how to run and report experiments for
`cuda-campaign-v2-sqrt-36`. It is an operational contract for the current CUDA
implementation surface, not a replacement for the mathematical methodology.

## Authority

Correctness authority flows downward:

1. `methodology/tile-operator-definition-v-claude.md`
2. Tsuchimura K36 two-case known-answer verdicts
3. CPU `cpp-campaign-v2` behavior where it implements the methodology
4. CUDA byte parity with the CPU TileOp and snapshot surfaces
5. Targeted CPU/CUDA fault-localization tools
6. Golden JSON files as regression tripwires
7. Performance profiles only after correctness gates pass

Goldens preserve known implementation behavior. They do not prove the behavior
is mathematically right. Current CUDA goldens are generated from CUDA debug
output on centered batches, so they are smoke/regression checks, not truth.

## Experiment Types

Use the smallest experiment that answers the question.

| Type | Purpose | Required gate |
| --- | --- | --- |
| Logic audit | Check that code matches methodology | File/line evidence plus stated assumption |
| Kernel correctness | Change CUDA TileOp production | Relevant parity tests plus golden or snapshot gate |
| Host orchestration | Change batching, streams, memory, CLI, or snapshot handoff | Snapshot SHA gate plus overflow diagnostics check |
| Streaming early exit | Return `SPANNING` before all TileOps finish | Column-complete compositor ingestion plus later snapshot/parity gate |
| Golden refresh | Accept intentional behavior change | Explain why old behavior was wrong or intentionally obsolete |
| Performance experiment | Measure speed or profile bottlenecks | Correctness gates first, then isolated benchmark |

## Validation Ladder

Run validation in layers. Stop at the first failed layer and investigate.
For campaign-level correctness, the order is external truth, implementation
equivalence, fault localization, then regression tripwires.

1. **Build/static layer**
   - Build the relevant K target (`K_SQ=36` by default; K40 needs its own build).
   - Static asserts lock `TileOp` layout, constants, and capacity assumptions.
   - FJ64 witness table SHA is verified before upload.

2. **External truth layer**
   - Tsuchimura K36 is the strongest executable sanity gate:
     - `R_inner=80,000,000`, `R_outer=80,015,782` => `SPANNING`
     - `R_inner=80,000,000`, `R_outer=80,015,790` => `MOAT`
   - Use `--region full-octant` for these checks. These radii are distinct from
     the default `R_outer = R_inner + 8192` shell-probe convention.

3. **Snapshot equivalence layer**
   - `scripts/run_snapshot_sha_gate.sh` compares CPU and CUDA snapshot bytes for
     the same inputs.
   - This catches wrong TileOps even when the final verdict coincidentally
     matches.
   - Use this when changes can affect host orchestration, compositor handoff,
     CLI semantics, region shape, or end-to-end verdict behavior.

4. **Fault-localization layer**
   - `cuda_vs_cpu_diff` without flags checks K1-K4 parent parity.
   - `cuda_vs_cpu_diff --m4` checks geo bits, dense remap, and group flags.
   - `cuda_vs_cpu_diff --k5` checks full K1-K5 TileOp byte parity.
   - Add `--verbose` when a mismatch needs the first divergent byte.

5. **CTest layer**
   - CTest covers stub roundtrip, K1 overflow, K3/K4 parent parity,
     dense-remap adversarial cases, geo i128 sweep, face-group parity,
     port-sort collisions, full TileOp parity, and the small CUDA-vs-CPU diff.

6. **Golden smoke layer**
   - `golden/validate_golden.sh <batch|all> [build-dir]` regenerates JSON with
     `cuda_golden_dump` and compares it to tracked golden files.
   - Golden JSON hashes four surfaces:
     - prime bitmap
     - face encoding
     - connectivity labels
     - TileOp bytes
   - Treat this layer as a cheap regression tripwire. Do not use it to override
     Tsuchimura, snapshot parity, CPU/CUDA fault localization, or methodology.

## Streaming Early Exit

Early exit is a verdict-mode optimization. It is sound only for `SPANNING`:
after a compositor root has both inner and outer reach bits, later tiles can add
connectivity but cannot remove the already witnessed connection.

`MOAT` cannot early-exit. It means no inner/outer connected component exists in
the whole region, so it requires all required region tiles to be ingested.

Minimum implementation contract:

1. GPU may process internal slabs/chunks in any efficient order, but the CPU
   compositor ingests only complete columns unless a new tile-level compositor
   proves missing-neighbor stitches are deferred correctly.
2. After each complete column ingestion, call `has_spanning()`. A true result may
   return `SPANNING` in verdict mode.
3. Full snapshot parity mode must continue producing and writing all TileOps.
   Early-exit verdict mode and full-snapshot mode are separate modes.
4. For non-`full-octant` regions, the verdict is only for that requested region.
   Full-annulus claims require the full-octant shape plus the methodology's
   reflection-closure assumptions.

## Golden Sets

The CUDA golden files under `golden/` are compact regression fixtures. They are
centered contiguous tile batches, not full proof runs. They currently freeze
CUDA debug-output hashes; use them to catch accidental drift, not to prove
mathematical or CPU-equivalence correctness.

Current tracked batches:

| Batch | K | Radius scale | Role |
| --- | --- | --- | --- |
| `k36-small-r10m` | 36 | R=10M | quick sanity |
| `k36-medium-r50m` | 36 | R=50M | typical medium batch |
| `k36-large-r85m` | 36 | R=85M | larger stress batch |
| `k36-edge-tsuchimura-r80015790` | 36 | R=80M edge | known-answer-adjacent batch |
| `k40-r100m` | 40 | R=100M | K40 drift guard |

Refresh goldens only when all of these are true:

1. The behavior change is intentional.
2. The stronger authority layer agrees with the new behavior.
3. The report names the previous and new commit, command, build directory, GPU,
   and reason for accepting the new hash.
4. The old mismatch is explained as either stale golden, corrected bug, or
   intentional semantics change.

Never refresh goldens just to make a failing test green.

## Performance Experiments

Performance claims require a correctness preflight.

Before timing:

1. Record commit, branch, machine/GPU, driver/CUDA version, build flags,
   `K_SQ`, radii, region, chunk size, and command.
2. Confirm the relevant correctness gate passed on the same commit.
3. Run CUDA profiling sequentially. Do not overlap performance-sensitive CUDA
   workloads on the same GPU.
4. Put durable performance notes under `profiling/YYYY-MM-DD-slug.md`.
5. Keep generated snapshots, profiles, build directories, and raw binaries out
   of git unless explicitly accepted as tracked artifacts.

Performance reports must separate:

- end-to-end wall time
- CUDA K1-K5 time printed by the program
- compositor time
- snapshot time
- profiler kernel-time breakdown when available

## Report Contract

Every experiment report should fit this shape:

```text
Question:
Commit/branch:
Machine/GPU:
Build:
Command(s):
Gate:
Result:
Evidence:
Decision:
Next:
```

For correctness work, `Result` means pass/fail plus the exact failing surface.
For performance work, `Result` means measured numbers plus whether the baseline
was improved, unchanged, or regressed.

## Default Gates By Change

| Change surface | Minimum gate |
| --- | --- |
| Constants, layout, or ABI | build + CTest + golden |
| K1 sieve | K1/K4 parity + golden |
| K2 MR primality/bitmap | parity + golden prime bitmap hash |
| K3 compact | parent parity + golden connectivity hash |
| K4 UF, geo, dense labels | `--m4` parity + golden connectivity hash |
| K5 face encode/sort/pack | `--k5` parity + golden face/TileOp hashes |
| Host dispatch, streams, memory slabs | snapshot SHA gate + overflow diagnostics |
| Streaming early exit | column-complete SPANNING witness + snapshot SHA gate |
| CLI, grid/region, snapshot handoff | Tsuchimura gate + snapshot SHA gate |
| Performance-only refactor | previous correctness gate + benchmark report |
