#!/usr/bin/env bash
#
# a100-mr-benchmark.sh -- Benchmark MR kernel vs Sieve on a vast.ai A100.
#
# Compares throughput of the two CUDA modes at three norm scales:
#   - 10^9  (near origin, dense primes)
#   - 10^15 (sqrt(36) frontier, moderate density)
#   - 10^18 (sqrt(40) feasibility, sparse primes)
#
# Each test runs a fixed 10^9 norm window at the target offset.
#
# Prerequisites:
#   - Deploy first with: ./deploy/a100-deploy.sh <ssh-connection-string>
#   - Instance must have CUDA binary at /workspace/gaussian-moat-cuda/build-a100/gm_cuda_primes
#
# Usage:
#   ./deploy/a100-mr-benchmark.sh <ssh-connection-string>
#   ./deploy/a100-mr-benchmark.sh root@203.0.113.42 --port 12345
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Parse SSH connection (same pattern as a100-deploy.sh)
# ---------------------------------------------------------------------------
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <ssh-connection-string-or-user@host> [--port PORT]" >&2
    exit 1
fi

SSH_PORT=22
SSH_TARGET=""

if [[ "$1" == ssh* ]]; then
    eval set -- $1
    shift
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -p) SSH_PORT="$2"; shift 2 ;;
            -i) shift 2 ;;
            -o) shift 2 ;;
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
REMOTE_DIR="/workspace/gaussian-moat-cuda"

echo "=== MR vs Sieve Benchmark ==="
echo "  Target: $SSH_TARGET:$SSH_PORT"
echo ""

# First, re-sync code to pick up any changes
echo "--- Syncing code ---"
rsync -avz --delete \
    --exclude '.git' \
    --exclude 'build/' \
    --exclude 'solver/target/' \
    --exclude 'tmp/' \
    --exclude 'output/' \
    -e "ssh -o StrictHostKeyChecking=accept-new -p $SSH_PORT" \
    "$(cd "$SCRIPT_DIR/.." && pwd)/" \
    "$SSH_TARGET:$REMOTE_DIR/" 2>&1 | tail -3

echo ""
echo "--- Rebuilding CUDA binary ---"
$SSH_CMD "$SSH_TARGET" bash -s "$REMOTE_DIR" <<'REMOTE_REBUILD'
set -euo pipefail
REMOTE_DIR="$1"

for cuda_path in /usr/local/cuda/bin /usr/local/cuda-12/bin; do
    [ -d "$cuda_path" ] && export PATH="$cuda_path:$PATH"
done

cd "$REMOTE_DIR/build-a100"
cmake .. -DTARGET_DEVICE=a100 2>&1 | tail -2
make -j$(nproc) gm_cuda_primes 2>&1 | tail -5

[ -x gm_cuda_primes ] && echo "Build OK" || { echo "Build FAILED"; exit 1; }
REMOTE_REBUILD

echo ""
echo "--- Running benchmarks ---"
$SSH_CMD "$SSH_TARGET" bash -s "$REMOTE_DIR" <<'REMOTE_BENCH'
set -euo pipefail
REMOTE_DIR="$1"

for cuda_path in /usr/local/cuda/bin /usr/local/cuda-12/bin; do
    [ -d "$cuda_path" ] && export PATH="$cuda_path:$PATH"
done

BIN="$REMOTE_DIR/build-a100/gm_cuda_primes"
OUTDIR="$REMOTE_DIR/tmp/mr-benchmark"
mkdir -p "$OUTDIR"

# Benchmark parameters:
#   - WINDOW: 10^9 norm range at each scale
#   - Three scales: 10^9, 10^15, 10^18
WINDOW=1000000000

declare -a SCALE_NAMES=("1e9" "1e15" "1e18")
declare -a SCALE_LO=(0 1000000000000000 1000000000000000000)

echo ""
echo "=========================================="
echo " GPU Info"
echo "=========================================="
nvidia-smi --query-gpu=name,compute_cap,memory.total,memory.free --format=csv,noheader 2>/dev/null || echo "(nvidia-smi not available)"
echo ""

# Results arrays
declare -a MR_PRIMES_SEC=()
declare -a MR_CAND_SEC=()
declare -a MR_WALL=()
declare -a SIEVE_PRIMES_SEC=()
declare -a SIEVE_CAND_SEC=()
declare -a SIEVE_WALL=()

