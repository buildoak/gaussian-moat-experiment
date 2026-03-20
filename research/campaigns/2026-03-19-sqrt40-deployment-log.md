---
date: 2026-03-19
engine: coordinator
status: running
campaign: sqrt(40) upper-bound
target_k_squared: 40
---

# sqrt(40) Campaign Deployment Log

## Instance Details

| Field | Value |
|---|---|
| Instance ID | 33123347 |
| Provider | vast.ai |
| GPU | NVIDIA GeForce RTX 3090 (SM 8.6, 24 GB VRAM) |
| RAM | 257 GB system |
| CUDA | 12.4 |
| Cost | $0.128/hr |
| SSH | `ssh -p 13346 root@ssh2.vast.ai` |
| Image | pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel |
| Disk | 20 GB |

## Build Summary

- **CUDA sieve:** Built with `-DTARGET_DEVICE=3090` (SM 8.6), binary at `/root/gaussian-moat-cuda/build/gm_cuda_primes`
- **Rust solver:** `cargo build --release`, binary at `/root/gaussian-moat-cuda/solver/target/release/gaussian-moat-solver`
- **Rust version:** 1.94.0 (2026-03-02)
- **Smoke test:** PASSED -- k^2=2 farthest point (11,4) verified

## Phase 0 Validation (k^2=36)

- **Command:** `ub-campaign.py --k-squared 36 --start-distance 85000000 --ceiling 85000000`
- **Result:** `inconclusive_edge` (expected for thin window -- component reached file edge at 85000012)
- **Primes:** 41,898,839 generated in 8.7s sieve + 22.6s solve = 31.3s total
- **Throughput:** 1.34M primes/sec effective
- **Assessment:** PASSED -- binaries functional, GPU throughput confirmed

## Campaign Start

- **Start time:** 2026-03-19 04:35:30 UTC
- **Local tmux session:** `sqrt40-campaign`
- **Command:**
  ```
  python3 deploy/ub-campaign.py \
      --k-squared 40 --start-distance 200000000 --ceiling 2000000000 \
      --sweep-mode sweep --no-overlap --verbose \
      --platform custom \
      --sieve-bin build/gm_cuda_primes \
      --solver-bin solver/target/release/gaussian-moat-solver \
      2>&1 | tee /root/sqrt40-campaign.log
  ```
- **Note:** Campaign auto-detected large prime count (46B estimated) and switched to chunked sweep mode

## Monitoring Commands

```bash
# Attach to live campaign output
tmux attach -t sqrt40-campaign

# Check remote log directly
ssh -p 13346 root@ssh2.vast.ai 'tail -50 /root/sqrt40-campaign.log'

# Check JSONL results
ssh -p 13346 root@ssh2.vast.ai 'cat /tmp/gm-ub/ub_k40-campaign.jsonl | python3 -m json.tool'

# Check last probe result
ssh -p 13346 root@ssh2.vast.ai 'tail -1 /tmp/gm-ub/ub_k40-campaign.jsonl | python3 -m json.tool'

# GPU memory check
ssh -p 13346 root@ssh2.vast.ai 'nvidia-smi --query-gpu=memory.used,memory.free,memory.total --format=csv,noheader'

# Copy results to local
scp -P 13346 root@ssh2.vast.ai:/tmp/gm-ub/ub_k40-campaign.jsonl \
    /Users/otonashi/thinking/building/gaussian-moat-cuda/research/results/

# Check instance cost
vastai show instances --raw | python3 -c "import sys,json;d=json.load(sys.stdin);[print(f'cost so far: \${i.get(\"total_dph_cost\",0):.2f}') for i in d if i['id']==33123347]"
```

## Destroy Instance When Done

```bash
vastai destroy instance 33123347
```

## Hung Chunk Planner Fix (04:46 UTC)

**Problem:** Initial launch at 04:35 hung immediately. The campaign tried to sweep distance=[200M, 2B] as one call, estimated 46B primes, switched to chunked sweep, and began computing ~50M chunks. This was the wrong approach -- one massive sweep across the full range.

**Root cause:** Two bugs in `ub-campaign.py`:
1. **Duplicate `_time_remaining_s` method** (lines 73 and 93). The second definition (line 93) shadowed the first and returned `None` instead of `float('inf')` when `--max-runtime-minutes` was not set. This caused `TypeError: '<=' not supported between instances of 'NoneType' and 'int'` at line 712 in the chunked sweep loop.
2. **Architectural issue:** Even a narrow 10k-distance band generates a huge norm range at distance 200M (norm ~ 4e16), which exceeds the 150M prime cap and triggers chunked sweep with 1430 chunks per probe. This is expected behavior for high-distance probes -- the chunked sweep is the correct mechanism.

