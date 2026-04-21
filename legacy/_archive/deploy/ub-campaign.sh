#!/usr/bin/env bash
#
# ub-campaign.sh — Upper-bound progressive moat search campaign
#
# Two modes:
#   PROGRESSIVE  — Start at distance D, sieve bands forward, advance until
#                  solver detects moat or max-distance reached.
#   BISECT       — Binary search: run progressive probes at candidate distances
#                  to find the transition point.
#
# Each iteration:
#   1. Compute band norm range from current start_distance
#   2. Generate Gaussian primes via CUDA sieve → GPRF file
#   3. Run angular solver with --start-distance
#   4. Parse: MOAT_FOUND → stop. Component at edge → advance start_distance.
#   5. Log result as JSONL
#
# The UB trick (Tsuchimura):
#   --start-distance D tells the solver to fictitiously assume all primes
#   with |z| <= D are already connected to the origin. Then it processes
#   primes forward from (D-k)^2. If the component terminates, D is an
#   upper bound on the farthest reachable distance.
#
# Key insight: the component may extend thousands of distance units past D
# before termination. Each iteration processes ~130M primes (~5 bands).
# Multiple iterations may be needed for a single probe distance.
#
# Checkpoint/resume: reads its own JSONL log on restart, skips completed.
#
# Usage:
#   ./deploy/ub-campaign.sh --k-squared 36 --start-distance 80015782 \
#       [--max-distance 200000000] [--bands-per-iter 5] [--wedges 6]
#
#   # Blind search from 10M:
#   ./deploy/ub-campaign.sh --k-squared 36 --start-distance 10000000 --mode bisect
#
# Environment overrides:
#   CUDA_BIN     Path to gm_cuda_primes
#   SOLVER_BIN   Path to gaussian-moat-solver
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
K_SQUARED=""
START_DISTANCE=""
MAX_DISTANCE=""                  # Hard ceiling; empty = 10x start distance
BANDS_PER_ITER=5                 # Number of band widths per GPRF file
WEDGES=6                         # Angular wedges for solver
WORK_DIR="/tmp/gm-ub"
DRY_RUN=false
MAX_ITERATIONS=200               # Safety: max iterations per progressive run
MODE="progressive"               # "progressive" or "bisect"
BISECT_TOLERANCE=1000000         # Binary search tolerance (distance units)
CUDA_BIN="${CUDA_BIN:-}"
SOLVER_BIN="${SOLVER_BIN:-}"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --k-squared)        K_SQUARED="$2"; shift 2 ;;
        --start-distance)   START_DISTANCE="$2"; shift 2 ;;
        --max-distance)     MAX_DISTANCE="$2"; shift 2 ;;
        --bands-per-iter)   BANDS_PER_ITER="$2"; shift 2 ;;
        --wedges)           WEDGES="$2"; shift 2 ;;
        --work-dir)         WORK_DIR="$2"; shift 2 ;;
        --cuda-bin)         CUDA_BIN="$2"; shift 2 ;;
        --solver-bin)       SOLVER_BIN="$2"; shift 2 ;;
        --max-iterations)   MAX_ITERATIONS="$2"; shift 2 ;;
        --mode)             MODE="$2"; shift 2 ;;
        --bisect-tolerance) BISECT_TOLERANCE="$2"; shift 2 ;;
        --dry-run)          DRY_RUN=true; shift ;;
        -h|--help)
            cat <<'HELP'
Usage: ub-campaign.sh --k-squared K --start-distance D [options]

Required:
  --k-squared K           Jump distance squared (e.g. 36)
  --start-distance D      Starting distance for UB probe

Optional:
  --max-distance D        Hard ceiling distance (default: 10x start)
  --bands-per-iter N      Band widths per GPRF file (default: 5, Jetson safe)
  --wedges N              Angular wedges for solver (default: 6)
  --work-dir DIR          Scratch directory (default: /tmp/gm-ub)
  --cuda-bin PATH         Path to gm_cuda_primes
  --solver-bin PATH       Path to gaussian-moat-solver
  --max-iterations N      Safety limit per progressive run (default: 200)
  --mode MODE             "progressive" (default) or "bisect"
  --bisect-tolerance N    Binary search tolerance in distance units (default: 1000000)
  --dry-run               Print probe plan without executing

