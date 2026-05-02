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

The first goal loop should target host/dispatcher overhead and diagnostics
barriers before touching MR or face encoding semantics.

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

Do not accept a speedup that weakens any stronger correctness gate.

## Wave 1 Scope

Primary targets:

1. Persist dispatcher/ring/workspace buffers across app batches.
2. Replace always-on per-slab host overflow array readbacks with cheap device
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
- GPU/CPU compositor overlap.
- `sqrt(40)` changes.

## Wave 2 Candidates

Only after Wave 1 is verified:

1. Restore no-trial-division off-axis MR path.
2. Re-sweep MR register caps and kernel launch configuration.
3. Consider face encode representative extraction rewrite.
4. Consider slab pipelining or GPU/CPU overlap if profiles still show enough
   host-side idle time.

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
for chunk in 50000 200000 500000; do
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

Report using `reference/performance-report-template.md`.

## Branch Discipline

- Keep each optimization mechanism in its own commit.
- Run local CPU CTest before handing the branch to GPU verification.
- Commit profile summaries, not raw bulky logs.
- Keep raw Vast artifacts under ignored `artifacts/` or `/workspace`.
- If a change touches math-sensitive code, explicitly cite the methodology
  section or test that preserves the invariant.

## Stop Conditions

Stop and reassess if:

- Any Tsuchimura verdict changes.
- Any overflow counter becomes nonzero on full gates.
- Snapshot SHA smoke fails.
- `cuda_vs_cpu_diff` finds a TileOp mismatch.
- A speedup is within run-to-run noise and increases complexity.
- Host-driver cleanup does not improve or clarify profile data.
