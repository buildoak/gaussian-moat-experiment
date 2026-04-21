#!/usr/bin/env bash
#
# sqrt32-chunked-campaign.sh -- Chunked LB sqrt(32) Gaussian moat campaign.
#
# Sieve generates sorted GPRF chunks, solver processes each at full speed.
# Each chunk overlaps the previous by K_NORM_OVERLAP norms to ensure no
# primes near the boundary are missed. The solver's --resume-farthest-norm
# flag carries the origin component's reach across chunks.
#
# Usage (local):
#   ./deploy/sqrt32-chunked-campaign.sh
#
# Usage (remote via SSH):
#   SSH_CMD="ssh -p 32568 root@ssh4.vast.ai" ./deploy/sqrt32-chunked-campaign.sh
#
# Environment variables:
#   CUDA_BIN     Path to gm_cuda_primes binary (default: ./build/gm_cuda_primes)
#   SOLVER_BIN   Path to solver binary (default: ./solver/target/release/gaussian-moat-solver)
#   WORK_DIR     Scratch directory for chunks (default: /tmp/gaussian-moat-chunks)
#   CHUNK_SIZE   Norms per chunk (default: 100000000000 = 10^11)
#   NORM_MAX     Maximum norm to reach (default: 10^13)
#   K_SQUARED    Jump distance squared (default: 32)
#   WEDGES       Angular wedge count (default: 0 = auto)
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
K_SQUARED="${K_SQUARED:-32}"
K_NORM_OVERLAP="${K_NORM_OVERLAP:-64}"  # >= k^2 worth of norm overlap
CHUNK_SIZE="${CHUNK_SIZE:-100000000000}"  # 10^11 norms per chunk
NORM_MAX="${NORM_MAX:-10000000000000}"    # 10^13
WEDGES="${WEDGES:-0}"
WORK_DIR="${WORK_DIR:-/tmp/gaussian-moat-chunks}"
CUDA_BIN="${CUDA_BIN:-./build/gm_cuda_primes}"
SOLVER_BIN="${SOLVER_BIN:-./solver/target/release/gaussian-moat-solver}"
CHECKPOINT_FILE="$WORK_DIR/checkpoint.txt"
LOG_FILE="$WORK_DIR/campaign.log"
RESULTS_JSON="$WORK_DIR/results.jsonl"

mkdir -p "$WORK_DIR"

log() {
    echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG_FILE"
}

# ---------------------------------------------------------------------------
# Resume from checkpoint if exists
# ---------------------------------------------------------------------------
START_NORM=0
RESUME_NORM=0
CHUNK_NUM=0
if [ -f "$CHECKPOINT_FILE" ]; then
    source "$CHECKPOINT_FILE"
    log "Resuming from chunk=$CHUNK_NUM norm=$START_NORM farthest_norm=$RESUME_NORM"
fi

log "=== sqrt($K_SQUARED) Chunked Campaign ==="
log "  chunk_size=$CHUNK_SIZE norm_max=$NORM_MAX overlap=$K_NORM_OVERLAP"
log "  cuda=$CUDA_BIN solver=$SOLVER_BIN"
log "  work_dir=$WORK_DIR"

# ---------------------------------------------------------------------------
# Campaign loop
# ---------------------------------------------------------------------------
NORM_LO=$START_NORM
TOTAL_PRIMES=0
TOTAL_SIEVE_MS=0
TOTAL_SOLVE_MS=0

