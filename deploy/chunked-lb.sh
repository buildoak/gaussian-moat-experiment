#!/usr/bin/env bash
#
# chunked-lb.sh -- Generic chunked lower-bound Gaussian moat campaign
#   with adaptive angular wedge scheduling and crash recovery.
#
# Sieve generates sorted GPRF chunks, solver processes each at full speed.
# Each chunk overlaps the previous by OVERLAP norms to ensure continuity.
# The solver's --resume-farthest-norm flag carries the origin component's
# reach across chunks.
#
# Adaptive wedge schedule: fewer wedges at low norms (faster, less memory),
# more wedges at high norms (needed for angular resolution as prime density
# drops and ring radii grow).
#
# Usage:
#   ./deploy/chunked-lb.sh --k-squared 32 --norm-max 10000000000000 \
#       [--chunk-size 100000000000] [--overlap 1000] [--norm-start 0] \
#       [--resume-farthest 0] [--work-dir /tmp/gm-chunks] [--wedges auto]
#
# Environment overrides:
#   CUDA_BIN     Path to gm_cuda_primes (default: ./build/gm_cuda_primes)
#   SOLVER_BIN   Path to solver (default: ./solver/target/release/gaussian-moat-solver)
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
K_SQUARED=""
NORM_START=0
NORM_MAX=""
CHUNK_SIZE=100000000000   # 10^11 norms per chunk
OVERLAP=1000              # norm overlap between chunks (generous for sqrt(32))
WORK_DIR="/tmp/gm-chunks"
WEDGES_MODE="auto"        # "auto" = adaptive schedule, or fixed integer
CUDA_BIN="${CUDA_BIN:-./build/gm_cuda_primes}"
SOLVER_BIN="${SOLVER_BIN:-./solver/target/release/gaussian-moat-solver}"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --k-squared)       K_SQUARED="$2"; shift 2 ;;
        --norm-start)      NORM_START="$2"; shift 2 ;;
        --norm-max)        NORM_MAX="$2"; shift 2 ;;
        --chunk-size)      CHUNK_SIZE="$2"; shift 2 ;;
        --overlap)         OVERLAP="$2"; shift 2 ;;
        --resume-farthest) RESUME_FARTHEST_OVERRIDE="$2"; shift 2 ;;
        --work-dir)        WORK_DIR="$2"; shift 2 ;;
        --wedges)          WEDGES_MODE="$2"; shift 2 ;;
        --cuda-bin)        CUDA_BIN="$2"; shift 2 ;;
        --solver-bin)      SOLVER_BIN="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 --k-squared K --norm-max N [options]"
            echo ""
            echo "Required:"
            echo "  --k-squared K        Jump distance squared (e.g. 32)"
            echo "  --norm-max N         Upper norm bound for campaign"
            echo ""
            echo "Optional:"
            echo "  --norm-start N       Starting norm (default: 0)"
            echo "  --chunk-size N       Norms per chunk (default: 10^11)"
            echo "  --overlap N          Norm overlap between chunks (default: 1000)"
            echo "  --resume-farthest N  Resume with origin reaching this norm"
            echo "  --work-dir DIR       Scratch directory (default: /tmp/gm-chunks)"
            echo "  --wedges auto|N      Wedge schedule: 'auto' or fixed count (default: auto)"
            echo "  --cuda-bin PATH      Path to gm_cuda_primes"
            echo "  --solver-bin PATH    Path to gaussian-moat-solver"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$K_SQUARED" || -z "$NORM_MAX" ]]; then
    echo "Error: --k-squared and --norm-max are required" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Paths and state
# ---------------------------------------------------------------------------
mkdir -p "$WORK_DIR"
CHECKPOINT_FILE="$WORK_DIR/checkpoint_k${K_SQUARED}.txt"
LOG_FILE="$WORK_DIR/campaign_k${K_SQUARED}.log"
RESULTS_JSON="$WORK_DIR/results_k${K_SQUARED}.jsonl"

log() {
    echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG_FILE"
}

# ---------------------------------------------------------------------------
# Adaptive wedge schedule
# Based on empirical observation: at low norms, angular resolution doesn't
# need many wedges; at high norms, the ring gets so large that splitting
# into more wedges reduces per-wedge memory and work.
# ---------------------------------------------------------------------------
get_wedges() {
    local norm=$1
    if [[ "$WEDGES_MODE" != "auto" ]]; then
        echo "$WEDGES_MODE"
        return
    fi
    # Adaptive schedule tuned for sqrt(32) campaign on 4090/Jetson
    if   (( norm < 100000000 ));       then echo 1    # < 10^8: 1 wedge
    elif (( norm < 10000000000 ));     then echo 2    # < 10^10: 2 wedges
    elif (( norm < 1000000000000 ));   then echo 4    # < 10^12: 4 wedges
    else                                    echo 8    # >= 10^12: 8 wedges
    fi
}