for i in 0 1 2; do
    NORM_LO=${SCALE_LO[$i]}
    NORM_HI=$((NORM_LO + WINDOW))
    NAME=${SCALE_NAMES[$i]}

    echo "=========================================="
    echo " Scale: $NAME  [norm_lo=$NORM_LO, window=$WINDOW]"
    echo "=========================================="

    # --- MR mode ---
    echo ""
    echo "  [MR mode] Running..."
    MR_OUT="$OUTDIR/mr_${NAME}.txt"
    MR_LOG="$OUTDIR/mr_${NAME}.log"

    "$BIN" --norm-lo "$NORM_LO" --norm-hi "$NORM_HI" \
           --output "$MR_OUT" --mode mr \
           --batch-size 10000000 --block-size 256 \
           2>"$MR_LOG" || true

    # Extract results from stderr
    mr_primes=$(grep "Primes found:" "$MR_LOG" | awk '{print $NF}' || echo "0")
    mr_wall=$(grep "Wall time:" "$MR_LOG" | awk '{print $NF}' | tr -d 's' || echo "0")
    mr_primes_s=$(grep "Primes/sec:" "$MR_LOG" | awk '{print $NF}' || echo "0")
    mr_cand_s=$(grep "Candidates/sec:" "$MR_LOG" | awk '{print $NF}' || echo "0")

    MR_PRIMES_SEC+=("$mr_primes_s")
    MR_CAND_SEC+=("$mr_cand_s")
    MR_WALL+=("$mr_wall")

    echo "  [MR] Primes: $mr_primes, Wall: ${mr_wall}s, Primes/s: $mr_primes_s, Candidates/s: $mr_cand_s"

    # --- Sieve mode ---
    echo ""
    echo "  [Sieve mode] Running..."
    SIEVE_OUT="$OUTDIR/sieve_${NAME}.txt"
    SIEVE_LOG="$OUTDIR/sieve_${NAME}.log"

    "$BIN" --norm-lo "$NORM_LO" --norm-hi "$NORM_HI" \
           --output "$SIEVE_OUT" --mode sieve \
           2>"$SIEVE_LOG" || true

    sieve_primes=$(grep "Primes found:" "$SIEVE_LOG" | awk '{print $NF}' || echo "0")
    sieve_wall=$(grep "Wall time:" "$SIEVE_LOG" | awk '{print $NF}' | tr -d 's' || echo "0")
    sieve_primes_s=$(grep "Primes/sec:" "$SIEVE_LOG" | awk '{print $NF}' || echo "0")
    sieve_cand_s=$(grep "Candidates/sec:" "$SIEVE_LOG" | awk '{print $NF}' || echo "0")

    SIEVE_PRIMES_SEC+=("$sieve_primes_s")
    SIEVE_CAND_SEC+=("$sieve_cand_s")
    SIEVE_WALL+=("$sieve_wall")

    echo "  [Sieve] Primes: $sieve_primes, Wall: ${sieve_wall}s, Primes/s: $sieve_primes_s, Candidates/s: $sieve_cand_s"

    # Clean up output files (keep logs)
    rm -f "$MR_OUT" "$SIEVE_OUT"

    echo ""
done

# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------
echo ""
echo "=========================================="
echo " RESULTS SUMMARY: MR vs Sieve (A100)"
echo "=========================================="
echo ""
printf "%-8s | %-15s %-15s %-10s | %-15s %-15s %-10s | %-8s\n" \
       "Scale" "MR Primes/s" "MR Cand/s" "MR Wall" \
       "Sieve Primes/s" "Sieve Cand/s" "Sieve Wall" "Winner"
printf -- "%-8s-+-%-15s-%-15s-%-10s-+-%-15s-%-15s-%-10s-+-%-8s\n" \
       "--------" "---------------" "---------------" "----------" \
       "---------------" "---------------" "----------" "--------"

for i in 0 1 2; do
    NAME=${SCALE_NAMES[$i]}
    mr_p=${MR_PRIMES_SEC[$i]:-0}
    mr_c=${MR_CAND_SEC[$i]:-0}
    mr_w=${MR_WALL[$i]:-0}
    sv_p=${SIEVE_PRIMES_SEC[$i]:-0}
    sv_c=${SIEVE_CAND_SEC[$i]:-0}
    sv_w=${SIEVE_WALL[$i]:-0}

    # Determine winner by primes/sec (higher is better)
    mr_val=$(echo "$mr_p" | tr -d ',' | awk '{printf "%.0f", $1}')
    sv_val=$(echo "$sv_p" | tr -d ',' | awk '{printf "%.0f", $1}')

    if [ "$mr_val" -gt "$sv_val" ] 2>/dev/null; then
        winner="MR"
    elif [ "$sv_val" -gt "$mr_val" ] 2>/dev/null; then
        winner="Sieve"
    else
        winner="Tie"
    fi

    printf "%-8s | %-15s %-15s %-10s | %-15s %-15s %-10s | %-8s\n" \
           "$NAME" "$mr_p" "$mr_c" "${mr_w}s" \
           "$sv_p" "$sv_c" "${sv_w}s" "$winner"
done

echo ""
echo "Benchmark logs saved to: $OUTDIR/"
echo ""
echo "=== Benchmark complete ==="
REMOTE_BENCH
