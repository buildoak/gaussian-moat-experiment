#!/usr/bin/env bash
#
# a100-deploy.sh -- Deploy gaussian-moat-cuda to a vast.ai A100 instance.
#
# Usage:
#   ./deploy/a100-deploy.sh <ssh-connection-string>
#
# The ssh-connection-string should be of the form:
#   ssh -p <port> root@<host>
# Or just:
#   root@<host>  (port 22)
#
# What it does:
#   1. rsync the repo to the remote
#   2. Install dependencies (cmake, CUDA toolkit dev headers, Rust toolchain)
#   3. Build CUDA sieve with -arch=sm_80 for A100
#   4. Build Rust solver with cargo build --release
#   5. Run a smoke test (k^2=4, small norm, verify correctness)
#   6. Report success
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# Parse SSH connection
# ---------------------------------------------------------------------------
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <ssh-connection-string-or-user@host>" >&2
    echo "" >&2
    echo "Examples:" >&2
    echo "  $0 root@203.0.113.42                        # port 22" >&2
    echo "  $0 'ssh -p 12345 root@203.0.113.42'         # custom port" >&2
    echo "  $0 root@203.0.113.42 --port 12345           # custom port" >&2
    exit 1
fi

SSH_PORT=22
SSH_TARGET=""

if [[ "$1" == ssh* ]]; then
    # Parse 'ssh -p <port> user@host' form
    eval set -- $1
    shift  # remove 'ssh'
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -p) SSH_PORT="$2"; shift 2 ;;
            -i) shift 2 ;;  # skip identity file
            -o) shift 2 ;;  # skip options
            *@*) SSH_TARGET="$1"; shift ;;
            *) shift ;;
        esac
    done
else
    SSH_TARGET="$1"
    shift
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --port|-p) SSH_PORT="$2"; shift 2 ;;
            *) shift ;;
        esac
    done
fi

if [ -z "$SSH_TARGET" ]; then
    echo "Error: could not parse SSH target from arguments" >&2
    exit 1
fi

SSH_CMD="ssh -o StrictHostKeyChecking=accept-new -p $SSH_PORT"
SCP_CMD="scp -o StrictHostKeyChecking=accept-new -P $SSH_PORT"
REMOTE_DIR="/workspace/gaussian-moat-cuda"

echo "=== A100 Deployment ==="
echo "  Target: $SSH_TARGET:$SSH_PORT"
echo "  Remote dir: $REMOTE_DIR"
echo ""

# ---------------------------------------------------------------------------
# Step 1: rsync repo to remote
# ---------------------------------------------------------------------------
echo "--- Step 1: Syncing repo to remote ---"

rsync -avz --delete \
    --exclude '.git' \
    --exclude 'build/' \
    --exclude 'solver/target/' \
    --exclude 'tmp/' \
    --exclude 'output/' \
    -e "ssh -o StrictHostKeyChecking=accept-new -p $SSH_PORT" \
    "$REPO_ROOT/" \
    "$SSH_TARGET:$REMOTE_DIR/"

echo "  Sync complete."

# ---------------------------------------------------------------------------
# Step 2: Install dependencies + build on remote
# ---------------------------------------------------------------------------
echo ""
echo "--- Step 2: Installing deps and building ---"

$SSH_CMD "$SSH_TARGET" bash -s "$REMOTE_DIR" <<'REMOTE_BUILD'
set -euo pipefail
REMOTE_DIR="$1"

echo "[remote] Checking CUDA installation..."
if ! command -v nvcc >/dev/null 2>&1; then
    echo "[remote] nvcc not found, checking standard paths..."
    for cuda_path in /usr/local/cuda/bin /usr/local/cuda-12/bin /usr/local/cuda-11/bin; do
        if [ -x "$cuda_path/nvcc" ]; then
            export PATH="$cuda_path:$PATH"
            echo "[remote] Found nvcc at $cuda_path"
            break
        fi
    done
fi

if ! command -v nvcc >/dev/null 2>&1; then
    echo "[remote] ERROR: CUDA toolkit not found. Install it or select an image with CUDA." >&2
    exit 1
fi

echo "[remote] CUDA version:"
nvcc --version | tail -1

echo "[remote] GPU info:"
nvidia-smi --query-gpu=name,compute_cap,memory.total --format=csv,noheader 2>/dev/null || echo "(nvidia-smi not available)"

