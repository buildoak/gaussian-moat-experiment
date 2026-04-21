#!/usr/bin/env bash
#
# 4090-connector-experiments.sh -- Baseline + upper-bound experiment suite.
#
# Establishes Jetson-vs-4090 parity at identical configs, then pushes to
# the real goal: upper-bound sqrt(36) connectivity proof.
#
# Experiments:
#   1. CUDA sieve baseline (10^9, 10^15 — skip 10^18 for now)
#   2. Connector baseline (k^2=36, angular=0/auto, near-origin GPRF)
#   3. Wedge sweep: angular={4,8,16,32} — find throughput vs RSS sweet spot
#   4. Upper-bound sqrt(36) — the money shot
#   5. Correctness gate: k^2=2, LB + UB, farthest_point == (11, 4)
#
# Usage:
#   ./deploy/4090-connector-experiments.sh <user@host> [--port PORT] [--device 4090|jetson]
#
# Prerequisites:
#   - Remote host has CUDA toolkit and Rust toolchain installed
#   - SSH access configured (key-based recommended)
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

echo "=== Gaussian Moat Experiment Suite ==="
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
    CMAKE_ARGS="-DTARGET_DEVICE=${DEVICE}"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. $CMAKE_ARGS 2>&1 | tail -5
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
OUTDIR="$REMOTE_DIR/runs/baseline-${DEVICE}-${TIMESTAMP}"
mkdir -p "$OUTDIR"

LOG="$OUTDIR/experiment.log"
RESULTS="$OUTDIR/results.jsonl"

log() {
    echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG"
}

# Helper: extract profile/summary fields from solver output
extract_field() {
    local file="$1" field="$2"
    grep "$field" "$file" 2>/dev/null | awk '{print $NF}' || echo "0"
}

# GPU and CPU info
log "=== Hardware Info ==="
nvidia-smi --query-gpu=name,compute_cap,memory.total,memory.free --format=csv,noheader 2>/dev/null | tee -a "$LOG" || true
nproc | xargs -I{} echo "CPUs: {}" | tee -a "$LOG"
free -h 2>/dev/null | head -2 | tee -a "$LOG" || true
log ""

# =========================================================================
# EXPERIMENT 1: CUDA Sieve baseline — 10^9 and 10^15
#   Apples-to-apples: same window, same scales.
#   Capture: wall time, primes/sec, prime count.
# =========================================================================
log "==========================================="
log " EXPERIMENT 1: CUDA Sieve Baseline"
log "  Scales: 10^9, 10^15 (skip 10^18)"
log "==========================================="

WINDOW=1000000000
declare -a SCALE_NAMES=("1e9" "1e15")
declare -a SCALE_LO=(0 1000000000000000)

for i in 0 1; do
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
    if [ "$WALL_MS" -gt 0 ]; then
        PPS=$(( PRIMES * 1000 / WALL_MS ))
    else
        PPS=0
    fi
    log "  Scale $NAME: ${PRIMES} primes, ${WALL_MS}ms, ${PPS} primes/sec"

    echo "{\"exp\":\"sieve\",\"scale\":\"$NAME\",\"primes\":$PRIMES,\"wall_ms\":$WALL_MS,\"primes_per_sec\":$PPS,\"device\":\"$DEVICE\"}" >> "$RESULTS"

    # Keep only the 1e9 GPRF for connector experiments
    if [ "$NAME" != "1e9" ]; then
        rm -f "$SIEVE_OUT"
    fi
done

# Validate 1e9 GPRF exists
GPRF_1E9="$OUTDIR/sieve_1e9.gprf"
if [ ! -f "$GPRF_1E9" ]; then
    log "ERROR: 1e9 GPRF not found, cannot run connector experiments"
    exit 1
fi

GPRF_SIZE=$(stat -c%s "$GPRF_1E9" 2>/dev/null || stat -f%z "$GPRF_1E9" 2>/dev/null)
GPRF_PRIMES=$(( (GPRF_SIZE - 64) / 16 ))
log ""
log "Using GPRF: $GPRF_1E9 (~$GPRF_PRIMES primes, $GPRF_SIZE bytes)"
log ""

