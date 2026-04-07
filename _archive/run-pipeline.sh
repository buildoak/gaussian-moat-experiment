#!/usr/bin/env bash
#
# run-pipeline.sh — Two-stage Gaussian moat pipeline
#   Stage 1: CUDA sieve generates GPRF file with Gaussian primes
#   Stage 2: Rust solver reads GPRF, runs angular connectivity analysis
#
# Usage:
#   ./run-pipeline.sh --k-squared 36 --norm-hi 1000000000 [--norm-lo 0] \
#                     [--output-dir ./output] [--wedges 0]
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
K_SQUARED=""
NORM_LO=0
NORM_HI=""
OUTPUT_DIR="./output"
WEDGES=0  # 0 = auto-detect from core count

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --k-squared)   K_SQUARED="$2"; shift 2 ;;
        --norm-lo)     NORM_LO="$2"; shift 2 ;;
        --norm-hi)     NORM_HI="$2"; shift 2 ;;
        --output-dir)  OUTPUT_DIR="$2"; shift 2 ;;
        --wedges)      WEDGES="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 --k-squared K --norm-hi N [--norm-lo N] [--output-dir DIR] [--wedges W]"
            echo ""
            echo "  --k-squared K   Jump distance squared (e.g., 36 for sqrt(36) moat)"
            echo "  --norm-hi N     Upper norm bound (exclusive) for prime generation"
            echo "  --norm-lo N     Lower norm bound (inclusive, default 0)"
            echo "  --output-dir D  Directory for GPRF and result files (default ./output)"
            echo "  --wedges W      Angular wedge count (0 = auto, default 0)"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$K_SQUARED" || -z "$NORM_HI" ]]; then
    echo "Error: --k-squared and --norm-hi are required" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CUDA_BIN="${SCRIPT_DIR}/build/gm_cuda_primes"
SOLVER_BIN="${SCRIPT_DIR}/solver/target/release/gaussian-moat-solver"

mkdir -p "$OUTPUT_DIR"

GPRF_FILE="${OUTPUT_DIR}/primes_${NORM_LO}_${NORM_HI}.gprf"
RESULT_FILE="${OUTPUT_DIR}/result_k${K_SQUARED}_${NORM_LO}_${NORM_HI}.txt"

# ---------------------------------------------------------------------------
# Memory guard
# ---------------------------------------------------------------------------
# Jetson Orin has 8GB unified memory. Cap virtual memory to avoid OOM kills.
ulimit -v 7000000 2>/dev/null || true  # ~6.7 GB soft limit

# ---------------------------------------------------------------------------
# Preflight checks
# ---------------------------------------------------------------------------
if [[ ! -x "$CUDA_BIN" ]]; then
    echo "Error: CUDA binary not found at $CUDA_BIN" >&2
    echo "Build it first: cd $SCRIPT_DIR && mkdir -p build && cd build && cmake .. && make" >&2
    exit 1
fi

if [[ ! -x "$SOLVER_BIN" ]]; then
    echo "Error: Rust solver not found at $SOLVER_BIN" >&2
    echo "Build it first: cd $SCRIPT_DIR/solver && cargo build --release" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Stage 1: CUDA sieve -> GPRF file
# ---------------------------------------------------------------------------
echo "=== Stage 1: CUDA sieve ===" >&2
echo "  norm range: [$NORM_LO, $NORM_HI)" >&2
echo "  output: $GPRF_FILE" >&2

STAGE1_START=$(date +%s%N)

"$CUDA_BIN" \
    --norm-lo "$NORM_LO" \
    --norm-hi "$NORM_HI" \
    --output "$GPRF_FILE" \
    --mode sieve

STAGE1_END=$(date +%s%N)
STAGE1_MS=$(( (STAGE1_END - STAGE1_START) / 1000000 ))
echo "  Stage 1 complete: ${STAGE1_MS}ms" >&2

if [[ ! -f "$GPRF_FILE" ]]; then
    echo "Error: GPRF file was not created" >&2
    exit 1
fi

GPRF_SIZE=$(stat -c%s "$GPRF_FILE" 2>/dev/null || stat -f%z "$GPRF_FILE" 2>/dev/null)
echo "  GPRF file: ${GPRF_SIZE} bytes" >&2

# ---------------------------------------------------------------------------
# Stage 2: Rust angular solver
# ---------------------------------------------------------------------------
echo "" >&2
echo "=== Stage 2: Angular connectivity ===" >&2
echo "  k-squared: $K_SQUARED" >&2
echo "  wedges: $WEDGES (0=auto)" >&2
echo "  prime file: $GPRF_FILE" >&2

STAGE2_START=$(date +%s%N)

"$SOLVER_BIN" \
    --k-squared "$K_SQUARED" \
    --angular "$WEDGES" \
    --prime-file "$GPRF_FILE" \
    --profile \
    2>&1 | tee "$RESULT_FILE"

STAGE2_END=$(date +%s%N)
STAGE2_MS=$(( (STAGE2_END - STAGE2_START) / 1000000 ))
echo "" >&2
echo "  Stage 2 complete: ${STAGE2_MS}ms" >&2

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
TOTAL_MS=$(( STAGE1_MS + STAGE2_MS ))
echo "" >&2
echo "=== Pipeline complete ===" >&2
echo "  Stage 1 (CUDA sieve): ${STAGE1_MS}ms" >&2
echo "  Stage 2 (angular):    ${STAGE2_MS}ms" >&2
echo "  Total:                ${TOTAL_MS}ms" >&2
echo "  Results: $RESULT_FILE" >&2
