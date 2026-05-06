# vast.ai Operational Guide - gaussian-moat CUDA

Operational reference for renting, deploying, building, and running the current
CUDA campaign on vast.ai. The current accepted evidence path is the compact
post-flight verification spine: exact profile, independent tile sample audit,
SPANNING cert when applicable, and MOAT hardening. Tsuchimura's adjacent K36
pair remains a calibration note, not the primary acceptance gate.

## Current Target

Use `tiles-maxxing/cuda-campaign-v2-sqrt-36/` for validated k^2=36 CUDA moat runs. It depends on `tiles-maxxing/cpp-campaign-v2/` through CMake, so deploy both trees with the same relative layout.

The older `campaign-sqrt-36/` and `campaign-sqrt-40/` trees, including their `tile_cuda_multi_kernel/` subtrees, are useful historical/performance references only. Do not use `tiles-maxxing/vast-ai/sweep-k36-postfix.sh` as a moat-detection template; it targets the stale v1 campaign layout and predates the corrected `R_inner` semantics.

## Quick Reference

```bash
# CLI location, if using the temp venv convention
VASTAI=/tmp/vastai-env/bin/vastai

# Search for a single RTX 4090. A 3090 also works, but the verified boundary
# campaign was run on a 4090.
$VASTAI search offers 'gpu_name=RTX_4090 cuda_vers>=12.0 disk_space>=40 num_gpus=1 dph<=0.35 reliability>=0.95' -o 'dph'

# Rent instance
$VASTAI create instance $OFFER_ID \
  --image pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel \
  --disk 40 --ssh --onstart-cmd 'apt-get update && apt-get install -y tmux cmake ninja-build rsync'

# Get SSH info once, then pin HOST and PORT for the whole session
$VASTAI show instances --raw | python3 -c "import sys,json; d=json.load(sys.stdin); [print(f'ID={i[\"id\"]} ssh -p {i[\"ports\"][\"22/tcp\"][0][\"HostPort\"]} root@{i[\"ssh_host\"]}') for i in d]"

# Check cost and state
$VASTAI show instances
```

Pin the SSH endpoint at session start:

```bash
HOST=sshX.vast.ai
PORT=12345
SSH_CMD="ssh -o StrictHostKeyChecking=accept-new -p $PORT root@$HOST"
```

## Architecture Mapping

| GPU | SM | CMake architecture | Notes |
|-----|----|--------------------|-------|
| RTX 4090 | 8.9 | `89` | Primary validated campaign target |
| RTX 3090 | 8.6 | `86` | Good lower-cost fallback |
| A100 | 8.0 | `80` | Also viable |
| Jetson Orin | 8.7 | `87` | Legacy/local fallback, not primary compute |

## Deploy

Run from the repository root on the Mac:

```bash
REMOTE=/workspace/gaussian-moat-cuda

rsync -avz --delete \
  --exclude '.git' \
  --exclude 'build*/' \
  --exclude '**/build*/' \
  --exclude '**/artifacts/' \
  --exclude '**/results/' \
  --exclude '**/profiles/' \
  --exclude '**/runs/' \
  --exclude '**/tmp/' \
  --exclude '**/*.bin' \
  --exclude '**/*.log' \
  -e "ssh -o StrictHostKeyChecking=accept-new -p $PORT" \
  ./ root@$HOST:$REMOTE/
```

For long transfers or builds, run inside `tmux` on the remote:

```bash
$SSH_CMD
tmux new -s build
cd /workspace/gaussian-moat-cuda
```

## Build

Build the validated CUDA campaign:

```bash
cd /workspace/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36
cmake -S . -B build-k36 -DK_SQ=36 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-k36 -j"$(nproc)"
ctest --test-dir build-k36 --output-on-failure
```

Use `-DCMAKE_CUDA_ARCHITECTURES=86` for a 3090, `80` for A100, or `87` for Jetson. The current CMake default is already `89`; do not patch local Makefiles for the v2 campaign.

