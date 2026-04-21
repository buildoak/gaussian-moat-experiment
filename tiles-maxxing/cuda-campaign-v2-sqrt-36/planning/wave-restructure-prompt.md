## Task: Restructure M2-M9 into Parallel Worker Waves

You have the canonical plan (attached as context). M1 is complete (commit 3aeef39).

### Your job
Rework M2-M9 into **waves of parallel Codex GPT 5.4 xhigh workers**. Each wave can have multiple workers running simultaneously. Workers within a wave must be independent (no dependencies on each other). The next wave starts only when the previous wave completes.

### Constraints
1. **Dependency integrity** — a worker cannot start until its dependencies are complete
2. **Verification gates** — each worker must have a clear pass/fail gate
3. **Atomic deliverables** — each worker produces a commit or clear artifact
4. **No merge conflicts** — workers in the same wave should touch different files/directories

### Output format
Write a new file `planning/2026-04-21-wave-execution-plan.md` with:

```markdown
---
date: 2026-04-21
type: wave-execution-plan
status: ready
---

# Wave Execution Plan — cuda-campaign-v2-sqrt-36

## Wave 1: [name]
**Depends on:** M1 (complete)
**Workers:**
- Worker 1A: [description]
  - Files: [what it touches]
  - Gate: [verification]
  - Prompt summary: [2-3 sentences]
- Worker 1B: [description]
  - ...

## Wave 2: [name]
**Depends on:** Wave 1
...
```

### Things to consider
1. M2 (K1+K2 lift) and M3 (K3+K4 lift) are both "lift from v1" tasks — can parts run in parallel?
2. K5 (M5-M7) is a rewrite, not a lift — can its skeleton start before M4 finishes?
3. The test infrastructure (byte-parity harnesses) could be a separate parallel track
4. Host pipeline (M8) needs all kernels but its scaffolding could start early

### References
- M1 scaffold: already committed, includes `cuda_vs_cpu_diff` CLI and stub passthrough
- v1 CUDA code: `../campaign-sqrt-36/tile_cuda_multi_kernel/src/` (K1-K5 kernels to lift)
- CPU reference: `../cpp-campaign-v2/` (ground truth for byte-parity)
- Canonical plan: context file (M2-M10 details)

### Deliverable
`planning/2026-04-21-wave-execution-plan.md` — ready for coordinator to dispatch waves.