# =========================================================================
# EXPERIMENT 2: Connector Baseline
#   angular=0 (auto), k^2=36, near-origin GPRF from Exp 1.
#   This is the Jetson-proven config: 1.35-1.78M primes/sec.
#   If we see similar numbers on 4090, the code is healthy.
# =========================================================================
log "==========================================="
log " EXPERIMENT 2: Connector Baseline"
log "  angular=0 (auto), k^2=36, GPRF file"
log "  Jetson reference: 1.35-1.78M primes/sec"
log "==========================================="

EXP2_OUT="$OUTDIR/exp2_connector_baseline.txt"

START_NS=$(date +%s%N)
"$SOLVER_BIN" \
    --k-squared 36 \
    --angular 0 \
    --prime-file "$GPRF_1E9" \
    --profile \
    2>&1 | tee "$EXP2_OUT"
END_NS=$(date +%s%N)
WALL_MS=$(( (END_NS - START_NS) / 1000000 ))

RSS=$(extract_field "$EXP2_OUT" "max_rss_bytes:")
PPS=$(extract_field "$EXP2_OUT" "primes_per_second:")
WEDGES=$(extract_field "$EXP2_OUT" "wedges used:")
PRIMES=$(extract_field "$EXP2_OUT" "primes processed:")

RSS_MB=$(echo "$RSS" | awk '{printf "%.0f", $1/1048576}')
log "  Result: ${PPS} primes/sec, ${WEDGES} wedges, RSS=${RSS_MB}MB, wall=${WALL_MS}ms"
echo "{\"exp\":\"connector_baseline\",\"angular\":0,\"k_squared\":36,\"wedges\":\"$WEDGES\",\"primes\":\"$PRIMES\",\"wall_ms\":$WALL_MS,\"rss_bytes\":\"$RSS\",\"primes_per_sec\":\"$PPS\",\"device\":\"$DEVICE\"}" >> "$RESULTS"
log ""

# =========================================================================
# EXPERIMENT 3: Wedge Sweep (small)
#   angular = 4, 8, 16, 32 — skip 64/128 (known bad, memory blowup).
#   Same GPRF, k^2=36. Find the sweet spot for throughput vs RSS.
# =========================================================================
log "==========================================="
log " EXPERIMENT 3: Wedge Sweep"
log "  angular = {4, 8, 16, 32}"
log "  k^2=36, near-origin GPRF"
log "==========================================="

for WEDGE_COUNT in 4 8 16 32; do
    log "  angular=$WEDGE_COUNT ..."

    RESULT_FILE="$OUTDIR/exp3_wedges_${WEDGE_COUNT}.txt"

    START_NS=$(date +%s%N)
    timeout 600 "$SOLVER_BIN" \
        --k-squared 36 \
        --angular "$WEDGE_COUNT" \
        --prime-file "$GPRF_1E9" \
        --profile \
        2>&1 | tee "$RESULT_FILE" || true
    END_NS=$(date +%s%N)
    WALL_MS=$(( (END_NS - START_NS) / 1000000 ))

    RSS=$(extract_field "$RESULT_FILE" "max_rss_bytes:")
    PPS=$(extract_field "$RESULT_FILE" "primes_per_second:")
    PRIMES=$(extract_field "$RESULT_FILE" "primes processed:")

    RSS_MB=$(echo "$RSS" | awk '{printf "%.0f", $1/1048576}')
    log "  angular=$WEDGE_COUNT: ${PPS} primes/sec, RSS=${RSS_MB}MB, wall=${WALL_MS}ms"

    echo "{\"exp\":\"wedge_sweep\",\"angular\":$WEDGE_COUNT,\"k_squared\":36,\"primes\":\"$PRIMES\",\"wall_ms\":$WALL_MS,\"rss_bytes\":\"$RSS\",\"primes_per_sec\":\"$PPS\",\"device\":\"$DEVICE\"}" >> "$RESULTS"
done
log ""

