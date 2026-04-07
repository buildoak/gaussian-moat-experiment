---
date: 2026-03-19
engine: coordinator
status: ready
campaign: sqrt(40) upper-bound
target_k_squared: 40
---

# sqrt(40) Upper-Bound Campaign Plan

## Objective

Find the Gaussian moat upper bound for k = sqrt(40) (k^2 = 40).
This is the next target after the k^2 = 36 campaign.

## Instance Requirements

| Requirement | Value |
|---|---|
| GPU | RTX 3090 (24 GB VRAM) |
| CUDA Compute | SM 8.6 (Ampere) |
| RAM | >= 32 GB (sieve + solver working sets) |
| Disk | >= 20 GB (GPRF files, logs, build artifacts) |
| SSH | Direct port access required |
| OS | Ubuntu/Debian with CUDA toolkit |

### Top 3 Candidates (vast.ai, 2026-03-19)

| ID | $/hr | RAM | Disk | Net Down | Location | Notes |
|---|---|---|---|---|---|---|
| 32252156 | $0.1241 | 32 GB | 57 GB | 920 Mbps | Quebec, CA | Cheapest with SSH, low vCPUs (3) but fine for GPU workload |
| 29825770 | $0.1603 | 16 GB | 730 GB | 303 Mbps | BC, Canada | RAM borderline at 16 GB -- may OOM on large sieve windows |
| 16320074 | $0.1620 | 43 GB | 108 GB | 7.5 Gbps | Spain | Best net speed, ample RAM |

**Recommendation:** ID 32252156 at $0.1241/hr. 32 GB RAM is sufficient, Quebec location has reasonable latency. Low vCPU count (3) is irrelevant -- work is GPU-bound. Disk is tight at 57 GB but sufficient (GPRF files are ephemeral, cleaned after each probe).

**Alternative:** ID 16320074 at $0.1620/hr if build speed matters (21 vCPUs for faster `cargo build --release`).

**IMPORTANT: SM 8.6 build target.** The current CMakeLists.txt has `jetson` (SM 8.7), `a100` (SM 8.0), and `4090` (SM 8.9). RTX 3090 is SM 8.6 (Ampere). Options:
- Build with `-DTARGET_DEVICE=a100` (SM 8.0) -- forward-compatible, runs on 3090 but may miss SM 8.6 optimizations.
- **Better:** Add a 3090 target to CMakeLists.txt (`CMAKE_CUDA_ARCHITECTURES 86`) before deploying. Or pass `-DCMAKE_CUDA_ARCHITECTURES=86` directly to cmake.

## Build Steps (on instance)

```bash
# 1. rsync repo to instance
rsync -avz --delete \
    --exclude '.git' --exclude 'build/' --exclude 'solver/target/' --exclude 'tmp/' \
    -e "ssh -p $PORT" \
    /Users/otonashi/thinking/building/gaussian-moat-cuda/ \
    root@$HOST:/gaussian-moat-cuda/

# 2. SSH in and build
ssh -p $PORT root@$HOST

# 3. Install deps
apt-get update -qq && apt-get install -y -qq cmake build-essential pkg-config curl ca-certificates

# 4. Rust toolchain
curl https://sh.rustup.rs -sSf | sh -s -- -y && . "$HOME/.cargo/env"

# 5. Build CUDA sieve (SM 8.6 for RTX 3090)
cd /gaussian-moat-cuda
mkdir -p build-3090 && cd build-3090
cmake .. -DCMAKE_CUDA_ARCHITECTURES=86
make -j$(nproc) gm_cuda_primes

# 6. Build Rust solver
cd /gaussian-moat-cuda/solver
cargo build --release

# 7. Verify binaries exist
ls -la /gaussian-moat-cuda/build-3090/gm_cuda_primes
ls -la /gaussian-moat-cuda/solver/target/release/gaussian-moat-solver
```

Or use the existing deploy script (adapting for 3090):
```bash
./deploy/a100-deploy.sh "ssh -p $PORT root@$HOST"
# Then on remote: rebuild with SM 86 instead of SM 80
```

## Campaign Phases

### Phase 0: k^2=36 Validation (GATE)

Validate the solver works correctly on this instance before running the real campaign.
Use k^2=36 where we know the expected result from the Jetson campaign.

```bash
cd /gaussian-moat-cuda

# Sieve a small window around the known k^2=36 moat region
./build-3090/gm_cuda_primes \
    --norm-lo 0 --norm-hi 10000 \
    --output /tmp/gm-validate.gprf --mode sieve

# Solve with k^2=2 (known moat at (11,4), distance sqrt(137))
./solver/target/release/gaussian-moat-solver \
    --k-squared 2 --angular 0 \
    --prime-file /tmp/gm-validate.gprf --profile
```

**Gate:** Output must contain `farthest point: (11, 4)`. If not, STOP -- binary is broken.

### Phase 1: Geometric Bracketing

Determine the rough scale of the sqrt(40) moat. Run three probes at geometrically increasing distances.

The UB probe starts at distance D, seeds the band [D-k, D] as auto-connected, and sweeps outward.
If the component stops growing before reaching the file edge, moat is confirmed at that distance.

