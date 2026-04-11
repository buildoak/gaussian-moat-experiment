# vast.ai Operational Guide — tiles-maxxing

Operational reference for renting, deploying, and running CUDA tile-processing kernels on vast.ai GPU instances. Lessons learned from prior campaigns (March 2026) are baked in.

## Quick Reference

```bash
# CLI location (temp venv — reinstall if missing)
VASTAI=/tmp/vastai-env/bin/vastai

# Search for 3090 offers
$VASTAI search offers 'gpu_name=RTX_3090 cuda_vers>=12.0 disk_space>=20 num_gpus=1 dph<=0.20 reliability>=0.95' -o 'dph'

# Rent instance
$VASTAI create instance $OFFER_ID --image pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel --disk 25 --ssh --onstart-cmd 'apt-get update && apt-get install -y tmux'

# Get SSH info
$VASTAI show instances --raw | python3 -c "import sys,json; d=json.load(sys.stdin); [print(f'ssh -p {i[\"ports\"][\"22/tcp\"][0][\"HostPort\"]} root@{i[\"ssh_host\"]}') for i in d]"

# Check cost
$VASTAI show instances

# DESTROY when done (CRITICAL)
$VASTAI destroy instance $ID
$VASTAI show instances  # verify empty
```

## Architecture Mapping

| GPU | SM | `-arch` flag | Notes |
|-----|-----|-------------|-------|
| RTX 3090 | 8.6 | `sm_86` | 82 SMs, 24 GB GDDR6X |
| Jetson Orin | 8.7 | `sm_87` | 8 SMs, 8 GB unified |
| RTX 4090 | 8.9 | `sm_89` | 128 SMs, 24 GB GDDR6X |
| A100 | 8.0 | `sm_80` | 108 SMs, 40/80 GB HBM2e |

## Deploy Workflow

### 1. Copy code to instance

```bash
SSH_CMD="ssh -o StrictHostKeyChecking=accept-new -p $PORT root@$HOST"
rsync -avz --delete \
    --exclude 'build/' --exclude '.git' \
    -e "ssh -o StrictHostKeyChecking=accept-new -p $PORT" \
    tile_cuda_multi_kernel/ \
    root@$HOST:/root/tile_cuda_multi_kernel/

# Also copy C++ reference for cross-validation
rsync -avz --delete \
    --exclude 'build/' --exclude '.git' \
    -e "ssh -o StrictHostKeyChecking=accept-new -p $PORT" \
    tile-cpp/ \
    root@$HOST:/root/tile-cpp/
```

### 2. Build on instance

The Makefile must be patched for target GPU. On the instance:

```bash
# Change sm_87 -> sm_86 (or sm_89 for 4090)
cd /root/tile_cuda_multi_kernel
sed -i 's/sm_87/sm_86/g' Makefile
make clean && make -j$(nproc)
```

### 3. Run

```bash
# ALWAYS use tmux for anything > 5 seconds
tmux new -s bench

# Smoke test (2 tiles)
./tile_kernel_multi test

# Benchmark (2000 tiles — matches Jetson baseline)
./tile_kernel_multi 2000

# Profile run
./tile_kernel_multi 2000 2>&1 | tee /root/bench-3090.txt
```

### 4. Copy results back

```bash
scp -P $PORT root@$HOST:/root/bench-3090.txt ./results/
```

### 5. DESTROY instance

```bash
$VASTAI destroy instance $ID
$VASTAI show instances  # must show empty
```

## Lessons Learned (Prior Campaigns)

### SSH Port Pinning
Pin the SSH endpoint once at session start. The vast.ai API can return changed bindings mid-run. Cache the port; never re-resolve. (See CLAUDE.md in repo root.)

### Disk Space
vast.ai instances often have tight disk. A prior 3090 campaign (March 2026, ID 33123347) died because a 8.4 GB prime file consumed all 20 GB disk. For tile-processing this is not an issue (binary + source < 50 MB), but request >= 20 GB disk to be safe.

### Instance Images
- `pytorch/pytorch:2.5.1-cuda12.4-cudnn9-devel` — includes nvcc, cuDNN headers, nsys. Reliable.
- Ensure CUDA version >= 12.0. The multi-kernel code uses C++17 and separate compilation.

### Cost Tracking
```bash
# Check accumulated cost
$VASTAI show instances --raw | python3 -c "import sys,json; d=json.load(sys.stdin); [print(f'ID: {i[\"id\"]}, cost so far: \${i.get(\"total_dph_cost\",0):.4f}') for i in d]"
```

### tmux Always
Every long-running command on vast.ai MUST run in tmux. SSH drops are common. If the SSH session dies, tmux keeps the process alive. Non-negotiable.

### Budget Awareness
Track cost before and after. 3090 instances run ~$0.13-0.20/hr. A 1-hour benchmark session costs < $0.20. Always destroy immediately after work is done.

## Register Cap Considerations by GPU

The multi-kernel architecture uses per-kernel `--maxrregcount` caps. These were tuned for Jetson Orin (8 SMs, 48 KB shared mem/SM, 64K regs/SM). On 3090 (82 SMs, 48 KB shared mem/SM, 64K regs/SM but wider execution), the same caps are a safe starting point. Tuning may unlock further gains.

| Kernel | Jetson Cap | Jetson Actual Regs | Notes |
|--------|-----------|-------------------|-------|
| K1 Sieve | 40 | 30 | Headroom for Barrett |
| K2 MR | uncapped | 46 | Montgomery ILP-critical |
| K3 Compact | 32 | 21 | Maximize occupancy |
| K4 UF | 40 | 34 | Balance atomicCAS hiding |
| K5 FaceEncode | 40 | 40 | Complex control flow |

## Profiling

### Built-in per-kernel timing
The multi-kernel binary reports per-kernel timing via CUDA events when run in benchmark mode (`./tile_kernel_multi 2000`). This is the primary profiling mechanism.

### nsys (if available)
```bash
nsys profile -o /root/profile-3090 ./tile_kernel_multi 2000
# Copy .nsys-rep back to Mac for GUI analysis
scp -P $PORT root@$HOST:/root/profile-3090.nsys-rep ./results/
```

### ncu (if available)
```bash
ncu --set full -o /root/ncu-3090 ./tile_kernel_multi 2000
```

## Correctness Verification

The C++ reference implementation (`tile-cpp/`) is the ground truth. Both the CUDA multi-kernel and C++ implementations must produce byte-identical TileOp output for the same tile coordinates.

Canonical smoke tiles:
- `(600000000, 600000000)` — prime_count=1978
- `(699999744, 400000000)` — prime_count=2057

Compare the `tileop_hex` field from both outputs. Must be identical.