while [ "$NORM_LO" -lt "$NORM_MAX" ]; do
    NORM_HI=$((NORM_LO + CHUNK_SIZE))
    if [ "$NORM_HI" -gt "$NORM_MAX" ]; then
        NORM_HI=$NORM_MAX
    fi

    # Apply overlap: start sieve k^2 norms before the boundary (except first chunk)
    if [ "$NORM_LO" -gt 0 ]; then
        SIEVE_LO=$((NORM_LO - K_NORM_OVERLAP))
    else
        SIEVE_LO=0
    fi

    CHUNK_FILE="$WORK_DIR/chunk_${CHUNK_NUM}.gprf"

    log ""
    log "=== Chunk $CHUNK_NUM: sieve [$SIEVE_LO, $NORM_HI), solve [$NORM_LO, $NORM_HI) ==="

    # --- SIEVE PHASE ---
    SIEVE_START=$(date +%s%N)

    "$CUDA_BIN" \
        --norm-lo "$SIEVE_LO" \
        --norm-hi "$NORM_HI" \
        --output "$CHUNK_FILE" \
        --mode sieve \
        2>> "$LOG_FILE"

    SIEVE_END=$(date +%s%N)
    SIEVE_MS=$(( (SIEVE_END - SIEVE_START) / 1000000 ))

    GPRF_BYTES=$(stat -c%s "$CHUNK_FILE" 2>/dev/null || stat -f%z "$CHUNK_FILE" 2>/dev/null || echo 0)
    GPRF_PRIMES=$(( (GPRF_BYTES - 64) / 16 ))
    log "  Sieve: ${SIEVE_MS}ms, ${GPRF_PRIMES} primes, ${GPRF_BYTES} bytes"
    TOTAL_SIEVE_MS=$((TOTAL_SIEVE_MS + SIEVE_MS))

    # --- SOLVER PHASE ---
    SOLVER_ARGS="--k-squared $K_SQUARED --angular $WEDGES --prime-file $CHUNK_FILE --profile"
    if [ "$RESUME_NORM" -gt 0 ]; then
        SOLVER_ARGS="$SOLVER_ARGS --resume-farthest-norm $RESUME_NORM"
    fi

    SOLVE_START=$(date +%s%N)

    RESULT=$("$SOLVER_BIN" $SOLVER_ARGS 2>&1) || true

    SOLVE_END=$(date +%s%N)
    SOLVE_MS=$(( (SOLVE_END - SOLVE_START) / 1000000 ))
    TOTAL_SOLVE_MS=$((TOTAL_SOLVE_MS + SOLVE_MS))

    # Parse solver output
    FARTHEST_NORM=$(echo "$RESULT" | grep -oP 'farthest_norm=\K[0-9]+' || echo "0")
    FARTHEST_DIST=$(echo "$RESULT" | grep -oP 'farthest distance: \K[0-9.]+' || echo "0")
    COMPONENT_SIZE=$(echo "$RESULT" | grep -oP 'origin component size: \K[0-9]+' || echo "0")
    PRIMES_PROCESSED=$(echo "$RESULT" | grep -oP 'primes processed: \K[0-9]+' || echo "0")
    MAX_RSS=$(echo "$RESULT" | grep -oP 'max_rss_bytes: \K[0-9]+' || echo "0")

    TOTAL_PRIMES=$((TOTAL_PRIMES + PRIMES_PROCESSED))

    # Update farthest norm for next chunk's resume
    if [ "$FARTHEST_NORM" -gt "$RESUME_NORM" ] 2>/dev/null; then
        RESUME_NORM=$FARTHEST_NORM
    fi

    log "  Solver: ${SOLVE_MS}ms, farthest_norm=${FARTHEST_NORM}, dist=${FARTHEST_DIST}, component=${COMPONENT_SIZE}"
    log "  Chunk total: $((SIEVE_MS + SOLVE_MS))ms, cumulative primes: $TOTAL_PRIMES"

    # Write structured result
    echo "{\"chunk\":$CHUNK_NUM,\"norm_lo\":$NORM_LO,\"norm_hi\":$NORM_HI,\"sieve_ms\":$SIEVE_MS,\"solve_ms\":$SOLVE_MS,\"primes\":$PRIMES_PROCESSED,\"farthest_norm\":$FARTHEST_NORM,\"farthest_dist\":$FARTHEST_DIST,\"component_size\":$COMPONENT_SIZE,\"rss_bytes\":$MAX_RSS}" >> "$RESULTS_JSON"

    # Save checkpoint (atomic write via temp file)
    cat > "$CHECKPOINT_FILE.tmp" <<CKPT
START_NORM=$NORM_HI
RESUME_NORM=$RESUME_NORM
CHUNK_NUM=$((CHUNK_NUM + 1))
CKPT
    mv "$CHECKPOINT_FILE.tmp" "$CHECKPOINT_FILE"

    # Cleanup chunk file to save disk
    rm -f "$CHUNK_FILE"

    # Check if moat found
    if echo "$RESULT" | grep -q "MOAT_FOUND"; then
        log ""
        log "!!! MOAT FOUND at chunk $CHUNK_NUM !!!"
        log "  farthest_norm=$FARTHEST_NORM dist=$FARTHEST_DIST"
        log "  component_size=$COMPONENT_SIZE"
        break
    fi

    NORM_LO=$NORM_HI
    CHUNK_NUM=$((CHUNK_NUM + 1))
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
log ""
log "=== Campaign Summary ==="
log "  Chunks processed: $CHUNK_NUM"
log "  Total primes: $TOTAL_PRIMES"
log "  Total sieve time: ${TOTAL_SIEVE_MS}ms"
log "  Total solve time: ${TOTAL_SOLVE_MS}ms"
log "  Farthest norm reached: $RESUME_NORM"
log "  Results: $RESULTS_JSON"