```bash
SIEVE=/gaussian-moat-cuda/build-3090/gm_cuda_primes
SOLVER=/gaussian-moat-cuda/solver/target/release/gaussian-moat-solver

# Probe 1: 200M (lightweight, ~2-5 min)
python3 deploy/ub-campaign.py \
    --k-squared 40 --start-distance 200000000 --ceiling 200100000 \
    --sweep-mode sweep --no-overlap --verbose \
    --sieve-bin $SIEVE --solver-bin $SOLVER \
    --wedges 8 --shell-width 5 \
    --max-runtime-minutes 30 \
    --tag sqrt40-bracket-200M

# Probe 2: 400M
python3 deploy/ub-campaign.py \
    --k-squared 40 --start-distance 400000000 --ceiling 400100000 \
    --sweep-mode sweep --no-overlap --verbose \
    --sieve-bin $SIEVE --solver-bin $SOLVER \
    --wedges 8 --shell-width 5 \
    --max-runtime-minutes 30 \
    --tag sqrt40-bracket-400M

# Probe 3: 800M
python3 deploy/ub-campaign.py \
    --k-squared 40 --start-distance 800000000 --ceiling 800100000 \
    --sweep-mode sweep --no-overlap --verbose \
    --sieve-bin $SIEVE --solver-bin $SOLVER \
    --wedges 8 --shell-width 5 \
    --max-runtime-minutes 30 \
    --tag sqrt40-bracket-800M
```

**Decision tree after Phase 1:**

| 200M | 400M | 800M | Interpretation | Next |
|---|---|---|---|---|
| moat | moat | moat | Moat is < 200M | Phase 2: bisect [0, 200M] |
| no moat | moat | moat | Moat is in [200M, 400M] | Phase 2: bisect [200M, 400M] |
| no moat | no moat | moat | Moat is in [400M, 800M] | Phase 2: bisect [400M, 800M] |
| no moat | no moat | no moat | Moat is > 800M | Extend: try 1.6B, 3.2B |

"moat" = status `moat_confirmed` in the probe output.
"no moat" = status `inconclusive_edge` (component still growing at file edge).

### Phase 2: Log-Bisection Protocol

Once Phase 1 brackets the moat to an interval [L, H], use bisection to narrow it.

```bash
# Example: if moat bracketed to [200M, 400M]
# Bisect at midpoint 300M
python3 deploy/ub-campaign.py \
    --k-squared 40 --start-distance 300000000 --ceiling 300100000 \
    --sweep-mode sweep --no-overlap --verbose \
    --sieve-bin $SIEVE --solver-bin $SOLVER \
    --wedges 8 --shell-width 5 \
    --max-runtime-minutes 30 \
    --tag sqrt40-bisect-300M
```

Repeat bisection: if moat confirmed at 300M, narrow to [200M, 300M]; if not, narrow to [300M, 400M].
Continue until interval width < 10M.

**Bisection protocol:**
1. `mid = (L + H) / 2`
2. Probe at `mid`
3. If moat confirmed: `H = mid`
4. If no moat: `L = mid`
5. Repeat until `H - L < 10M`
6. When narrow enough, proceed to Phase 3

Alternatively, use ub-campaign.py's built-in bisect mode:
```bash
python3 deploy/ub-campaign.py \
    --k-squared 40 --start-distance $L --ceiling $H \
    --mode bisect --bisect-tolerance 10000000 \
    --sweep-mode sweep --no-overlap --verbose \
    --sieve-bin $SIEVE --solver-bin $SOLVER \
    --wedges 8 --shell-width 5 \
    --max-runtime-minutes 120 \
    --tag sqrt40-bisect
```

### Phase 3: Exact Resolution

Once the moat is bracketed to a ~10M window, run progressive sweep across the full window
to find the exact farthest point.

```bash
python3 deploy/ub-campaign.py \
    --k-squared 40 --start-distance $NARROW_L --ceiling $NARROW_H \
    --mode progressive --sweep-mode sweep --no-overlap --verbose \
    --sieve-bin $SIEVE --solver-bin $SOLVER \
    --wedges 8 --shell-width 10 \
    --max-iterations 500 \
    --max-runtime-minutes 180 \
    --tag sqrt40-exact
```

**Final gate:** The last probe with `moat_confirmed` status gives the upper bound.
Record `farthest_point`, `farthest_distance`, `component_size`.

## Safeguards

### 1. Timeout per probe
- `--max-runtime-minutes 30` on each Phase 1 probe
- Individual probe timeout computed by ub-campaign.py based on norm range estimate
- **Rule:** If a single probe exceeds 30 minutes, something is wrong (OOM, sieve stall, GPU hang). Kill and investigate.

### 2. Memory monitoring
Before each phase, check GPU memory:
```bash
nvidia-smi --query-gpu=memory.used,memory.free,memory.total --format=csv,noheader
```
- RTX 3090 has 24 GB VRAM. Sieve + solver should use < 20 GB.
- If `memory.free < 4 GB` before a probe, stop and investigate.
- Use `--no-overlap` always (prevents parallel sieve + solver memory stacking).
- System RAM: watch with `free -h`. GPRF files are memory-mapped; 32 GB system RAM is sufficient.

