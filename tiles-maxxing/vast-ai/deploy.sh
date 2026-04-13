#!/usr/bin/env bash
#
# deploy.sh — Deploy tile_cuda_multi_kernel + tile-cpp + tiles-compositor
#              to a vast.ai instance.
#
# Usage:
#   ./vast-ai/deploy.sh <port> <host> [sm_arch] [K_SQ]
#
# Example:
#   ./vast-ai/deploy.sh 12345 ssh4.vast.ai sm_86 36
#
# What it does:
#   1. rsync multi-kernel source + C++ reference to remote
#   2. Patch Makefile for target arch (default: sm_86 for 3090)
#   3. Build CUDA kernel, tile-cpp (libtile.a), tiles-compositor on remote
#   4. Run smoke test (2 tiles) and print output
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TILES_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PORT="${1:?Usage: $0 <port> <host> [sm_arch] [K_SQ]}"
HOST="${2:?Usage: $0 <port> <host> [sm_arch] [K_SQ]}"
SM_ARCH="${3:-sm_86}"
K_SQ="${4:-40}"

SSH_OPTS="-o StrictHostKeyChecking=accept-new"
SSH_CMD="ssh $SSH_OPTS -p $PORT"
TARGET="root@$HOST"

echo "=== tiles-maxxing Deploy ==="
echo "  Target: $TARGET:$PORT"
echo "  Arch: $SM_ARCH"
echo "  K_SQ: $K_SQ"
echo ""

# Step 1: rsync
echo "--- Syncing code ---"
rsync -avz --delete \
    --exclude 'build*/' --exclude '.git' --exclude '*.o' --exclude 'tile_kernel_multi' \
    -e "ssh $SSH_OPTS -p $PORT" \
    "$TILES_DIR/tile_cuda_multi_kernel/" \
    "$TARGET:/root/tile_cuda_multi_kernel/"

rsync -avz --delete \
    --exclude 'build*/' --exclude '.git' \
    -e "ssh $SSH_OPTS -p $PORT" \
    "$TILES_DIR/tile-cpp/" \
    "$TARGET:/root/tile-cpp/"

if [ -d "$TILES_DIR/tiles-compositor" ]; then
    rsync -avz --delete \
        --exclude 'build*/' --exclude '.git' \
        -e "ssh $SSH_OPTS -p $PORT" \
        "$TILES_DIR/tiles-compositor/" \
        "$TARGET:/root/tiles-compositor/"
fi

echo "  Sync complete."

# Step 2: Build CUDA kernel
echo ""
echo "--- Building CUDA kernel on remote (arch=$SM_ARCH, K_SQ=$K_SQ) ---"
$SSH_CMD "$TARGET" bash -s "$SM_ARCH" "$K_SQ" <<'REMOTE_BUILD'
set -euo pipefail
SM_ARCH="$1"
K_SQ="$2"

# Ensure nvcc is on PATH
for p in /usr/local/cuda/bin /usr/local/cuda-12/bin; do
    [ -x "$p/nvcc" ] && export PATH="$p:$PATH" && break
done

cd /root/tile_cuda_multi_kernel

# Patch arch
sed -i "s/sm_87/$SM_ARCH/g" Makefile

echo "[remote] Building CUDA kernel with arch=$SM_ARCH K_SQ=$K_SQ..."
make clean 2>/dev/null || true
make -j$(nproc) K_SQ="$K_SQ" 2>&1

if [ ! -x tile_kernel_multi ]; then
    echo "[remote] ERROR: CUDA kernel build failed"
    exit 1
fi
echo "[remote] CUDA kernel build successful."

echo ""
echo "[remote] GPU info:"
nvidia-smi --query-gpu=name,compute_cap,memory.total,driver_version --format=csv,noheader

echo ""
echo "[remote] nvcc version:"
nvcc --version | tail -1
REMOTE_BUILD

# Step 3: Build tile-cpp (libtile.a)
echo ""
echo "--- Building tile-cpp on remote (K_SQ=$K_SQ) ---"
$SSH_CMD "$TARGET" bash -s "$K_SQ" <<'REMOTE_TILECPP'
set -euo pipefail
K_SQ="$1"
BUILD_DIR="/root/tile-cpp/build-k${K_SQ}"

cd /root/tile-cpp
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DCMAKE_BUILD_TYPE=Release -DK_SQ="$K_SQ"
make -j$(nproc) tile 2>&1

if [ ! -f libtile.a ]; then
    echo "[remote] ERROR: tile-cpp build failed"
    exit 1
fi
echo "[remote] tile-cpp build successful: $(ls -lh libtile.a | awk '{print $5}')"
REMOTE_TILECPP

# Step 4: Build tiles-compositor (if present)
if [ -d "$TILES_DIR/tiles-compositor" ]; then
    echo ""
    echo "--- Building tiles-compositor on remote (K_SQ=$K_SQ) ---"
    $SSH_CMD "$TARGET" bash -s "$K_SQ" <<'REMOTE_COMPOSITOR'
set -euo pipefail
K_SQ="$1"

if [ ! -d /root/tiles-compositor ]; then
    echo "[remote] tiles-compositor not found, skipping."
    exit 0
fi

BUILD_DIR="/root/tiles-compositor/build-k${K_SQ}"
cd /root/tiles-compositor
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DCMAKE_BUILD_TYPE=Release -DK_SQ="$K_SQ"
make -j$(nproc) 2>&1

echo "[remote] tiles-compositor build successful."
REMOTE_COMPOSITOR
fi

# Step 5: Smoke test
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