Modes:
  progressive   Run forward from start-distance until moat found
  bisect        Binary search for the moat boundary
HELP
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$K_SQUARED" || -z "$START_DISTANCE" ]]; then
    echo "Error: --k-squared and --start-distance are required" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Derived constants
# ---------------------------------------------------------------------------
K_FLOAT=$(python3 -c "import math; print(math.sqrt($K_SQUARED))")
K_INT=$(python3 -c "import math; k=math.isqrt($K_SQUARED); print(k if k*k==$K_SQUARED else k+1)")

if [[ -z "$MAX_DISTANCE" ]]; then
    MAX_DISTANCE=$((START_DISTANCE * 10))
fi

# Default binary paths (Jetson layout)
if [[ -z "$CUDA_BIN" ]]; then
    if [[ -x "$HOME/gaussian-moat-cuda/build/gm_cuda_primes" ]]; then
        CUDA_BIN="$HOME/gaussian-moat-cuda/build/gm_cuda_primes"
    elif [[ -x "./build/gm_cuda_primes" ]]; then
        CUDA_BIN="./build/gm_cuda_primes"
    fi
fi
if [[ -z "$SOLVER_BIN" ]]; then
    if [[ -x "$HOME/gaussian-moat-cuda/solver/target/release/gaussian-moat-solver" ]]; then
        SOLVER_BIN="$HOME/gaussian-moat-cuda/solver/target/release/gaussian-moat-solver"
    elif [[ -x "./solver/target/release/gaussian-moat-solver" ]]; then
        SOLVER_BIN="./solver/target/release/gaussian-moat-solver"
    fi
fi

# ---------------------------------------------------------------------------
# Paths and state
# ---------------------------------------------------------------------------
mkdir -p "$WORK_DIR"
LOG_FILE="$WORK_DIR/ub_k${K_SQUARED}.log"
RESULTS_JSONL="$WORK_DIR/ub_k${K_SQUARED}.jsonl"

log() {
    echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG_FILE"
}

# ---------------------------------------------------------------------------
# Preflight checks
# ---------------------------------------------------------------------------
if [[ "$DRY_RUN" == false ]]; then
    if [[ -z "$CUDA_BIN" || ! -x "$CUDA_BIN" ]]; then
        echo "Error: CUDA binary not found. Set --cuda-bin or CUDA_BIN env var." >&2
        exit 1
    fi
    if [[ -z "$SOLVER_BIN" || ! -x "$SOLVER_BIN" ]]; then
        echo "Error: Solver binary not found. Set --solver-bin or SOLVER_BIN env var." >&2
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# Norm range computation
# ---------------------------------------------------------------------------
# Band width at distance D with step k: approximately 4*D*k norms.
# The solver starts at (D - k_int)^2, and we provide BANDS_PER_ITER
# band widths of primes beyond that.
compute_norms() {
    local dist=$1
    local bands=$2
    python3 -c "
import math
D = $dist
k = math.sqrt($K_SQUARED)
k_int = math.ceil(k)
# Start from solver's start_norm with small buffer
norm_lo = max(0, int((D - k_int) ** 2))
# Band width ~ 4 * D * k (from prior calibration)
band_width = int(4 * D * k)
norm_hi = norm_lo + band_width * $bands
print(f'{norm_lo} {norm_hi} {band_width}')
"
}

# Estimate prime count
estimate_primes() {
    local norm_lo=$1
    local norm_hi=$2
    python3 -c "
import math
lo = max(2, $norm_lo)
hi = $norm_hi
mid = (lo + hi) / 2
if mid < 10:
    print(100)
else:
    density = 1.0 / (2.0 * math.log(mid))
    count = int((hi - lo) * density)
    print(max(count, 1))
"
}