# ---------------------------------------------------------------------------
# Preflight checks
# ---------------------------------------------------------------------------
if [[ ! -x "$CUDA_BIN" ]]; then
    echo "Error: CUDA binary not found at $CUDA_BIN" >&2
    echo "Build: mkdir -p build && cd build && cmake -DTARGET_DEVICE=4090 .. && make -j" >&2
    exit 1
fi

if [[ ! -x "$SOLVER_BIN" ]]; then
    echo "Error: Solver binary not found at $SOLVER_BIN" >&2
    echo "Build: cd solver && cargo build --release" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Resume from checkpoint or CLI override
# ---------------------------------------------------------------------------
CURRENT_NORM=$NORM_START
FARTHEST_NORM=0
CHUNK_NUM=0

if [[ -n "${RESUME_FARTHEST_OVERRIDE:-}" ]] && [[ "$RESUME_FARTHEST_OVERRIDE" -gt 0 ]]; then
    FARTHEST_NORM=$RESUME_FARTHEST_OVERRIDE
    log "CLI resume override: farthest_norm=$FARTHEST_NORM"
fi

if [[ -f "$CHECKPOINT_FILE" ]]; then
    source "$CHECKPOINT_FILE"
    log "Resuming from checkpoint: chunk=$CHUNK_NUM norm=$CURRENT_NORM farthest=$FARTHEST_NORM"
fi

log "=== Chunked LB Campaign: k^2=$K_SQUARED ==="
log "  norm_range=[$CURRENT_NORM, $NORM_MAX)"
log "  chunk_size=$CHUNK_SIZE overlap=$OVERLAP wedges=$WEDGES_MODE"
log "  cuda=$CUDA_BIN"
log "  solver=$SOLVER_BIN"
log "  work_dir=$WORK_DIR"

# ---------------------------------------------------------------------------
# Campaign loop
# ---------------------------------------------------------------------------
TOTAL_PRIMES=0
TOTAL_SIEVE_MS=0
TOTAL_SOLVE_MS=0

