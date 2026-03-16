#!/usr/bin/env bash
#
# 4090-connector-experiments.sh -- Connector performance benchmark matrix on RTX 4090.
#
# Runs the full experiment suite to diagnose the 50x connector degradation
# observed between Jetson Orin (1.35M/sec) and A100 (28.7K/sec).
#
# Experiments:
#   1. CUDA sieve at three scales (10^9, 10^15, 10^18) — apples-to-apples
#   2. Connector with WORKING Jetson config (angular=0/auto, k^2=36, GPRF file)
#   3. Connector with BROKEN A100 config (angular=0, k^2=36, no start-distance, ~128 wedges)
#   4. Wedge sweep: angular={1,4,8,16,32} — throughput AND RSS for each
#   5. Upper-bound mode test with windowed GPRF
#
# This script also runs on Jetson. Set DEVICE=jetson to use jetson paths.
#
# Usage:
#   ./deploy/4090-connector-experiments.sh <user@host> [--port PORT] [--device 4090|jetson]
#
# Prerequisites:
#   - Deploy first with: ./deploy/a100-deploy.sh <ssh-connection-string>
#     (deploy script works for any GPU — it builds from source)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
SSH_PORT=22
SSH_TARGET=""
DEVICE="4090"

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <user@host> [--port PORT] [--device 4090|jetson]" >&2
    exit 1
fi

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
    SSH_TARGET="$1"; shift
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --port|-p)   SSH_PORT="$2"; shift 2 ;;
            --device)    DEVICE="$2"; shift 2 ;;
            *)           shift ;;
        esac
    done
fi

if [ -z "$SSH_TARGET" ]; then
    echo "Error: could not parse SSH target" >&2
    exit 1
fi

SSH_CMD="ssh -o StrictHostKeyChecking=accept-new -p $SSH_PORT"
REMOTE_DIR="/workspace/gaussian-moat-cuda"

echo "=== 4090 Connector Experiment Suite ==="
echo "  Target: $SSH_TARGET:$SSH_PORT"
echo "  Device: $DEVICE"
echo ""

# ---------------------------------------------------------------------------
# Sync code
# ---------------------------------------------------------------------------
echo "--- Syncing code ---"
rsync -avz --delete \
    --exclude '.git' \
    --exclude 'build*' \
    --exclude 'solver/target/' \
    --exclude 'tmp/' \
    --exclude 'output/' \
    --exclude 'runs/' \
    -e "ssh -o StrictHostKeyChecking=accept-new -p $SSH_PORT" \
    "$(cd "$SCRIPT_DIR/.." && pwd)/" \
    "$SSH_TARGET:$REMOTE_DIR/" 2>&1 | tail -3
echo ""

# ---------------------------------------------------------------------------
# Build + run experiments on remote
# ---------------------------------------------------------------------------
echo "--- Building and running experiments ---"
$SSH_CMD "$SSH_TARGET" bash -s "$REMOTE_DIR" "$DEVICE" <<'REMOTE_EXPERIMENTS'
set -euo pipefail
REMOTE_DIR="$1"
DEVICE="$2"

for cuda_path in /usr/local/cuda/bin /usr/local/cuda-12/bin; do
    [ -d "$cuda_path" ] && export PATH="$cuda_path:$PATH"
done
. "$HOME/.cargo/env" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Build CUDA sieve
# ---------------------------------------------------------------------------
echo "=== Building CUDA sieve ==="
cd "$REMOTE_DIR"

if [ "$DEVICE" = "jetson" ]; then
    BUILD_DIR="build"
    CMAKE_ARGS=""
else
    BUILD_DIR="build-${DEVICE}"
    # 4090 = SM 8.9, A100 = SM 8.0. Auto-detect if possible.
    CMAKE_ARGS="-DTARGET_DEVICE=${DEVICE}"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. $CMAKE_ARGS 2>&1 | tail -3
make -j$(nproc) gm_cuda_primes 2>&1 | tail -5
[ -x gm_cuda_primes ] && echo "CUDA build OK" || { echo "CUDA build FAILED"; exit 1; }
CUDA_BIN="$REMOTE_DIR/$BUILD_DIR/gm_cuda_primes"