# ---------------------------------------------------------------------------
# Checkpoint/resume
# ---------------------------------------------------------------------------
# For progressive mode: find the last completed iteration matching the given
# tag, and the farthest_dist + last start_distance it reached.
# The tag groups iterations across advancing start_distances.
get_resume_state() {
    local tag=$1
    if [[ ! -f "$RESULTS_JSONL" ]]; then
        echo "0 0 0 0"
        return
    fi
    python3 -c "
import json
last_iter = 0
last_farthest = 0.0
last_start_dist = 0
moat_found = False
with open('$RESULTS_JSONL') as f:
    for line in f:
        try:
            d = json.loads(line)
            if d.get('tag') == '$tag':
                it = d.get('iteration', 0)
                if it > last_iter:
                    last_iter = it
                    last_farthest = d.get('farthest_dist', 0.0)
                    last_start_dist = d.get('start_distance', 0)
                if d.get('moat') == 'yes':
                    moat_found = True
        except:
            pass
print(f'{last_iter} {int(last_farthest)} {1 if moat_found else 0} {last_start_dist}')
" 2>/dev/null || echo "0 0 0 0"
}

# ---------------------------------------------------------------------------
# Progressive UB probe
# ---------------------------------------------------------------------------
# Run forward from start_distance until moat found or max_iterations.
# Returns: PROG_MOAT_FOUND ("yes"/"no"), PROG_FARTHEST_DIST
run_progressive() {
    local start_dist=$1
    local tag=${2:-"PROG"}

    PROG_MOAT_FOUND="no"
    PROG_FARTHEST_DIST=0
    local farthest_dist="0"

    # Check resume state (matched by tag, not start_distance)
    local resume_state
    resume_state=$(get_resume_state "$tag")
    local resume_iter resume_farthest resume_moat resume_last_start
    resume_iter=$(echo "$resume_state" | awk '{print $1}')
    resume_farthest=$(echo "$resume_state" | awk '{print $2}')
    resume_moat=$(echo "$resume_state" | awk '{print $3}')
    resume_last_start=$(echo "$resume_state" | awk '{print $4}')

    if [[ "$resume_moat" == "1" ]]; then
        log "  [$tag] Already completed with moat found"
        PROG_MOAT_FOUND="yes"
        PROG_FARTHEST_DIST=$resume_farthest
        return
    fi

    local iter=$((resume_iter + 1))
    local current_dist=$start_dist
    if [[ "$resume_farthest" -gt 0 ]]; then
        # Resume: use the last start_distance + advance from farthest
        local safety_margin=$((K_INT * 2))
        current_dist=$((resume_farthest - safety_margin))
        start_dist=$current_dist
        log "  [$tag] Resuming from iteration $iter: last_farthest=$resume_farthest, new start_dist=$current_dist"
    fi

    while (( iter <= MAX_ITERATIONS )); do
        local norms
        norms=$(compute_norms "$current_dist" "$BANDS_PER_ITER")
        local norm_lo norm_hi band_width
        norm_lo=$(echo "$norms" | awk '{print $1}')
        norm_hi=$(echo "$norms" | awk '{print $2}')
        band_width=$(echo "$norms" | awk '{print $3}')

        local est_primes
        est_primes=$(estimate_primes "$norm_lo" "$norm_hi")

        local gprf_file="$WORK_DIR/ub_d${start_dist}_iter${iter}.gprf"

        log "  [$tag] Iteration $iter: sieve_distance=$current_dist, norms=[$norm_lo, $norm_hi), est_primes=$est_primes"

        if [[ "$DRY_RUN" == true ]]; then
            log "    [DRY RUN] sieve [$norm_lo, $norm_hi) -> solve --start-distance $start_dist --angular $WEDGES"
            # Simulate: assume no moat for distances < 80M, moat for >= 81M
            # (only for dry run planning)
            current_dist=$((current_dist + band_width * BANDS_PER_ITER / (2 * current_dist)))
            if (( current_dist > MAX_DISTANCE )); then
                log "  [$tag] Dry run: reached max distance"
                break
            fi
            iter=$((iter + 1))
            continue
        fi

        # --- SIEVE ---
        local sieve_start sieve_end
        sieve_start=$(date +%s%N)

        "$CUDA_BIN" \
            --norm-lo "$norm_lo" \
            --norm-hi "$norm_hi" \
            --output "$gprf_file" \
            --mode sieve \
            --k-squared "$K_SQUARED" \
            2>> "$LOG_FILE"

        sieve_end=$(date +%s%N)
        local sieve_ms=$(( (sieve_end - sieve_start) / 1000000 ))

        local gprf_bytes gprf_primes
        gprf_bytes=$(stat -c%s "$gprf_file" 2>/dev/null || stat -f%z "$gprf_file" 2>/dev/null || echo 0)
        gprf_primes=$(( (gprf_bytes - 64) / 16 ))

        log "    Sieve: ${sieve_ms}ms, ${gprf_primes} primes"

        # --- SOLVER ---
        local solve_start solve_end
        solve_start=$(date +%s%N)

        local solver_output
        solver_output=$("$SOLVER_BIN" \
            --k-squared "$K_SQUARED" \
            --start-distance "$start_dist" \
            --angular "$WEDGES" \
            --prime-file "$gprf_file" \
            --profile 2>&1) || true

        solve_end=$(date +%s%N)
        local solve_ms=$(( (solve_end - solve_start) / 1000000 ))

        # Parse
        local farthest_dist farthest_norm farthest_point component_size primes_processed
        farthest_dist=$(echo "$solver_output" | grep -oP 'farthest distance: \K[0-9.]+' || echo "0")
        farthest_norm=$(echo "$solver_output" | grep -oP 'RESULT farthest_norm=\K[0-9]+' || echo "0")
        farthest_point=$(echo "$solver_output" | grep -oP 'farthest_point=\K\([^)]+\)' || echo "(0,0)")
        component_size=$(echo "$solver_output" | grep -oP 'origin component size: \K[0-9]+' || echo "0")
        primes_processed=$(echo "$solver_output" | grep -oP 'primes processed: \K[0-9]+' || echo "0")

        local moat_status="no"
        if echo "$solver_output" | grep -q "MOAT_FOUND"; then
            moat_status="yes"
        elif [[ "$component_size" -gt 0 && "$primes_processed" -gt 0 && "$component_size" -lt "$primes_processed" ]]; then
            # Component didn't absorb all primes. Check: did farthest advance
            # significantly from start, or did it stall well before the file edge?
            local farthest_int
            farthest_int=$(python3 -c "print(int(float('$farthest_dist')))")
            local file_edge_dist
            file_edge_dist=$(python3 -c "import math; print(int(math.sqrt($norm_hi)))")

            # If farthest is more than 10% back from file edge, likely real termination
            local edge_buffer
            edge_buffer=$(python3 -c "print(int(0.9 * $file_edge_dist))")

            if [[ "$farthest_int" -lt "$edge_buffer" ]]; then
                moat_status="yes"
                log "    MOAT DETECTED: farthest=$farthest_dist (${farthest_int}) << file_edge=$file_edge_dist"
            else
                moat_status="edge"
            fi
        else
            moat_status="edge"
        fi

        local farthest_int_for_advance
        farthest_int_for_advance=$(python3 -c "print(int(float('$farthest_dist')))")

        log "    Solver: ${solve_ms}ms, farthest=${farthest_dist}, component=${component_size}/${primes_processed}, moat=${moat_status}"

        # Write JSONL
        echo "{\"start_distance\":${start_dist},\"iteration\":${iter},\"sieve_distance\":${current_dist},\"norm_lo\":${norm_lo},\"norm_hi\":${norm_hi},\"primes\":${gprf_primes},\"sieve_ms\":${sieve_ms},\"solve_ms\":${solve_ms},\"farthest_dist\":${farthest_dist},\"farthest_norm\":${farthest_norm},\"farthest_point\":\"${farthest_point}\",\"component_size\":${component_size},\"total_primes\":${primes_processed},\"moat\":\"${moat_status}\",\"k_squared\":${K_SQUARED},\"wedges\":${WEDGES},\"tag\":\"${tag}\"}" >> "$RESULTS_JSONL"

        # Cleanup
        rm -f "$gprf_file"

        if [[ "$moat_status" == "yes" ]]; then
            PROG_MOAT_FOUND="yes"
            PROG_FARTHEST_DIST=$farthest_int_for_advance
            log "  [$tag] MOAT FOUND at D=$start_dist: farthest=$farthest_dist, component=$component_size"
            return
        fi

        # Advance strategy: use farthest distance as the new start_distance
        # for the NEXT UB probe. This walks forward through the moat region.
        # Each iteration is an independent UB probe at a new (higher) distance.
        #
        # If farthest > current sieve distance, the component extended forward.
        # The new start_distance should be farthest - safety_margin to ensure
        # the next probe overlaps.
        #
        # If farthest didn't advance (component stuck), advance by the
        # approximate shell width in distance units.
        if [[ "$farthest_int_for_advance" -gt "$current_dist" ]]; then
            local safety_margin=$((K_INT * 2))
            current_dist=$((farthest_int_for_advance - safety_margin))
            # Also update start_dist for the solver
            start_dist=$current_dist
            log "    Advancing: new start_distance=$current_dist (farthest=$farthest_int_for_advance - margin=$safety_margin)"
        else
            local shell_dist
            shell_dist=$(python3 -c "
import math
bw = $band_width * $BANDS_PER_ITER
D = max($current_dist, 1)
print(max(10, int(bw / (2 * D))))
")
            current_dist=$((current_dist + shell_dist))
            start_dist=$current_dist
            log "    Warning: no advance. Stepping forward by $shell_dist to start_distance=$current_dist"
        fi

        iter=$((iter + 1))
    done

    PROG_FARTHEST_DIST=$(python3 -c "print(int(float('${farthest_dist:-0}')))" 2>/dev/null || echo 0)
    log "  [$tag] Reached iteration limit ($MAX_ITERATIONS) without moat. Farthest: $PROG_FARTHEST_DIST"
}

# ---------------------------------------------------------------------------
# Mode: PROGRESSIVE
# ---------------------------------------------------------------------------
run_mode_progressive() {
    log ""
    log "=== MODE: PROGRESSIVE ==="
    log "  Start distance: $START_DISTANCE"
    log "  Bands per iteration: $BANDS_PER_ITER"

    run_progressive "$START_DISTANCE" "PROG"

    if [[ "$PROG_MOAT_FOUND" == "yes" ]]; then
        CAMPAIGN_RESULT="moat_found"
        CAMPAIGN_BOUNDARY=$PROG_FARTHEST_DIST
    else
        CAMPAIGN_RESULT="no_moat_in_range"
        CAMPAIGN_BOUNDARY=$PROG_FARTHEST_DIST
    fi
}

# ---------------------------------------------------------------------------
# Mode: BISECT
# ---------------------------------------------------------------------------
# Geometric doubling to find first moat distance, then binary search.
run_mode_bisect() {
    log ""
    log "=== MODE: BISECT ==="
    log "  Start distance: $START_DISTANCE"
    log "  Max distance: $MAX_DISTANCE"
    log "  Bisect tolerance: $BISECT_TOLERANCE"

    # Phase 1: Geometric doubling to find bracket
    local lo=0
    local hi=0
    local dist=$START_DISTANCE

    log ""
    log "--- Phase 1: LOCATE (geometric doubling) ---"

    while (( dist <= MAX_DISTANCE )); do
        log "  Trying D=$dist..."

        run_progressive "$dist" "LOCATE"

        if [[ "$PROG_MOAT_FOUND" == "yes" ]]; then
            hi=$dist
            log "  LOCATE: Moat found at D=$dist"
            break
        else
            lo=$dist
            log "  LOCATE: No moat at D=$dist, doubling..."
        fi

        dist=$((dist * 2))
    done

    if [[ $hi -eq 0 ]]; then
        log "  LOCATE: No moat found up to max_distance=$MAX_DISTANCE"
        CAMPAIGN_RESULT="no_moat_in_range"
        CAMPAIGN_BOUNDARY=$PROG_FARTHEST_DIST
        return
    fi

    if [[ $lo -eq 0 ]]; then
        lo=$((hi / 2))
        (( lo < 1 )) && lo=1
    fi

    # Phase 2: Binary search
    log ""
    log "--- Phase 2: REFINE (binary search) ---"
    log "  Range: [$lo, $hi], tolerance: $BISECT_TOLERANCE"

    while (( hi - lo > BISECT_TOLERANCE )); do
        local mid=$(( (lo + hi) / 2 ))
        log "  Trying D=$mid..."

        run_progressive "$mid" "REFINE"

        if [[ "$PROG_MOAT_FOUND" == "yes" ]]; then
            hi=$mid
            log "  REFINE: Moat at D=$mid -> range [$lo, $hi]"
        else
            lo=$mid
            log "  REFINE: No moat at D=$mid -> range [$lo, $hi]"
        fi
    done

    log "  REFINE complete: boundary in [$lo, $hi]"
    CAMPAIGN_RESULT="boundary_found"
    CAMPAIGN_BOUNDARY=$(( (lo + hi) / 2 ))
}

# ---------------------------------------------------------------------------
# Campaign summary
# ---------------------------------------------------------------------------
print_summary() {
    log ""
    log "=========================================="
    log "=== UB Campaign Summary ==="
    log "=========================================="
    log "  k^2 = $K_SQUARED (k = $K_FLOAT)"
    log "  Mode: $MODE"
    log "  Start distance: $START_DISTANCE"
    log "  Max distance: $MAX_DISTANCE"
    log "  Result: $CAMPAIGN_RESULT"
    log "  Boundary/farthest: $CAMPAIGN_BOUNDARY"
    log "  Bands per iteration: $BANDS_PER_ITER"
    log "  Wedges: $WEDGES"
    log "  Log: $LOG_FILE"
    log "  Results: $RESULTS_JSONL"
    log "=========================================="

    if [[ -f "$RESULTS_JSONL" ]]; then
        local total_entries total_sieve_ms total_solve_ms
        total_entries=$(wc -l < "$RESULTS_JSONL" 2>/dev/null || echo 0)
        total_sieve_ms=$(python3 -c "
import json
total = 0
for line in open('$RESULTS_JSONL'):
    try:
        d = json.loads(line)
        total += d.get('sieve_ms', 0)
    except: pass
print(total)
" 2>/dev/null || echo 0)
        total_solve_ms=$(python3 -c "
import json
total = 0
for line in open('$RESULTS_JSONL'):
    try:
        d = json.loads(line)
        total += d.get('solve_ms', 0)
    except: pass
print(total)
" 2>/dev/null || echo 0)

        log "  Total iterations: $total_entries"
        log "  Total sieve time: ${total_sieve_ms}ms ($((total_sieve_ms / 1000))s)"
        log "  Total solve time: ${total_solve_ms}ms ($((total_solve_ms / 1000))s)"
        log "  Total wall time: $((total_sieve_ms + total_solve_ms))ms ($((( total_sieve_ms + total_solve_ms) / 1000))s)"
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
CAMPAIGN_RESULT=""
CAMPAIGN_BOUNDARY=0

log "=== UB Campaign: k^2=$K_SQUARED, start=$START_DISTANCE, max=$MAX_DISTANCE ==="
log "  mode=$MODE bands=$BANDS_PER_ITER wedges=$WEDGES"
log "  cuda=$CUDA_BIN"
log "  solver=$SOLVER_BIN"
log "  work_dir=$WORK_DIR"
log "  dry_run=$DRY_RUN"

if [[ "$DRY_RUN" == true ]]; then
    log ""
    log "=== DRY RUN MODE ==="
    log "No binaries will be executed."
fi

case "$MODE" in
    progressive) run_mode_progressive ;;
    bisect)      run_mode_bisect ;;
    *)
        echo "Error: unknown mode '$MODE'. Use 'progressive' or 'bisect'." >&2
        exit 1
        ;;
esac

print_summary