while (( CURRENT_NORM < NORM_MAX )); do
    CHUNK_HI=$((CURRENT_NORM + CHUNK_SIZE))
    (( CHUNK_HI > NORM_MAX )) && CHUNK_HI=$NORM_MAX

    # Sieve overlap: start OVERLAP norms before boundary (except first chunk)
    if (( CURRENT_NORM > 0 )); then
        SIEVE_LO=$((CURRENT_NORM - OVERLAP))
        (( SIEVE_LO < 0 )) && SIEVE_LO=0
    else
        SIEVE_LO=0
    fi

    WEDGES=$(get_wedges "$CURRENT_NORM")
    CHUNK_FILE="$WORK_DIR/chunk_${CHUNK_NUM}.gprf"

    log ""
    log "=== Chunk $CHUNK_NUM: sieve [$SIEVE_LO, $CHUNK_HI) | solve wedges=$WEDGES | farthest=$FARTHEST_NORM ==="

    # --- SIEVE PHASE ---
    SIEVE_START=$(date +%s%N)

    "$CUDA_BIN" \
        --norm-lo "$SIEVE_LO" \
        --norm-hi "$CHUNK_HI" \
        --output "$CHUNK_FILE" \
        --mode sieve \
        --k-squared "$K_SQUARED" \
        2>> "$LOG_FILE"

    SIEVE_END=$(date +%s%N)
    SIEVE_MS=$(( (SIEVE_END - SIEVE_START) / 1000000 ))

    GPRF_BYTES=$(stat -c%s "$CHUNK_FILE" 2>/dev/null || stat -f%z "$CHUNK_FILE" 2>/dev/null || echo 0)
    # GPRF header is 64 bytes, each record is 16 bytes
    GPRF_PRIMES=$(( (GPRF_BYTES - 64) / 16 ))
    log "  Sieve: ${SIEVE_MS}ms, ${GPRF_PRIMES} primes, ${GPRF_BYTES} bytes"
    TOTAL_SIEVE_MS=$((TOTAL_SIEVE_MS + SIEVE_MS))

    # --- SOLVER PHASE ---
    SOLVER_ARGS="--k-squared $K_SQUARED --angular $WEDGES --prime-file $CHUNK_FILE --profile"
    if (( FARTHEST_NORM > 0 )); then
        SOLVER_ARGS="$SOLVER_ARGS --resume-farthest-norm $FARTHEST_NORM"
    fi

    SOLVE_START=$(date +%s%N)

    # shellcheck disable=SC2086
    RESULT=$("$SOLVER_BIN" $SOLVER_ARGS 2>&1) || true

    SOLVE_END=$(date +%s%N)
    SOLVE_MS=$(( (SOLVE_END - SOLVE_START) / 1000000 ))
    TOTAL_SOLVE_MS=$((TOTAL_SOLVE_MS + SOLVE_MS))

    # Parse RESULT line (machine-readable): RESULT farthest_norm=N farthest_point=(a,b) component_size=N primes_processed=N
    NEW_FARTHEST=$(echo "$RESULT" | grep -oP 'RESULT farthest_norm=\K[0-9]+' || echo "0")
    FARTHEST_POINT=$(echo "$RESULT" | grep -oP 'farthest_point=\K\([^)]+\)' || echo "(0,0)")
    COMPONENT_SIZE=$(echo "$RESULT" | grep -oP 'component_size=\K[0-9]+' || echo "0")
    PRIMES_PROCESSED=$(echo "$RESULT" | grep -oP 'primes_processed=\K[0-9]+' || echo "0")

    # Fallback parsing (pre-RESULT line format)
    if [[ "$NEW_FARTHEST" == "0" ]]; then
        NEW_FARTHEST=$(echo "$RESULT" | grep -oP 'farthest_norm=\K[0-9]+' | tail -1 || echo "0")
    fi

    FARTHEST_DIST=$(echo "$RESULT" | grep -oP 'farthest distance: \K[0-9.]+' || echo "0")
    MAX_RSS=$(echo "$RESULT" | grep -oP 'max_rss_bytes: \K[0-9]+' || echo "0")
    PRIMES_PER_SEC=$(echo "$RESULT" | grep -oP 'primes_per_second: \K[0-9]+' || echo "0")

    TOTAL_PRIMES=$((TOTAL_PRIMES + PRIMES_PROCESSED))

    # Update farthest norm for next chunk
    if [[ "$NEW_FARTHEST" -gt "$FARTHEST_NORM" ]] 2>/dev/null; then
        FARTHEST_NORM=$NEW_FARTHEST
    fi

    log "  Solver: ${SOLVE_MS}ms, farthest_norm=${NEW_FARTHEST}, dist=${FARTHEST_DIST}, component=${COMPONENT_SIZE}, throughput=${PRIMES_PER_SEC}/s"
    log "  Chunk total: $((SIEVE_MS + SOLVE_MS))ms | Cumulative: primes=$TOTAL_PRIMES farthest=$FARTHEST_NORM"

    # Structured JSONL result
    echo "{\"chunk\":$CHUNK_NUM,\"norm_lo\":$CURRENT_NORM,\"norm_hi\":$CHUNK_HI,\"sieve_lo\":$SIEVE_LO,\"wedges\":$WEDGES,\"sieve_ms\":$SIEVE_MS,\"solve_ms\":$SOLVE_MS,\"primes\":$PRIMES_PROCESSED,\"farthest_norm\":$NEW_FARTHEST,\"farthest_point\":\"$FARTHEST_POINT\",\"farthest_dist\":$FARTHEST_DIST,\"component_size\":$COMPONENT_SIZE,\"primes_per_sec\":$PRIMES_PER_SEC,\"rss_bytes\":$MAX_RSS}" >> "$RESULTS_JSON"

    # Atomic checkpoint
    cat > "$CHECKPOINT_FILE.tmp" <<CKPT
CURRENT_NORM=$CHUNK_HI
FARTHEST_NORM=$FARTHEST_NORM
CHUNK_NUM=$((CHUNK_NUM + 1))
CKPT
    mv "$CHECKPOINT_FILE.tmp" "$CHECKPOINT_FILE"

    # Cleanup chunk file to save disk
    rm -f "$CHUNK_FILE"

    # Check for moat
    if echo "$RESULT" | grep -q "MOAT_FOUND"; then
        log ""
        log "!!! MOAT FOUND at chunk $CHUNK_NUM !!!"
        log "  farthest_norm=$FARTHEST_NORM dist=$FARTHEST_DIST point=$FARTHEST_POINT"
        log "  component_size=$COMPONENT_SIZE"
        break
    fi

    CURRENT_NORM=$CHUNK_HI
    CHUNK_NUM=$((CHUNK_NUM + 1))
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
log ""
log "=== Campaign Summary ==="
log "  k^2=$K_SQUARED"
log "  Chunks processed: $CHUNK_NUM"
log "  Total primes: $TOTAL_PRIMES"
log "  Total sieve time: ${TOTAL_SIEVE_MS}ms"
log "  Total solve time: ${TOTAL_SOLVE_MS}ms"
log "  Total time: $((TOTAL_SIEVE_MS + TOTAL_SOLVE_MS))ms"
log "  Farthest norm reached: $FARTHEST_NORM"
log "  Results: $RESULTS_JSON"
log "  Checkpoint: $CHECKPOINT_FILE"
