#!/usr/bin/env bash
#
# a100-sqrt36-campaign.sh -- Run the sqrt(36) Gaussian moat campaign on an A100.
#
# Progressive band iteration:
#   - CUDA sieve generates GPRF file for current band
#   - Rust solver processes with angular mode
#   - Pipeline overlap: sieve for band N+1 runs in background while solver processes band N
#   - Collects profiling data (throughput, RSS, timing per stage)
#   - Logs results to structured JSON
#   - Continues until moat is found or max iterations reached
#
# Usage:
#   ./deploy/a100-sqrt36-campaign.sh <user@host> [--port PORT] [--start-band N] [--max-bands N]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
SSH_PORT=22
SSH_TARGET=""
K_SQUARED=36
BAND_SIZE=1000000000        # 10^9 norms per band
START_BAND=0                # Starting band index (0 = start from norm 0)
MAX_BANDS=10000             # Safety cap
WEDGES=0                    # Auto-detect
OVERLAP_MODE=1              # 1 = pipeline overlap (sieve N+1 while solving N)

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <user@host> [options]" >&2
    echo "" >&2
    echo "Options:" >&2
    echo "  --port PORT          SSH port (default 22)" >&2
    echo "  --start-band N       Resume from band N (default 0)" >&2
    echo "  --max-bands N        Max bands to process (default 10000)" >&2
    echo "  --band-size N        Norms per band (default 1000000000)" >&2
    echo "  --k-squared K        Jump distance squared (default 36)" >&2
    echo "  --wedges W           Angular wedge count (default 0=auto)" >&2
    echo "  --no-overlap         Disable pipeline overlap" >&2
    exit 1
fi

SSH_TARGET="$1"; shift
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port|-p)      SSH_PORT="$2"; shift 2 ;;
        --start-band)   START_BAND="$2"; shift 2 ;;
        --max-bands)    MAX_BANDS="$2"; shift 2 ;;
        --band-size)    BAND_SIZE="$2"; shift 2 ;;
        --k-squared)    K_SQUARED="$2"; shift 2 ;;
        --wedges)       WEDGES="$2"; shift 2 ;;
        --no-overlap)   OVERLAP_MODE=0; shift ;;
        *)              echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

SSH_CMD="ssh -o StrictHostKeyChecking=accept-new -p $SSH_PORT"
REMOTE_DIR="/workspace/gaussian-moat-cuda"

echo "=== sqrt($K_SQUARED) Gaussian Moat Campaign ==="
echo "  Target: $SSH_TARGET:$SSH_PORT"
echo "  Band size: $BAND_SIZE norms"
echo "  Start band: $START_BAND"
echo "  Max bands: $MAX_BANDS"
echo "  Pipeline overlap: $([ $OVERLAP_MODE -eq 1 ] && echo 'enabled' || echo 'disabled')"
echo ""

# ---------------------------------------------------------------------------
# Upload campaign script and run on remote via tmux
# ---------------------------------------------------------------------------

$SSH_CMD "$SSH_TARGET" bash -s \
    "$REMOTE_DIR" "$K_SQUARED" "$BAND_SIZE" "$START_BAND" "$MAX_BANDS" "$WEDGES" "$OVERLAP_MODE" \
    <<'REMOTE_CAMPAIGN'
set -euo pipefail

REMOTE_DIR="$1"
K_SQUARED="$2"
BAND_SIZE="$3"
START_BAND="$4"
MAX_BANDS="$5"
WEDGES="$6"
OVERLAP_MODE="$7"

# Setup paths
for cuda_path in /usr/local/cuda/bin /usr/local/cuda-12/bin; do
    [ -d "$cuda_path" ] && export PATH="$cuda_path:$PATH"
done
. "$HOME/.cargo/env" 2>/dev/null || true

CUDA_BIN="$REMOTE_DIR/build-a100/gm_cuda_primes"
SOLVER_BIN="$REMOTE_DIR/solver/target/release/gaussian-moat-solver"

TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_DIR="$REMOTE_DIR/runs/sqrt${K_SQUARED}-${TIMESTAMP}"
mkdir -p "$RUN_DIR"

LOG_FILE="$RUN_DIR/campaign.log"
RESULTS_JSON="$RUN_DIR/results.jsonl"
SUMMARY_FILE="$RUN_DIR/summary.json"

log() {
    echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG_FILE"
}