# =========================================================================
# EXPERIMENT 4: Upper-bound sqrt(36) — the money shot
#   Generate GPRF for a window in the sqrt(36)=6 distance range using sieve.
#   Run connector with --start-distance, upper-bound mode, k^2=36.
#   Verify: primes processed > 0, farthest point makes sense.
#
#   For sqrt(k^2=36), the start distance is 6.
#   upper_bound_start_norm = (start_distance - k_radius)^2
#     k_radius = ceil(sqrt(36)) = 6, so start_norm = (6-6)^2 = 0
#   That means we need primes from norm 0 upward — our 1e9 GPRF covers this.
#   We also test start_distance=7 which gives start_norm = (7-6)^2 = 1.
# =========================================================================
log "==========================================="
log " EXPERIMENT 4: Upper-bound sqrt(36)"
log "  k^2=36, start-distance=6 and 7"
log "  This is the real goal."
log "==========================================="

for START_DIST in 6 7; do
    log "  start-distance=$START_DIST ..."

    EXP4_OUT="$OUTDIR/exp4_ub_sqrt36_sd${START_DIST}.txt"

    START_NS=$(date +%s%N)
    "$SOLVER_BIN" \
        --k-squared 36 \
        --angular 0 \
        --start-distance "$START_DIST" \
        --prime-file "$GPRF_1E9" \
        --profile \
        2>&1 | tee "$EXP4_OUT"
    END_NS=$(date +%s%N)
    WALL_MS=$(( (END_NS - START_NS) / 1000000 ))

    PPS=$(extract_field "$EXP4_OUT" "primes_per_second:")
    PRIMES=$(extract_field "$EXP4_OUT" "primes processed:")
    FARTHEST=$(grep "farthest point:" "$EXP4_OUT" 2>/dev/null || echo "unknown")
    COMPONENT=$(extract_field "$EXP4_OUT" "origin component size:")
    RSS=$(extract_field "$EXP4_OUT" "max_rss_bytes:")

    RSS_MB=$(echo "$RSS" | awk '{printf "%.0f", $1/1048576}')
    log "  sd=$START_DIST: $FARTHEST, component=$COMPONENT, ${PPS} p/s, ${PRIMES} primes, RSS=${RSS_MB}MB"

    echo "{\"exp\":\"ub_sqrt36\",\"start_distance\":$START_DIST,\"k_squared\":36,\"primes\":\"$PRIMES\",\"wall_ms\":$WALL_MS,\"rss_bytes\":\"$RSS\",\"primes_per_sec\":\"$PPS\",\"farthest\":\"$FARTHEST\",\"component\":\"$COMPONENT\",\"device\":\"$DEVICE\"}" >> "$RESULTS"
done
log ""

# =========================================================================
# EXPERIMENT 5: Correctness Gate
#   k^2=2, both LB and UB modes.
#   Gate: farthest_point == (11, 4) in both modes.
#   This is the known-good invariant from unit tests.
# =========================================================================
log "==========================================="
log " EXPERIMENT 5: Correctness Gate"
log "  k^2=2, LB + UB, farthest must be (11, 4)"
log "==========================================="

# Generate a small GPRF for k^2=2
GPRF_SMALL="$OUTDIR/sieve_small.gprf"
"$CUDA_BIN" --norm-lo 0 --norm-hi 10000 --output "$GPRF_SMALL" --mode sieve 2>/dev/null || true

EXP5_LB="$OUTDIR/exp5_k2_lb.txt"
EXP5_UB="$OUTDIR/exp5_k2_ub.txt"

# Lower-bound mode
"$SOLVER_BIN" \
    --k-squared 2 \
    --angular 0 \
    --prime-file "$GPRF_SMALL" \
    --profile \
    2>&1 | tee "$EXP5_LB"

# Upper-bound mode (start-distance=8, matching unit test)
"$SOLVER_BIN" \
    --k-squared 2 \
    --angular 0 \
    --start-distance 8 \
    --prime-file "$GPRF_SMALL" \
    --profile \
    2>&1 | tee "$EXP5_UB"

FARTHEST_LB=$(grep "farthest point:" "$EXP5_LB" 2>/dev/null || echo "unknown")
FARTHEST_UB=$(grep "farthest point:" "$EXP5_UB" 2>/dev/null || echo "unknown")

GATE_PASS="FAIL"
if echo "$FARTHEST_LB" | grep -q "(11, 4)" && echo "$FARTHEST_UB" | grep -q "(11, 4)"; then
    GATE_PASS="PASS"
