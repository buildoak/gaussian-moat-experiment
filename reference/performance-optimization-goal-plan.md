# Performance Optimization Goal Plan

Branch: `opt/performance-wave-1`

Baseline:
- `main@7c227c2 Add project README`
- Verified implementation lineage: `bdb4b0a Fix dispatcher reuse test fixture size`
- Gate board: `reference/current-gate-board.md`
- Verification record: `tiles-maxxing/cuda-campaign-v2-sqrt-36/planning/2026-05-02-vast-4090-verification.md`

## Objective

Improve end-to-end CUDA campaign throughput without moving the mathematical
surface. The current RTX 4090 baseline is about `43.6k tiles/s` for full
Tsuchimura runs, with GPU TileOp generation around `69.6k tiles/s` and CPU
streaming composition around `120k tiles/s`.

The first goal loop should measure the full pipeline as a coupled CPU/GPU
system, then optimize the cheapest barriers before touching high-risk math
surfaces. GPU work, host transfers, and CPU composition should collaborate
instead of serially waiting whenever we can prove the ordering remains sound.

## Acceptance Gates

Every optimization commit that affects runtime behavior must pass:

1. CPU CTest locally.
2. CUDA CTest on a CUDA host.
3. `cuda_vs_cpu_diff --m4 --k5 --verbose` probes for both Tsuchimura cases.
4. Snapshot SHA smoke.
5. Full Tsuchimura gate:
   - `80000000 -> 80015782`: early `SPANNING`
   - `80000000 -> 80015782`: full `SPANNING`, zero overflows
   - `80000000 -> 80015790`: full `MOAT`, zero overflows
6. Before/after profile JSON using the same hardware class and command shape.
7. Overflow accounting:
   - normal output must still print all overflow counters,
   - profile JSON must include overflow counters,
   - full Tsuchimura gates must remain zero-overflow,
   - exploratory runs must report overflow rate as
     `overflowed_tiles / produced_tiles`; target is below `0.001%` unless a
     run is explicitly designed to stress overflow handling.

Do not accept a speedup that weakens any stronger correctness gate.

## Optimization Menu

| Rank | Item | Expected payoff | Risk | Complexity | Gate |
|---:|---|---:|---:|---:|---|
| 1 | Instrument timeline and throughput by stage | High leverage | Low | Low | Profile JSON sanity + CTest |
| 2 | Tune app batch / device slab / ring parameters | Low-Med | Low | Low | CUDA CTest + Tsuchimura + profile |
| 3 | Persist dispatcher/ring/workspace buffers across app batches | High | Low-Med | Med | CUDA CTest + snapshot + Tsuchimura |
| 4 | Replace per-slab host overflow readbacks with summary counters | High | Med | Med | overflow tests + Tsuchimura zero counters |
| 5 | CPU/GPU overlap: dispatch batch N+1 while compositor ingests batch N | Very High | Med-High | High | full gates + timeline proof |
| 6 | Inspect and optimize CPU streaming compositor | High | Med | Med-High | compositor parity + Tsuchimura |
| 7 | Inspect CUDA face extraction / face encoding kernels | Very High | Med-High | High | `--k5` byte parity + snapshot |
| 8 | Restore/optimize off-axis MR path | High | Med | Low-Med | `--m4 --k5` parity + Tsuchimura |
| 9 | Data-in/data-out constraints and transfer layout | Med | Med | Med | profile + parity + snapshot |
| 10 | Register cap / launch configuration sweeps | Med | Low | Low | same-hardware profile comparison |
| 11 | K1 sieve/cache/table experiments | Low-Med | Low-Med | Med | K1/K4/K5 parity |

Fixed constraints for this campaign loop:
- tile size and tile width are fixed,
- full-octant Tsuchimura semantics are fixed,
- `sqrt(40)` and beyond are not Wave 1 acceptance targets.

## Wave 0 Scope - Measurement Harness

Before changing runtime behavior:

1. Add or document a compact profile extractor that turns profile JSON into:
   - CUDA TileOps/s,
   - compositor tiles/s,
   - full pipeline tiles/s,
   - produced vs ingested tiles,
   - app batches, dispatcher chunks, slabs,
   - overflow counts and overflow rate.
2. Run baseline with at least `chunk-size=50000`, `200000`, and `500000`.
3. Record whether bottlenecks are GPU compute, CPU compositor, transfers,
   synchronization, or unnecessary allocation/readback.

This wave is accepted when the next edit target is justified by data, not by
intuition.

## Wave 1 Scope - Cheap Barriers

Primary targets:

1. Tune safe runtime parameters:
   - app batch size / `--chunk-size`,
   - device slab sizing,
   - ring slots,
   - diagnostics mode.