## Correct Static-Annulus Semantics

The current CUDA campaign computes static annulus crossing: `SPANNING` means
some Gaussian-prime component connects `geo_I` to `geo_O`; `MOAT` means no such
component was detected for that annulus. It is not an exact origin-component
computation.

For K36 hardening near 80M, keep `R_inner` anchored or deliberately define a
radius-translation matrix, then vary `R_outer` by width.

Correct:

```bash
--r-inner=80000000 --r-outer=<tested_outer_radius> --region full-octant
```

Different question:

```bash
--r-inner=<tested_radius> --r-outer=<tested_radius + 8192>
```

That form asks a translated shell question. It can be valuable for radial
stability experiments, but it must not be confused with the anchored K36
hardening matrix.

## Validated Runs

Current K36 hardening checkpoint, `k^2=36`, `R_inner=80,000,000`:

```bash
cd /workspace/gaussian-moat-cuda

python3 verification/postflight/moat_hardening_matrix.py \
  --out-root /workspace/runs/k36-hardening \
  --widths 17000 18000 19000 20000 32768 \
  --campaign-bin tiles-maxxing/cuda-campaign-v2-sqrt-36/build-k36/campaign_main_cuda \
  --execute
```

Expected current K36 matrix verdicts:

| Width | Expected verdict | Role |
|---:|---|---|
| 17,000 | MOAT | primary hardening anchor |
| 18,000 | MOAT | width monotonicity check |
| 19,000 | MOAT | width monotonicity check |
| 20,000 | MOAT | width monotonicity check |
| 32,768 | MOAT | wider confirmation row |

Adjacent Tsuchimura calibration note:

| `R_outer` with `R_inner=80,000,000` | Expected verdict | Role |
|---:|---|---|
| 80,015,782 | SPANNING | SPANNING cert sanity |
| 80,015,790 | MOAT | boundary-adjacent calibration |

Do not describe this pair as the primary current gate or as proof of exact
origin reachability.

The CUDA campaign default is verdict-only. Use `--snapshot-out` only when
snapshot parity is the objective; snapshot mode intentionally disables early
exit and writes every TileOp.

## Sweep Template

Use the same anchored `R_inner` for every point:

```bash
mkdir -p /workspace/sweeps/boundary/logs
cd /workspace/gaussian-moat-cuda/tiles-maxxing/cuda-campaign-v2-sqrt-36

for R_OUTER in 80015000 80015700 80015780 80015782 80015790 80015800; do
  ./build-k36/campaign_main_cuda \
    --k-sq=36 \
    --r-inner=80000000 \
    --r-outer="$R_OUTER" \
    --region full-octant \
    --chunk-size=200000 \
    --timing \
    --profile "/workspace/sweeps/boundary/r${R_OUTER}.profile.json" \
    > "/workspace/sweeps/boundary/logs/r${R_OUTER}.log" 2>&1
done
```

Keep each sweep in a timestamped directory and pull logs/profiles back before destroying the instance.

## Pull Results

```bash
mkdir -p tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/vast-ai-pull
rsync -avz \
  -e "ssh -o StrictHostKeyChecking=accept-new -p $PORT" \
  root@$HOST:/workspace/sweeps/ \
  tiles-maxxing/cuda-campaign-v2-sqrt-36/artifacts/vast-ai-pull/
```

## Cost and Cleanup

Use `tmux` for any command over 5 seconds. SSH drops are common; `tmux` keeps builds and sweeps alive.

If the task explicitly includes cleanup or the user asks you to stop billing,
destroy the instance immediately after pulling results:

```bash
$VASTAI destroy instance $ID
$VASTAI show instances
```

If the instance was already running before your task, do not destroy it unless the user explicitly asks.

## References

- `AGENTS.md` - project correctness hierarchy and current gate canon
- `reference/current-verification-spine.md` - active verification spine
- `verification/postflight/README.md` - post-flight contract
- `methodology/tile-operator-definition-v-claude.md` - mathematical TileOp
  contract