**Fix applied:**
- Removed dead first `_time_remaining_s` definition
- Fixed surviving definition to return `float('inf')` instead of `None` when no runtime limit set
- File: `deploy/ub-campaign.py`, synced to remote via scp

**New launch method:**
- Individual geometric bracketing probes (200M, 400M, 800M, 100M, 50M) instead of one 200M-2B sweep
- Each probe sweeps a narrow 10k-distance band: `--start-distance D --ceiling D+10000`
- Script: `/root/run-sqrt40.sh` with `set -euo pipefail`, 30-min timeout per probe
- Runs in **remote tmux** (`tmux new-session -d -s sqrt40`) -- SSH drops no longer kill the campaign
- Session metadata: `/root/sqrt40-session-meta.json`

**Campaign relaunched:** 04:47:44 UTC. First probe (distance=200M) actively sieving chunk 1 of 1430.

## Updated Monitoring Commands

```bash
# Attach local tmux (already monitoring remote)
tmux attach -t sqrt40-campaign

# Or SSH directly to remote tmux
ssh -p 13346 root@ssh2.vast.ai 'tmux attach -t sqrt40'

# Check remote log
ssh -p 13346 root@ssh2.vast.ai 'tail -50 /root/sqrt40-campaign.log'

# Check JSONL results
ssh -p 13346 root@ssh2.vast.ai 'tail -1 /tmp/gm-ub/ub_k40-campaign.jsonl | python3 -m json.tool'

# Session metadata
ssh -p 13346 root@ssh2.vast.ai 'cat /root/sqrt40-session-meta.json'

# GPU memory/temp check
ssh -p 13346 root@ssh2.vast.ai 'nvidia-smi --query-gpu=memory.used,memory.free,temperature.gpu --format=csv,noheader'

# Copy results to local
scp -P 13346 root@ssh2.vast.ai:/tmp/gm-ub/ub_k40-campaign.jsonl \
    /Users/otonashi/thinking/building/gaussian-moat-cuda/research/results/
```

## CMakeLists.txt Change

Committed as `65dc2f7`: Added RTX 3090 (SM 8.6) as `-DTARGET_DEVICE=3090` target alongside jetson/a100/4090.

## Campaign Relaunch: Direct Bracket Probing (05:03 UTC)

**Problem:** Sweep mode was wrong approach for bracket probing. Even with the `_time_remaining_s` fix, each probe at D=200M generates a norm range of ~5.6e15, which exceeds the 150M prime cap and triggers chunked sweep with 1430 chunks per probe. This is architecturally correct for full sweeps but catastrophically wrong for quick bracket probes.

**Fix:**
- Killed the hung chunked sweep campaign
- Added paper-quality logging to `ub-campaign.py`:
  - Session header record: GPU info, binary SHA256 checksums, CUDA version, argv
  - Per-probe `probe_start_utc` / `probe_end_utc` timestamps
  - GPU memory usage after each sieve
- Created `deploy/sqrt40-bracket.sh`: direct sieve+solver binary calls, 1 call per probe
  - Bypasses ub-campaign.py entirely for bracket probing
  - Phase 0: k^2=36 validation at D=85M (known boundary)
  - Phase 1: 5 geometric bracket probes for k^2=40 (D=200M, 400M, 100M, 50M, 800M)
- Committed as `0dd7e32`

**Results location:** `research/results/sqrt40-bracket-log.jsonl` + `.txt`

**Phase 0 Validation Result:**
- k^2=36, D=85M: sieve 6.4s (32.6M primes), solver 15.3s
- Farthest point: (66374049, 53098840), norm=7.225e15 (at file edge)
- Status: `inconclusive_edge` (expected for narrow validation band)
- Throughput: 2.1M primes/sec solver, 5.1M primes/sec sieve
- PASSED: binaries confirmed functional on RTX 3090

**Phase 1 In Progress:**
- Probe D=200M: sieve produced 73.2M primes in ~14s, solver running
- Expected: 5 bracket probes x ~2 min each = ~10 min total for Phase 1