# Build Rust solver
echo ""
echo "=== Building Rust solver ==="
cd "$REMOTE_DIR/solver"
cargo build --release 2>&1 | tail -5
SOLVER_BIN="$REMOTE_DIR/solver/target/release/gaussian-moat-solver"
[ -x "$SOLVER_BIN" ] && echo "Solver build OK" || { echo "Solver build FAILED"; exit 1; }

# ---------------------------------------------------------------------------
# Setup output
# ---------------------------------------------------------------------------
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUTDIR="$REMOTE_DIR/runs/connector-bench-${DEVICE}-${TIMESTAMP}"
mkdir -p "$OUTDIR"

LOG="$OUTDIR/experiment.log"
RESULTS="$OUTDIR/results.jsonl"

log() {
    echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG"
}

# GPU and CPU info
log "=== Hardware Info ==="
nvidia-smi --query-gpu=name,compute_cap,memory.total,memory.free --format=csv,noheader 2>/dev/null | tee -a "$LOG" || true
nproc | xargs -I{} echo "CPUs: {}" | tee -a "$LOG"
free -h 2>/dev/null | head -2 | tee -a "$LOG" || true
log ""

# =========================================================================
# EXPERIMENT 1: CUDA Sieve at three scales (apples-to-apples with A100)
# =========================================================================
log "==========================================="
log " EXPERIMENT 1: CUDA Sieve — 3 scales"
log "==========================================="

WINDOW=1000000000
declare -a SCALE_NAMES=("1e9" "1e15" "1e18")
declare -a SCALE_LO=(0 1000000000000000 1000000000000000000)

for i in 0 1 2; do
    NORM_LO=${SCALE_LO[$i]}
    NORM_HI=$((NORM_LO + WINDOW))
    NAME=${SCALE_NAMES[$i]}

    log "  Scale $NAME: sieving [$NORM_LO, $NORM_HI)..."

    SIEVE_OUT="$OUTDIR/sieve_${NAME}.gprf"
    SIEVE_LOG="$OUTDIR/sieve_${NAME}.log"

    START_NS=$(date +%s%N)
    "$CUDA_BIN" --norm-lo "$NORM_LO" --norm-hi "$NORM_HI" \
                --output "$SIEVE_OUT" --mode sieve \
                2>"$SIEVE_LOG" || true
    END_NS=$(date +%s%N)
    WALL_MS=$(( (END_NS - START_NS) / 1000000 ))

    PRIMES=$(grep "Primes found:" "$SIEVE_LOG" 2>/dev/null | awk '{print $NF}' || echo "0")
    log "  Scale $NAME: ${PRIMES} primes, ${WALL_MS}ms"

    echo "{\"exp\":\"sieve\",\"scale\":\"$NAME\",\"primes\":$PRIMES,\"wall_ms\":$WALL_MS,\"device\":\"$DEVICE\"}" >> "$RESULTS"

    # Keep the 1e9 GPRF for connector experiments
    if [ "$NAME" != "1e9" ]; then
        rm -f "$SIEVE_OUT"
    fi
done

# We need a near-origin GPRF for connector tests
GPRF_1E9="$OUTDIR/sieve_1e9.gprf"
if [ ! -f "$GPRF_1E9" ]; then
    log "ERROR: 1e9 GPRF not found, cannot run connector experiments"
    exit 1
fi

GPRF_SIZE=$(stat -c%s "$GPRF_1E9" 2>/dev/null || stat -f%z "$GPRF_1E9" 2>/dev/null)
GPRF_PRIMES=$(( (GPRF_SIZE - 64) / 16 ))
log ""
log "Using GPRF: $GPRF_1E9 ($GPRF_PRIMES primes, $GPRF_SIZE bytes)"
log ""

# =========================================================================
# EXPERIMENT 2: Connector — WORKING Jetson config
# =========================================================================
log "==========================================="
log " EXPERIMENT 2: Connector — Working config"
log "  (angular=0 auto, k^2=36, GPRF file)"
log "  This is what ran at 1.35-1.78M/sec on Jetson"
log "==========================================="

START_NS=$(date +%s%N)
"$SOLVER_BIN" \
    --k-squared 36 \
    --angular 0 \
    --prime-file "$GPRF_1E9" \
    --profile \
    2>&1 | tee "$OUTDIR/exp2_working_config.txt"
