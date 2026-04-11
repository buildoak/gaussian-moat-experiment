#!/usr/bin/env bash
#
# deploy.sh — Deploy tile_cuda_multi_kernel to a vast.ai instance.
#
# Usage:
#   ./vast-ai/deploy.sh <port> <host> [sm_arch]
#
# Example:
#   ./vast-ai/deploy.sh 12345 ssh4.vast.ai sm_86
#
# What it does:
#   1. rsync multi-kernel source + C++ reference to remote
#   2. Patch Makefile for target arch (default: sm_86 for 3090)
#   3. Build on remote
#   4. Run smoke test (2 tiles) and print output
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TILES_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PORT="${1:?Usage: $0 <port> <host> [sm_arch]}"
HOST="${2:?Usage: $0 <port> <host> [sm_arch]}"
SM_ARCH="${3:-sm_86}"

SSH_OPTS="-o StrictHostKeyChecking=accept-new"
SSH_CMD="ssh $SSH_OPTS -p $PORT"
TARGET="root@$HOST"

echo "=== tiles-maxxing Deploy ==="
echo "  Target: $TARGET:$PORT"
echo "  Arch: $SM_ARCH"
echo ""

# Step 1: rsync
echo "--- Syncing code ---"
rsync -avz --delete \
    --exclude 'build/' --exclude '.git' --exclude '*.o' --exclude 'tile_kernel_multi' \
    -e "ssh $SSH_OPTS -p $PORT" \
    "$TILES_DIR/tile_cuda_multi_kernel/" \
    "$TARGET:/root/tile_cuda_multi_kernel/"

rsync -avz --delete \
    --exclude 'build/' --exclude '.git' \
    -e "ssh $SSH_OPTS -p $PORT" \
    "$TILES_DIR/tile-cpp/" \
    "$TARGET:/root/tile-cpp/"

echo "  Sync complete."

# Step 2: Build
echo ""
echo "--- Building on remote (arch=$SM_ARCH) ---"
$SSH_CMD "$TARGET" bash -s "$SM_ARCH" <<'REMOTE_BUILD'
set -euo pipefail
SM_ARCH="$1"

# Ensure nvcc is on PATH
for p in /usr/local/cuda/bin /usr/local/cuda-12/bin; do
    [ -x "$p/nvcc" ] && export PATH="$p:$PATH" && break
done

cd /root/tile_cuda_multi_kernel

# Patch arch
sed -i "s/sm_87/$SM_ARCH/g" Makefile

echo "[remote] Building with arch=$SM_ARCH..."
make clean 2>/dev/null || true
make -j$(nproc) 2>&1

if [ ! -x tile_kernel_multi ]; then
    echo "[remote] ERROR: build failed"
    exit 1
fi
echo "[remote] Build successful."

echo ""
echo "[remote] GPU info:"
nvidia-smi --query-gpu=name,compute_cap,memory.total,driver_version --format=csv,noheader

echo ""
echo "[remote] nvcc version:"
nvcc --version | tail -1
REMOTE_BUILD

# Step 3: Smoke test
echo ""
echo "--- Smoke test (2 tiles) ---"
$SSH_CMD "$TARGET" bash <<'REMOTE_SMOKE'
cd /root/tile_cuda_multi_kernel
./tile_kernel_multi test
REMOTE_SMOKE

echo ""
echo "=== Deploy complete ==="
echo "  SSH: ssh -p $PORT $TARGET"
echo "  Benchmark: ssh -p $PORT $TARGET 'cd /root/tile_cuda_multi_kernel && tmux new -s bench \"./tile_kernel_multi 2000 2>&1 | tee /root/bench-3090.txt\"'"