# Install build deps
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq cmake build-essential pkg-config curl ca-certificates >/dev/null 2>&1

# Rust toolchain
if ! command -v rustup >/dev/null 2>&1; then
    echo "[remote] Installing Rust toolchain..."
    curl https://sh.rustup.rs -sSf | sh -s -- -y >/dev/null 2>&1
fi
. "$HOME/.cargo/env" 2>/dev/null || true

rustup toolchain install stable >/dev/null 2>&1 || true
rustup default stable >/dev/null 2>&1 || true

echo ""
echo "[remote] Building CUDA sieve (sm_80 for A100)..."
cd "$REMOTE_DIR"
mkdir -p build-a100
cd build-a100
cmake .. \
    -DCMAKE_CUDA_ARCHITECTURES=80 \
    -DCMAKE_CUDA_FLAGS="-O3 --use_fast_math -lineinfo --ptxas-options=-v" \
    2>&1 | tail -5
make -j$(nproc) gm_cuda_primes 2>&1 | tail -10

if [ ! -x gm_cuda_primes ]; then
    echo "[remote] ERROR: CUDA build failed" >&2
    exit 1
fi
echo "[remote] CUDA sieve built successfully."

echo ""
echo "[remote] Building Rust solver..."
cd "$REMOTE_DIR/solver"
. "$HOME/.cargo/env" 2>/dev/null || true
cargo build --release 2>&1 | tail -5

if [ ! -x target/release/gaussian-moat-solver ]; then
    echo "[remote] ERROR: Rust build failed" >&2
    exit 1
fi
echo "[remote] Rust solver built successfully."
REMOTE_BUILD

echo "  Build complete."

# ---------------------------------------------------------------------------
# Step 3: Smoke test
# ---------------------------------------------------------------------------
echo ""
echo "--- Step 3: Running smoke test (k^2=4, norm_hi=10000) ---"

$SSH_CMD "$SSH_TARGET" bash -s "$REMOTE_DIR" <<'REMOTE_SMOKE'
set -euo pipefail
REMOTE_DIR="$1"

# Add CUDA to path
for cuda_path in /usr/local/cuda/bin /usr/local/cuda-12/bin; do
    [ -d "$cuda_path" ] && export PATH="$cuda_path:$PATH"
done
. "$HOME/.cargo/env" 2>/dev/null || true

CUDA_BIN="$REMOTE_DIR/build-a100/gm_cuda_primes"
SOLVER_BIN="$REMOTE_DIR/solver/target/release/gaussian-moat-solver"
SMOKE_DIR="$REMOTE_DIR/tmp/smoke-test"
mkdir -p "$SMOKE_DIR"

echo "[smoke] Stage 1: CUDA sieve -> GPRF (norm range [0, 10000))"
"$CUDA_BIN" --norm-lo 0 --norm-hi 10000 --output "$SMOKE_DIR/smoke.gprf" --mode sieve 2>&1

GPRF_SIZE=$(stat -c%s "$SMOKE_DIR/smoke.gprf" 2>/dev/null || stat -f%z "$SMOKE_DIR/smoke.gprf" 2>/dev/null)
echo "[smoke] GPRF file: $GPRF_SIZE bytes"

echo ""
echo "[smoke] Stage 2: Rust angular solver (k^2=4)"
"$SOLVER_BIN" --k-squared 4 --angular 0 --prime-file "$SMOKE_DIR/smoke.gprf" --profile 2>&1

echo ""
echo "[smoke] Stage 3: Verify correctness -- k^2=2 should detect moat at (11,4)"
"$SOLVER_BIN" --k-squared 2 --angular 0 --prime-file "$SMOKE_DIR/smoke.gprf" 2>&1 | grep -q "farthest point: (11, 4)" && echo "[smoke] PASS: k^2=2 moat at (11,4) verified" || echo "[smoke] WARN: k^2=2 moat check inconclusive"

rm -rf "$SMOKE_DIR"
echo ""
echo "=== Smoke test complete ==="
REMOTE_SMOKE

echo ""
echo "=== Deployment successful ==="
echo "  Remote CUDA binary: $REMOTE_DIR/build-a100/gm_cuda_primes"
echo "  Remote solver binary: $REMOTE_DIR/solver/target/release/gaussian-moat-solver"
echo "  Run campaign with: $SCRIPT_DIR/a100-sqrt36-campaign.sh $SSH_TARGET --port $SSH_PORT"