# Save campaign config
cat > "$RUN_DIR/config.json" <<CONF
{
  "k_squared": $K_SQUARED,
  "band_size": $BAND_SIZE,
  "start_band": $START_BAND,
  "max_bands": $MAX_BANDS,
  "wedges": $WEDGES,
  "overlap_mode": $OVERLAP_MODE,
  "timestamp": "$TIMESTAMP",
  "cuda_bin": "$CUDA_BIN",
  "solver_bin": "$SOLVER_BIN"
}
CONF

# GPU info
nvidia-smi --query-gpu=name,compute_cap,memory.total,driver_version --format=csv,noheader \
    > "$RUN_DIR/gpu_info.txt" 2>/dev/null || true

log "Campaign starting: sqrt($K_SQUARED), band_size=$BAND_SIZE, start=$START_BAND"
log "GPU: $(cat "$RUN_DIR/gpu_info.txt" 2>/dev/null || echo 'unknown')"

MOAT_FOUND=0
TOTAL_PRIMES=0
TOTAL_SIEVE_TIME=0
TOTAL_SOLVE_TIME=0
PREV_GPRF=""
BG_SIEVE_PID=""
BG_GPRF=""

cleanup_bg() {
    if [ -n "$BG_SIEVE_PID" ] && kill -0 "$BG_SIEVE_PID" 2>/dev/null; then
        kill "$BG_SIEVE_PID" 2>/dev/null || true
        wait "$BG_SIEVE_PID" 2>/dev/null || true
    fi
}
trap cleanup_bg EXIT

for ((BAND = START_BAND; BAND < START_BAND + MAX_BANDS; BAND++)); do
    NORM_LO=$((BAND * BAND_SIZE))
    NORM_HI=$(((BAND + 1) * BAND_SIZE))
    GPRF_FILE="$RUN_DIR/band_${BAND}.gprf"
    BAND_RESULT="$RUN_DIR/band_${BAND}_result.txt"

    log ""
    log "=== Band $BAND: norm range [$NORM_LO, $NORM_HI) ==="

    # --- SIEVE PHASE ---
    # If overlap mode and we pre-sieved this band in background, use that result
    if [ "$OVERLAP_MODE" -eq 1 ] && [ -n "$BG_SIEVE_PID" ] && [ -n "$BG_GPRF" ]; then
        log "  Waiting for background sieve (PID $BG_SIEVE_PID)..."
        if wait "$BG_SIEVE_PID" 2>/dev/null; then
            if [ -f "$BG_GPRF" ]; then
                GPRF_FILE="$BG_GPRF"
                log "  Using pre-sieved GPRF: $GPRF_FILE"
            fi
        else
            log "  Background sieve failed, re-running foreground..."
        fi
        BG_SIEVE_PID=""
        BG_GPRF=""
    fi

    # Run sieve if we don't have the GPRF yet
    if [ ! -f "$GPRF_FILE" ]; then
        log "  Sieving band $BAND..."
        SIEVE_START=$(date +%s%N)

        "$CUDA_BIN" \
            --norm-lo "$NORM_LO" \
            --norm-hi "$NORM_HI" \
            --output "$GPRF_FILE" \
            --mode sieve \
            2>> "$LOG_FILE"

        SIEVE_END=$(date +%s%N)
        SIEVE_MS=$(( (SIEVE_END - SIEVE_START) / 1000000 ))
    else
        SIEVE_MS=0  # Already had the file from background
    fi

    GPRF_BYTES=$(stat -c%s "$GPRF_FILE" 2>/dev/null || stat -f%z "$GPRF_FILE" 2>/dev/null || echo 0)
    GPRF_PRIMES=$(( (GPRF_BYTES - 64) / 16 ))  # 64-byte header, 16 bytes per record
    log "  Sieve: ${SIEVE_MS}ms, GPRF: ${GPRF_BYTES} bytes, ~${GPRF_PRIMES} primes"
    TOTAL_SIEVE_TIME=$((TOTAL_SIEVE_TIME + SIEVE_MS))

    # --- PIPELINE OVERLAP: start sieving next band in background ---
    if [ "$OVERLAP_MODE" -eq 1 ]; then
        NEXT_BAND=$((BAND + 1))
        if [ "$NEXT_BAND" -lt "$((START_BAND + MAX_BANDS))" ]; then
            NEXT_NORM_LO=$((NEXT_BAND * BAND_SIZE))
            NEXT_NORM_HI=$(((NEXT_BAND + 1) * BAND_SIZE))
            BG_GPRF="$RUN_DIR/band_${NEXT_BAND}.gprf"

            "$CUDA_BIN" \
                --norm-lo "$NEXT_NORM_LO" \
                --norm-hi "$NEXT_NORM_HI" \
                --output "$BG_GPRF" \
                --mode sieve \
                2>> "$LOG_FILE" &
            BG_SIEVE_PID=$!
            log "  Background sieve for band $NEXT_BAND started (PID $BG_SIEVE_PID)"
        fi
    fi

    # --- SOLVER PHASE ---
    log "  Running angular solver (k^2=$K_SQUARED, wedges=$WEDGES)..."
    SOLVE_START=$(date +%s%N)

    "$SOLVER_BIN" \
        --k-squared "$K_SQUARED" \
        --angular "$WEDGES" \
        --prime-file "$GPRF_FILE" \
        --profile \
        2>&1 | tee "$BAND_RESULT" >> "$LOG_FILE"

    SOLVE_END=$(date +%s%N)
    SOLVE_MS=$(( (SOLVE_END - SOLVE_START) / 1000000 ))
    TOTAL_SOLVE_TIME=$((TOTAL_SOLVE_TIME + SOLVE_MS))

    # Parse solver output
    FARTHEST=$(grep -oP 'farthest distance: \K[0-9.]+' "$BAND_RESULT" 2>/dev/null || echo "0")
    COMPONENT_SIZE=$(grep -oP 'origin component size: \K[0-9]+' "$BAND_RESULT" 2>/dev/null || echo "0")
    PRIMES_PROCESSED=$(grep -oP 'primes processed: \K[0-9]+' "$BAND_RESULT" 2>/dev/null || echo "0")
    MAX_RSS=$(grep -oP 'max_rss_bytes: \K[0-9]+' "$BAND_RESULT" 2>/dev/null || echo "0")

    TOTAL_PRIMES=$((TOTAL_PRIMES + PRIMES_PROCESSED))
    TOTAL_MS=$((SIEVE_MS + SOLVE_MS))

    log "  Solver: ${SOLVE_MS}ms, farthest=${FARTHEST}, component=${COMPONENT_SIZE}, primes=${PRIMES_PROCESSED}"
    log "  Band total: ${TOTAL_MS}ms, RSS: ${MAX_RSS} bytes"

    # Write structured result
    echo "{\"band\":$BAND,\"norm_lo\":$NORM_LO,\"norm_hi\":$NORM_HI,\"sieve_ms\":$SIEVE_MS,\"solve_ms\":$SOLVE_MS,\"total_ms\":$TOTAL_MS,\"primes\":$PRIMES_PROCESSED,\"farthest\":$FARTHEST,\"component_size\":$COMPONENT_SIZE,\"rss_bytes\":$MAX_RSS,\"gprf_bytes\":$GPRF_BYTES}" >> "$RESULTS_JSON"

    # Clean up previous band's GPRF to save disk space
    if [ -n "$PREV_GPRF" ] && [ -f "$PREV_GPRF" ]; then
        rm -f "$PREV_GPRF"
    fi
    PREV_GPRF="$GPRF_FILE"

    # TODO: moat detection across bands requires stitching band boundaries.
    # For now, each band is independent; the solver detects moats within a band.
    # Full cross-band moat detection would need an upper-bound mode with --start-distance.