fi

log "  Lower-bound: $FARTHEST_LB"
log "  Upper-bound: $FARTHEST_UB"
log "  Gate: $GATE_PASS"

echo "{\"exp\":\"correctness_gate\",\"k_squared\":2,\"farthest_lb\":\"$FARTHEST_LB\",\"farthest_ub\":\"$FARTHEST_UB\",\"gate\":\"$GATE_PASS\",\"device\":\"$DEVICE\"}" >> "$RESULTS"

rm -f "$GPRF_SMALL"
log ""

# =========================================================================
# Summary Table
# =========================================================================
log "==========================================="
log " EXPERIMENT SUMMARY"
log "==========================================="
log ""

# Sieve results
log "--- Sieve Baseline ---"
log "  Scale    | Primes     | Wall (ms) | Primes/sec"
log "  ---------|------------|-----------|----------"
for NAME in 1e9 1e15; do
    FILE="$OUTDIR/sieve_${NAME}.log"
    if [ -f "$FILE" ]; then
        P=$(grep "Primes found:" "$FILE" 2>/dev/null | awk '{print $NF}' || echo "?")
        # Pull wall time from results.jsonl
        W=$(grep "\"scale\":\"$NAME\"" "$RESULTS" 2>/dev/null | sed 's/.*"wall_ms":\([0-9]*\).*/\1/' || echo "?")
        PPS=$(grep "\"scale\":\"$NAME\"" "$RESULTS" 2>/dev/null | sed 's/.*"primes_per_sec":\([0-9]*\).*/\1/' || echo "?")
        printf "  %-8s | %10s | %9s | %s\n" "$NAME" "$P" "$W" "$PPS" | tee -a "$LOG"
    fi
done
log ""

# Connector baseline
log "--- Connector Baseline ---"
if [ -f "$EXP2_OUT" ]; then
    log "  primes/sec: $(extract_field "$EXP2_OUT" "primes_per_second:")"
    log "  wedges:     $(extract_field "$EXP2_OUT" "wedges used:")"
    log "  RSS:        $(extract_field "$EXP2_OUT" "max_rss_bytes:") bytes"
fi
log ""

# Wedge sweep
log "--- Wedge Sweep ---"
log "  Angular | Primes/sec | RSS (MB) | Wall (ms)"
log "  --------|-----------|----------|--------"
for W in 4 8 16 32; do
    FILE="$OUTDIR/exp3_wedges_${W}.txt"
    if [ -f "$FILE" ]; then
        PPS=$(extract_field "$FILE" "primes_per_second:")
        RSS=$(extract_field "$FILE" "max_rss_bytes:")
        RSS_MB=$(echo "$RSS" | awk '{printf "%.0f", $1/1048576}')
        ELAPSED=$(extract_field "$FILE" "elapsed:")
        printf "  %7d | %10s | %8s | %s\n" "$W" "$PPS" "$RSS_MB" "$ELAPSED" | tee -a "$LOG"
    fi
done
log ""

# Upper-bound sqrt(36)
log "--- Upper-bound sqrt(36) ---"
for SD in 6 7; do
    FILE="$OUTDIR/exp4_ub_sqrt36_sd${SD}.txt"
    if [ -f "$FILE" ]; then
        FP=$(grep "farthest point:" "$FILE" 2>/dev/null || echo "?")
        PP=$(extract_field "$FILE" "primes processed:")
        CS=$(extract_field "$FILE" "origin component size:")
        log "  sd=$SD: $FP | primes=$PP | component=$CS"
    fi
done
log ""

# Correctness gate
log "--- Correctness Gate ---"
log "  k^2=2 LB: $FARTHEST_LB"
log "  k^2=2 UB: $FARTHEST_UB"
log "  Gate: $GATE_PASS"
log ""

log "Results file: $RESULTS"
log "Full log: $LOG"
log "Output dir: $OUTDIR"
log ""
log "=== Experiments complete ==="

REMOTE_EXPERIMENTS

echo ""
echo "=== Experiment suite dispatched ==="
echo "Fetch results with:"
echo "  rsync -avz -e 'ssh -p $SSH_PORT' $SSH_TARGET:$REMOTE_DIR/runs/ ./runs/"