END_NS=$(date +%s%N)
WALL_MS=$(( (END_NS - START_NS) / 1000000 ))

RSS=$(grep "max_rss_bytes:" "$OUTDIR/exp2_working_config.txt" 2>/dev/null | awk '{print $NF}' || echo "0")
PPS=$(grep "primes_per_second:" "$OUTDIR/exp2_working_config.txt" 2>/dev/null | awk '{print $NF}' || echo "0")
WEDGES=$(grep "wedges used:" "$OUTDIR/exp2_working_config.txt" 2>/dev/null | awk '{print $NF}' || echo "0")
PRIMES=$(grep "primes processed:" "$OUTDIR/exp2_working_config.txt" 2>/dev/null | awk '{print $NF}' || echo "0")

log "  Working config: ${PPS} primes/sec, ${WEDGES} wedges, RSS=${RSS} bytes"
echo "{\"exp\":\"connector_working\",\"angular\":0,\"k_squared\":36,\"wedges\":$WEDGES,\"primes\":$PRIMES,\"wall_ms\":$WALL_MS,\"rss_bytes\":$RSS,\"primes_per_sec\":$PPS,\"device\":\"$DEVICE\"}" >> "$RESULTS"
log ""

# =========================================================================
# EXPERIMENT 3: Connector — Broken A100 config (simulated)
#   The A100 bug was: angular=0 -> effective_wedge_count = 4*cores = 128
#   On 4090 host with N cores, this would be 4*N. We force the old behavior.
#   Also: k^2=36, band 0 (no start-distance), no GPRF start_norm filter
# =========================================================================
log "==========================================="
log " EXPERIMENT 3: Connector — Broken config"
log "  (angular=128 forced, simulating A100 4x cores bug)"
log "==========================================="

# Force 128 wedges to simulate the A100 bug
START_NS=$(date +%s%N)
"$SOLVER_BIN" \
    --k-squared 36 \
    --angular 128 \
    --prime-file "$GPRF_1E9" \
    --profile \
    2>&1 | tee "$OUTDIR/exp3_broken_config.txt"
END_NS=$(date +%s%N)
WALL_MS=$(( (END_NS - START_NS) / 1000000 ))

RSS=$(grep "max_rss_bytes:" "$OUTDIR/exp3_broken_config.txt" 2>/dev/null | awk '{print $NF}' || echo "0")
PPS=$(grep "primes_per_second:" "$OUTDIR/exp3_broken_config.txt" 2>/dev/null | awk '{print $NF}' || echo "0")
PRIMES=$(grep "primes processed:" "$OUTDIR/exp3_broken_config.txt" 2>/dev/null | awk '{print $NF}' || echo "0")

log "  Broken config: ${PPS} primes/sec, 128 wedges, RSS=${RSS} bytes"
echo "{\"exp\":\"connector_broken\",\"angular\":128,\"k_squared\":36,\"wedges\":128,\"primes\":$PRIMES,\"wall_ms\":$WALL_MS,\"rss_bytes\":$RSS,\"primes_per_sec\":$PPS,\"device\":\"$DEVICE\"}" >> "$RESULTS"
log ""

# =========================================================================
# EXPERIMENT 4: Wedge sweep — throughput vs RSS
# =========================================================================
log "==========================================="
log " EXPERIMENT 4: Wedge Sweep"
log "  angular = {1, 4, 8, 16, 32, 64, 128}"
log "  k^2=36, near-origin GPRF (~25M primes)"
log "==========================================="