2. Persist dispatcher/ring/workspace buffers across app batches.
3. Replace always-on per-slab host overflow array readbacks with cheap device
   summary counters, keeping detailed diagnostics behind explicit profile/debug
   mode.

Why these first:
- They are mostly host-driver lifecycle changes.
- They should not alter TileOp math, face order, byte layout, or verdict logic.
- They produce cleaner profiles before deeper GPU kernel work.

Non-goals for Wave 1:
- MR primality algorithm changes.
- Face representative extraction rewrites.
- TileOp layout changes.
- `sqrt(40)` changes.

## Wave 2 Scope - CPU/GPU Collaboration

Target: make the GPU and CPU overlap safely.

Candidate shape:

1. GPU produces complete-column batches.
2. CPU compositor ingests batch N while GPU dispatches batch N+1.
3. Early exit remains checked only after complete-column ingestion.
4. Snapshot mode keeps full production and disables early exit.
5. MOAT still finalizes only after all columns are ingested.

This can move full-pipeline throughput toward the slower of CUDA TileOps/s and
compositor tiles/s instead of the harmonic sum of both. On the current baseline,
that means a theoretical ceiling closer to `~69k tiles/s` than `~44k tiles/s`,
before transfer and synchronization overhead.

Proof burden:
- no partial-column stitching bugs,
- no lifetime bugs in host TileOp buffers,
- no early-exit check before all required neighbors in the ingested prefix are
  available,
- timeline evidence that overlap actually occurs.

## Wave 3 Scope - CPU Compositor

Inspect why the CPU compositor takes `~127-128s` on full Tsuchimura runs despite
being simpler than CUDA TileOp generation.

Candidate areas:

1. DSU root lookup/compression hot paths.
2. Face stitch loops and ordinal matching.
3. Frontier compaction data structures.
4. Allocation churn during column ingestion.
5. Cache locality of live groups and reach flags.
6. Special fast paths for empty/low-port tiles.

Gate:
- full compositor and streaming compositor parity tests,
- Tsuchimura gate,
- snapshot SHA smoke if TileOp order or ingestion order is affected.

## Wave 4 Scope - CUDA Kernels

Only after Wave 0-3 isolate GPU-side bottlenecks:

1. Inspect face extraction and face encoding kernels first.
2. Restore no-trial-division off-axis MR path.
3. Re-sweep MR register caps and kernel launch configuration.
4. Consider compacted prime position decode changes.
5. Consider K1 sieve/cache/table experiments only after higher bottlenecks move.

Face encoding is high payoff but high correctness risk: every change must keep
face/port ordering, group labels, representative extraction, and byte parity.

## Wave 5 Scope - Larger Campaign Readiness

After K36 performance is verified:

1. Re-evaluate memory model at `R=1.1B` and larger.
2. Check overflow rate across larger-radius samples; keep target under `0.001%`.
3. Decide what changes are needed for `sqrt(40)` and beyond.
4. Do not start `sqrt(40)` campaign logic until K36 gates remain green.

## Measurement Protocol

Use these primary benchmark cases:

```bash
scripts/run_tsuchimura_gate.sh \
  --cuda-bin ./build-k36/campaign_main_cuda \
  --chunk-size 200000 \
  --timing \
  --profile-dir /workspace/profiles
```

Also run early-exit chunk comparison after behavior is correct:

```bash
for chunk in 25000 50000 100000 200000 500000; do
  ./build-k36/campaign_main_cuda \
    --k-sq=36 \
    --r-inner=80000000 \
    --r-outer=80015782 \
    --region full-octant \
    --chunk-size="$chunk" \
    --timing \
    --profile "/workspace/profiles/R80015782_early_chunk${chunk}.profile.json"
done
```

If device slab size or ring slots become tunable CLI/config parameters, sweep
them independently. Do not mix parameter sweeps with code changes in the same
performance comparison.

Report using `reference/performance-report-template.md`.

## Branch Discipline

- Keep each optimization mechanism in its own commit.
- Run local CPU CTest before handing the branch to GPU verification.
- Commit profile summaries, not raw bulky logs.
- Keep raw Vast artifacts under ignored `artifacts/` or `/workspace`.
- If a change touches math-sensitive code, explicitly cite the methodology
  section or test that preserves the invariant.
- Track overflow counters and overflow rate in every performance summary, even
  when all counters are zero.

## Stop Conditions

Stop and reassess if:

- Any Tsuchimura verdict changes.
- Any overflow counter becomes nonzero on full gates.
- Exploratory overflow rate reaches or exceeds `0.001%` without an explicit
  overflow-stress rationale.
- Snapshot SHA smoke fails.
- `cuda_vs_cpu_diff` finds a TileOp mismatch.
- A speedup is within run-to-run noise and increases complexity.
- Host-driver cleanup does not improve or clarify profile data.
