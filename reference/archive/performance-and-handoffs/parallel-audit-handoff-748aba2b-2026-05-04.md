# Parallel Audit Handoff - 2026-05-04

This handoff records an external read-only audit stream run in parallel to the
K38/K40 Gaussian moat CUDA campaign. It is intended for the active campaign
session to inspect after it returns, without having to infer what happened from
git noise or remote state.

## Source Session

- Coordinator: Jenkins/Codex
- Gaal salt: `GAAL_SALT_854103566635e3dc`
- Gaal session id:
  `rollout-2026-05-04T15-59-57-019df2db-fe52-7a03-b040-3ee1748aba2b`
- Short gaal id: `748aba2b`
- Model: `gpt-5.5`
- Transcript:
  `/Users/otonashi/.gaal/data/codex/sessions/2026/05/04/748aba2b.md`

## Audit Artifacts

Isolated audit root, intentionally outside this repo:

```text
/Users/otonashi/thinking/pratchett-os/tmp/gaussian-moat-parallel-audit-20260504
```

Main audit note:

```text
/Users/otonashi/thinking/pratchett-os/tmp/gaussian-moat-parallel-audit-20260504/notes/k38-k40-evidence-audit.md
```

Pulled remote evidence mirror:

```text
/Users/otonashi/thinking/pratchett-os/tmp/gaussian-moat-parallel-audit-20260504/remote
```

Pulled evidence: 159 text/json/tsv/log/profile/BZ files, about 720K total.

## Key Takeaways

1. The collar/rounding concern appears resolved:
   - Tile collar / halo / face-strip depth should be `C = floor(sqrt(K))`.
   - For `K=38` and `K=40`, this means `C = 6`, not `7`.
   - `ceil(sqrt(K)) = 7` belongs only to conservative geo prefilter coverage,
     not the tile collar.

2. K38 evidence tightened after the original campaign transcript:
   - Current audited local-static-annulus bracket at width `32768`:
     - `R_inner = 71,875,000`: `SPANNING`
     - `R_inner = 73,437,500`: `MOAT`
   - All audited K38 rows are BZ-clean and overflow-clean.
   - The low `sqrt(38)` result looks surprising, but not obviously caused by a
     collar/rounding bug in the pulled artifacts.

3. K40 evidence from the pulled artifacts:
   - Current audited local-static-annulus bracket at width `32768`:
     - `R_inner = 978,000,000`: `SPANNING`
     - `R_inner = 980,000,000`: `MOAT`
   - K40 `980M` no-early diagnostic was observed running, but was not complete
     during this audit. Do not treat it as evidence until its completed
     verdict/profile are pulled.

4. Golden/oracle cleanup was handled separately:
   - Commit: `b6b82d8 Fix non-square golden geo oracle`
   - The stale widened-ceil Python oracle logic was patched to canonical
     integer norm-form.
   - CPU-only checks passed:
     - `uv run --script tiles-maxxing/cpp-campaign-v2/goldens/test_preflight_oracle_geo.py`
     - `uv run python -m py_compile ...`
     - K38/K40 `GeoTests` passed
     - K36 `GeoTests` passed with the non-square test skipped as expected
     - `git diff --check` passed

## Request To Campaign Session

Please consider this external audit before finalizing K38/K40 status:

1. Read the audit note at:
   `/Users/otonashi/thinking/pratchett-os/tmp/gaussian-moat-parallel-audit-20260504/notes/k38-k40-evidence-audit.md`
2. Pull/reconcile any completed remote diagnostic artifacts that appeared after
   this audit, especially `k40-980m-diag-20260504T124022Z`.
3. Decide whether the K38 `71.875M SPANNING` / `73.4375M MOAT` bracket should
   get an additional no-early or trace-path diagnostic.
4. Account for commit `b6b82d8` when reasoning about golden/preflight oracle
   surfaces.

Reminder: all results above are local static-annulus detector evidence only.
They are not origin-component moat proofs.