for WEDGE_COUNT in 1 4 8 16 32 64 128; do
    log "  Wedges=$WEDGE_COUNT ..."

    RESULT_FILE="$OUTDIR/exp4_wedges_${WEDGE_COUNT}.txt"

    START_NS=$(date +%s%N)
    timeout 600 "$SOLVER_BIN" \
        --k-squared 36 \
        --angular "$WEDGE_COUNT" \
        --prime-file "$GPRF_1E9" \
        --profile \
        2>&1 | tee "$RESULT_FILE" || true
    END_NS=$(date +%s%N)
    WALL_MS=$(( (END_NS - START_NS) / 1000000 ))

    RSS=$(grep "max_rss_bytes:" "$RESULT_FILE" 2>/dev/null | awk '{print $NF}' || echo "0")
    PPS=$(grep "primes_per_second:" "$RESULT_FILE" 2>/dev/null | awk '{print $NF}' || echo "0")
    PRIMES=$(grep "primes processed:" "$RESULT_FILE" 2>/dev/null | awk '{print $NF}' || echo "0")
    ELAPSED=$(grep "elapsed:" "$RESULT_FILE" 2>/dev/null | awk '{print $NF}' | tr -d 's' || echo "0")

    RSS_GB=$(echo "$RSS" | awk '{printf "%.2f", $1/1073741824}')
    log "  Wedges=$WEDGE_COUNT: ${PPS} primes/sec, RSS=${RSS_GB} GB, wall=${WALL_MS}ms"

    echo "{\"exp\":\"wedge_sweep\",\"angular\":$WEDGE_COUNT,\"k_squared\":36,\"primes\":$PRIMES,\"wall_ms\":$WALL_MS,\"rss_bytes\":$RSS,\"primes_per_sec\":$PPS,\"device\":\"$DEVICE\"}" >> "$RESULTS"
done
log ""

# =========================================================================
# EXPERIMENT 5: Upper-bound mode with small k^2 (correctness check)
# =========================================================================
log "==========================================="
log " EXPERIMENT 5: Upper-bound mode correctness"
log "  k^2=2, start-distance=8, GPRF file"
log "  Expected: farthest point (11, 4)"
log "==========================================="

# Generate a small GPRF for k^2=2
GPRF_SMALL="$OUTDIR/sieve_small.gprf"
"$CUDA_BIN" --norm-lo 0 --norm-hi 10000 --output "$GPRF_SMALL" --mode sieve 2>/dev/null || true

"$SOLVER_BIN" \
    --k-squared 2 \
    --angular 0 \
    --prime-file "$GPRF_SMALL" \
    --profile \
    2>&1 | tee "$OUTDIR/exp5_ub_k2_lb.txt"

"$SOLVER_BIN" \
    --k-squared 2 \
    --angular 0 \
    --start-distance 8 \
    --prime-file "$GPRF_SMALL" \
    --profile \
    2>&1 | tee "$OUTDIR/exp5_ub_k2_ub.txt"

FARTHEST_LB=$(grep "farthest point:" "$OUTDIR/exp5_ub_k2_lb.txt" 2>/dev/null || echo "unknown")
FARTHEST_UB=$(grep "farthest point:" "$OUTDIR/exp5_ub_k2_ub.txt" 2>/dev/null || echo "unknown")
log "  Lower-bound: $FARTHEST_LB"
log "  Upper-bound: $FARTHEST_UB"

rm -f "$GPRF_SMALL"
log ""

# =========================================================================
# Summary
# =========================================================================
log "==========================================="
log " EXPERIMENT SUMMARY"
log "==========================================="
log ""
log "Results file: $RESULTS"
log "Full log: $LOG"
log "Output dir: $OUTDIR"
log ""

# Print a compact summary table
log "Wedge sweep results:"
log "  Wedges | Primes/sec | RSS (GB) | Wall (s)"
log "  -------|-----------|----------|--------"
for WEDGE_COUNT in 1 4 8 16 32 64 128; do
    FILE="$OUTDIR/exp4_wedges_${WEDGE_COUNT}.txt"
    if [ -f "$FILE" ]; then
        PPS=$(grep "primes_per_second:" "$FILE" 2>/dev/null | awk '{print $NF}' || echo "0")
        RSS=$(grep "max_rss_bytes:" "$FILE" 2>/dev/null | awk '{print $NF}' || echo "0")
        ELAPSED=$(grep "elapsed:" "$FILE" 2>/dev/null | awk '{print $NF}' | tr -d 's' || echo "0")
        RSS_GB=$(echo "$RSS" | awk '{printf "%.2f", $1/1073741824}')
        printf "  %7d | %10s | %8s | %s\n" "$WEDGE_COUNT" "$PPS" "$RSS_GB" "$ELAPSED" | tee -a "$LOG"
    fi
done

log ""
log "=== Experiments complete ==="

REMOTE_EXPERIMENTS

echo ""
echo "=== Experiment suite dispatched ==="
echo "Fetch results with:"
echo "  rsync -avz -e 'ssh -p $SSH_PORT' $SSH_TARGET:$REMOTE_DIR/runs/ ./runs/"