### 3. Checkpoint after each probe
- ub-campaign.py automatically appends each probe result to `$WORK_DIR/ub_$TAG.jsonl`
- After each phase, copy results to local:
  ```bash
  scp -P $PORT root@$HOST:/tmp/gm-ub/ub_sqrt40-*.jsonl \
      /Users/otonashi/thinking/building/gaussian-moat-cuda/research/results/
  ```
- Keep a running log on the instance:
  ```bash
  tee -a /gaussian-moat-cuda/sqrt40_campaign.log
  ```

### 4. Cost cap: $5 instance budget
- At $0.1241/hr (cheapest candidate), $5 buys ~40 hours of compute.
- Set a wall-clock alarm: `--max-runtime-minutes 2400` (40 hours) on the outer campaign.
- Monitor elapsed cost: `vastai show instances` shows accumulated cost.
- **Hard rule:** If total instance cost exceeds $5, destroy immediately:
  ```bash
  vastai destroy instance $INSTANCE_ID
  ```
- Estimated cost for full campaign: $1-3 (see estimates below).

### 5. Crash recovery (SSH drop)
- Run campaign inside `tmux` on the instance:
  ```bash
  tmux new -s sqrt40
  # ... run campaign inside tmux ...
  # If disconnected:
  ssh -p $PORT root@$HOST
  tmux attach -t sqrt40
  ```
- ub-campaign.py writes results to JSONL after each probe. On restart, can inspect last result and resume from that distance.
- GPRF files are ephemeral -- each probe generates fresh. No need to preserve intermediate files.
- To resume after crash:
  1. Check last probe: `tail -1 /tmp/gm-ub/ub_sqrt40-*.jsonl | python3 -m json.tool`
  2. Read `next_start_distance` from last record
  3. Re-run ub-campaign.py with `--start-distance <that_value>`

## Time and Cost Estimates

### Per-probe timing (estimated from k^2=36 data on 3090-class GPU)

| Distance | Norm range | Est. primes | Sieve time | Solve time | Total |
|---|---|---|---|---|---|
| 200M | ~40B span | ~1.7B | ~10 min | ~5 min | ~15 min |
| 400M | ~80B span | ~3.1B | ~20 min | ~10 min | ~30 min |
| 800M | ~160B span | ~5.5B | ~35 min | ~15 min | ~50 min |

Note: These are rough estimates. 3090 sieve throughput is ~2-3M primes/sec.
Actual timing depends on prime density at that distance and chunking behavior.

### Total campaign estimate

| Phase | Probes | Est. time | Est. cost |
|---|---|---|---|
| Phase 0: Validation | 1 | 2 min | $0.01 |
| Phase 1: Bracketing | 3 | 15-50 min each = 1-2.5 hr | $0.12-0.31 |
| Phase 2: Bisection | 5-8 | 10-30 min each = 1-4 hr | $0.12-0.50 |
| Phase 3: Exact | 10-50 | 5-15 min each = 1-12 hr | $0.12-1.49 |
| **Total** | **19-62** | **3-19 hr** | **$0.37-2.31** |

Well within the $5 budget. Even worst case is under $3.

## Decision Points

### If moat is at < 50M distance
- Surprisingly small. Verify with a second independent probe from distance 0.
- Cross-check: run the full LB campaign (chunked-lb.sh) to confirm the farthest point.
- This would be a very fast campaign -- under 1 hour total.

### If moat is in 50M-500M range
- Expected range based on k^2=36 scaling.
- Standard bisection protocol applies.
- Total campaign: 4-10 hours, $0.50-1.25.

### If moat is > 1B distance
- Much larger than expected. May indicate a scaling law change at k^2=40.
- Increase cost budget to $10 if needed.
- Consider: is the instance fast enough? May need to switch to a 4090 ($0.20-0.30/hr) for 50% faster sieve throughput.
- Run overnight with generous `--max-runtime-minutes`.

## Pre-flight Checklist

- [ ] Rent instance: `vastai create instance $OFFER_ID --image pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel --disk 30 --ssh --onstart-cmd 'apt-get update && apt-get install -y tmux'`
- [ ] Get SSH info: `vastai show instances --raw`
- [ ] Deploy: rsync + build (see Build Steps above)
- [ ] Validate: Phase 0 gate (k^2=2 farthest at (11,4))
- [ ] Run Phase 1 in tmux
- [ ] Copy results locally after each phase
- [ ] Destroy instance when done: `vastai destroy instance $ID`

## Reference

- k^2=36 moat at distance < 80,015,782 (from MEMORY.md)
- k^2=32 LB campaign: instance 32952569, chunked-lb.sh, checkpoints, ~19 hours
- Deploy script: `deploy/a100-deploy.sh` (adapt for 3090 SM 8.6)
- Campaign runner: `deploy/ub-campaign.py` (supports progressive, bisect, sweep modes)
- Solver binary: `solver/target/release/gaussian-moat-solver` (Rust, clap CLI)
- Sieve binary: `build-3090/gm_cuda_primes` (CUDA, SM 8.6)