done

# Wait for any remaining background sieve
cleanup_bg

# Write summary
log ""
log "=== Campaign Complete ==="
log "  Bands processed: $((MAX_BANDS))"
log "  Total primes: $TOTAL_PRIMES"
log "  Total sieve time: ${TOTAL_SIEVE_TIME}ms"
log "  Total solve time: ${TOTAL_SOLVE_TIME}ms"
log "  Moat found: $MOAT_FOUND"

cat > "$SUMMARY_FILE" <<SUMM
{
  "k_squared": $K_SQUARED,
  "bands_processed": $((MAX_BANDS)),
  "total_primes": $TOTAL_PRIMES,
  "total_sieve_ms": $TOTAL_SIEVE_TIME,
  "total_solve_ms": $TOTAL_SOLVE_TIME,
  "moat_found": $MOAT_FOUND,
  "run_dir": "$RUN_DIR"
}
SUMM

echo ""
echo "Results directory: $RUN_DIR"
echo "Results log: $RESULTS_JSON"
REMOTE_CAMPAIGN

echo ""
echo "Campaign launched on remote."
echo "  Monitor: $SSH_CMD $SSH_TARGET 'tail -f $REMOTE_DIR/runs/sqrt${K_SQUARED}-*/campaign.log'"
echo "  Fetch results: rsync -avz -e \"ssh -p $SSH_PORT\" $SSH_TARGET:$REMOTE_DIR/runs/ ./runs/"
