# Agentic Optimization Workflow

This repo is suitable for long-running agent work only when each branch keeps
one explicit baseline, one narrow optimization objective, and one acceptance
gate stack.

## Branch Shape

1. Start from a clean main or a named cleanup branch.
2. Name optimization branches by target, for example
   `opt/dispatcher-workspace-persistence`.
3. Keep changes grouped by one performance mechanism. Do not mix math
   semantics, deployment docs, and kernel rewrites in one branch.
4. Keep raw profiles, build trees, snapshots, and Vast pulls under ignored
   `artifacts/`, `profiles/`, `runs/`, or `/workspace` paths.

## Preflight

Before edits:

```bash
git status --short --branch
git log --oneline -5
```

Read:

- `AGENTS.md`
- `reference/current-verification-spine.md`
- `reference/optimization-safety-checklist.md`
- `agents-directives/experiment-contract.md`
- `methodology/tile-operator-definition-v-claude.md` if touching grid, TileOp,
  face/port, stitching, boundary, or verdict logic.

## Post-Correctness

Run the required gates from `reference/current-verification-spine.md`. For CUDA changes,
local C++ tests alone are not enough.

## Post-Performance

Every performance report must include:

- commit hash,
- hardware,
- command,
- profile JSON path,
- baseline profile path,
- total seconds,
- CUDA K1-K5 seconds,
- compositor seconds,
- produced and ingested tile counts,
- overflow counters,
- verdict,
- gate status.

Use `reference/performance-report-template.md` for the report shape.
