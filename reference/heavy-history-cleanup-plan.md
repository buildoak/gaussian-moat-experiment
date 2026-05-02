# Heavy Git History Cleanup Plan

GitHub currently rejects `main` because historical commits contain blobs over
100MB. The working tree can be clean while push still fails, because GitHub
checks all objects introduced by the push.

## Known Oversized Blobs

Top offenders observed on 2026-05-02:

| Size | Path |
|---:|---|
| `2.09GB` | `tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/instance-35425891-final/snapshots/20260422T224733Z/r80m-visible.snapshot.bin` |
| `802MB` | `_archive/research-historical/results/moat-calibration-2026-03-23/w3-k32-R1500K-control-comp.json` |
| `780MB` | `_archive/research-historical/results/moat-calibration-2026-03-23/w4-k32-R2823K-moat-comp.json` |
| `662MB` | `_archive/research-historical/results/k36-ensemble-cal-2026-03-23/w2-k36-50M-control-wide.json` |
| `652MB` | `_archive/research-historical/results/k36-ensemble-cal-2026-03-23/w4-k36-80.4M-moat-wide.json` |
| `580MB` | `_archive/research-historical/results/moat-calibration-2026-03-23/w2-k26-R1016K-moat-comp.json` |
| `287MB` | `_archive/research-historical/results/moat-calibration-2026-03-23/w1-k26-R500K-control-comp.json` |
| `165MB` | `_archive/research-historical/results/k36-ensemble-cal-2026-03-23/w1-k36-50M-control-tight.json` |
| `162MB` | `_archive/research-historical/results/k36-ensemble-cal-2026-03-23/w3-k36-80.4M-moat-tight.json` |

## Minimal Safe Course

Do not run this plan without explicit user approval. It rewrites history.

1. Freeze work:

```bash
git status --short --branch
```

2. Create a full local backup outside the repo:

```bash
cd /Users/otonashi/thinking/building
BACKUP="gaussian-moat-cuda.backup.$(date +%Y%m%d-%H%M%S)"
cp -a gaussian-moat-cuda "$BACKUP"
git -C "$BACKUP" status --short --branch
```

3. Create a rescue bundle:

```bash
cd /Users/otonashi/thinking/building/gaussian-moat-cuda
mkdir -p ../repo-backups
git bundle create ../repo-backups/gaussian-moat-cuda-before-filter-$(date +%Y%m%d-%H%M%S).bundle --all
```

4. Confirm the bundle can be cloned:

```bash
tmpdir="$(mktemp -d)"
git clone ../repo-backups/gaussian-moat-cuda-before-filter-*.bundle "$tmpdir/check"
git -C "$tmpdir/check" log --oneline -3
```

5. Install/use `git-filter-repo` if needed:

```bash
git filter-repo --version
```

6. Remove generated/heavy paths from all history:

```bash
git filter-repo \
  --path tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/ \
  --path _archive/research-historical/results/ \
  --path legacy/_archive/research-historical/results/ \
  --path target/ \
  --path legacy/target/ \
  --path legacy/tile-probe/target/ \
  --path tiles-maxxing/campaign-sqrt-36/tile-cpp/census_output/ \
  --path tiles-maxxing/campaign-sqrt-40/tile-cpp/census_output/ \
  --path tiles-maxxing/tile-cpp/census_output/ \
  --invert-paths
```

7. Verify no blob over 100MB remains:

```bash
git rev-list --objects --all |
  git cat-file --batch-check='%(objecttype) %(objectname) %(objectsize) %(rest)' |
  awk '$1=="blob" && $3 > 100000000 {print $3, $4}'
```

Expected: no output.

8. Run secret checks from `reference/pre-push-secret-check.md`.

9. Run local correctness smoke:

```bash
cd tiles-maxxing/cpp-campaign-v2
cmake -S . -B build -DK_SQ=36 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
ctest --test-dir build --output-on-failure
```

10. Push only after choosing force semantics:

```bash
git push --force-with-lease origin main
```

## Safer Alternative

If preserving the current local history is more important than rewriting it,
create a clean publish branch from the current tree with `git checkout --orphan`
and push that branch to a new remote. This avoids rewriting local archaeology
but loses commit-level history on the public branch.