**Monitoring:**
```bash
# Live output
tmux attach -t sqrt40-campaign  # local (watching remote tmux)
ssh -p 13346 root@ssh2.vast.ai 'tmux attach -t sqrt40'  # direct

# Check results
ssh -p 13346 root@ssh2.vast.ai 'tail -30 /root/sqrt40-bracket.log'
ssh -p 13346 root@ssh2.vast.ai 'cat /root/gaussian-moat-cuda/research/results/sqrt40-bracket-log.jsonl.txt'

# Copy results to local
scp -P 13346 root@ssh2.vast.ai:/root/gaussian-moat-cuda/research/results/sqrt40-bracket-log.jsonl.txt \
    /Users/otonashi/thinking/building/gaussian-moat-cuda/research/results/
```

## Redeployment: Larger Instance (07:20 UTC)

**Problem:** Instance 33123347 (20 GB disk) ran out of disk at the 3.2B probe. Prime file for 3.2B distance is ~16 GB, leaving no room for working space. Probes 50M through 1.6B completed successfully (component survived at all distances).

**Actions:**
1. Destroyed instance 33123347
2. Searched for RTX 3090 with 50+ GB disk under $0.20/hr (found 30 offers)
3. First attempt (ID 31126091, California): SSH port forwarding broken -- destroyed immediately
4. Second attempt (ID 25092516, California): successful

**New Instance Details:**

| Field | Value |
|---|---|
| Instance ID | 33129683 |
| Provider | vast.ai |
| GPU | NVIDIA GeForce RTX 3090 (SM 8.6, 24 GB VRAM) |
| CUDA driver | 570.190 |
| CUDA toolkit | 12.4 |
| Cost | $0.150/hr |
| SSH | `ssh -p 19682 root@ssh9.vast.ai` |
| Image | pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel |
| Disk | 50 GB (49 GB free) |
| Location | California, US |

**Build:**
- CUDA sieve: SM 8.6 target, sha256=`ca754c929e27a388949f15983320d33fe20b0e36395f723b98b5a3462b1d48d1`
- Rust solver: sha256=`8e517b8ca9707c72e27edbc89ac6b0102e2de378f719c58d778619019b22ebf2`
- Rust 1.94.0

**Phase 0 Re-validation (k^2=36, D=85M):**
- Result: `inconclusive_edge` at distance 85000054 (expected)
- Primes: 139,660,600 generated in 48.1s sieve + 63.2s solve = 111.3s total
- Component: 106,457,316 / 139,660,600 in main component
- PASSED: binaries functional on new instance

**Phase 3 Campaign: Geometric Probes (started ~07:30 UTC)**

Probes queued sequentially in tmux session `sqrt40`:
1. **2.4B** -- intermediate step between 1.6B and 3.2B (running)
2. **3.2B** -- previously crashed on disk-full
3. **6.4B** -- next geometric step

Command pattern for each probe:
```
python3 deploy/ub-campaign.py \
    --k-squared 40 --start-distance D --ceiling D \
    --sweep-mode sweep --no-overlap --verbose \
    --platform custom \
    --sieve-bin build/gm_cuda_primes \
    --solver-bin solver/target/release/gaussian-moat-solver \
    --tag sqrt40-phase3-{D} \
    --work-dir /root/gaussian-moat-cuda/sqrt40-campaign
```

**Log files:**
- Per-probe JSONL: `/root/gaussian-moat-cuda/sqrt40-campaign/ub_sqrt40-phase3-{D}.jsonl`
- Combined text log: `/root/gaussian-moat-cuda/sqrt40-phase3-log.txt`

**Monitoring:**
```bash
# Attach to remote tmux
ssh -p 19682 root@ssh9.vast.ai 'tmux attach -t sqrt40'

# Check log
ssh -p 19682 root@ssh9.vast.ai 'tail -50 /root/gaussian-moat-cuda/sqrt40-phase3-log.txt'

# Check last probe result
ssh -p 19682 root@ssh9.vast.ai 'tail -1 /root/gaussian-moat-cuda/sqrt40-campaign/ub_sqrt40-phase3-2.4B.jsonl | python3 -m json.tool'

# GPU status
ssh -p 19682 root@ssh9.vast.ai 'nvidia-smi --query-gpu=utilization.gpu,memory.used,temperature.gpu --format=csv,noheader'

# Disk check
ssh -p 19682 root@ssh9.vast.ai 'df -h / && ls -lh /root/gaussian-moat-cuda/sqrt40-campaign/'

# Copy results to local
scp -P 19682 root@ssh9.vast.ai:/root/gaussian-moat-cuda/sqrt40-campaign/*.jsonl \
    /Users/otonashi/thinking/building/gaussian-moat-cuda/research/results/
```

**Destroy when done:**
```bash
vastai destroy instance 33129683
```
