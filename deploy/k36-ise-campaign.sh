#!/usr/bin/env bash
set -euo pipefail

# k²=36 ISE Campaign — 2026-03-22
# Three concurrent runs probing moat region, below, and above.
# Each run uses RAYON_NUM_THREADS=2 so 3 runs × 2 = 6 threads (= 6 cores).
#
# Run from: ~/gaussian-moat-cuda/tile-probe/
# Usage: ./deploy/k36-ise-campaign.sh {moat|below|above|launch-all}
#
# Benchmark: ~75.8s/shell at 2 threads, 32 stripes, 2000² tiles.
# Run A (moat): 200 shells ≈ 4.2 hours
# Run B (below): 100 shells ≈ 2.1 hours
# Run C (above): 100 shells ≈ 2.1 hours

source "$HOME/.cargo/env"

ISE_BIN="./target/release/ise"
K_SQ=36
TILE=2000
STRIPES=32
KERNEL="scanline"
THREADS=2
RESULTS_DIR="research/results/k36-ise-campaign-$(date +%Y-%m-%d)"

mkdir -p "$RESULTS_DIR"

run_ise() {
    local name="$1" r_min="$2" r_max="$3"
    local json_file="$RESULTS_DIR/${name}.json"
    local csv_file="$RESULTS_DIR/${name}.csv"
    local log_file="$RESULTS_DIR/${name}.log"

    echo "[$(date -u)] Starting $name: k²=$K_SQ r=[$r_min, $r_max] tiles=${TILE}² stripes=$STRIPES threads=$THREADS"

    RAYON_NUM_THREADS=$THREADS "$ISE_BIN" \
        --k-squared "$K_SQ" \
        --r-min "$r_min" --r-max "$r_max" \
        --tile-size "$TILE" \
        --stripes "$STRIPES" \
        --kernel "$KERNEL" \
        --threads "$THREADS" \
        --trace --profile \
        --json-trace "$json_file" \
        --csv "$csv_file" \
        2>&1 | tee "$log_file"

    echo "[$(date -u)] Finished $name"
}

case "${1:-}" in
    moat)
        # Run A: Moat hunt — 400K band around 80M
        # 200 shells, ~4.2 hours at 2 threads
        run_ise "run-a-moat-80M" 79700000 80100000
        ;;
    below)
        # Run B: Below moat — 200K band at 70M (expect connected / high f(r))
        # 100 shells, ~2.1 hours at 2 threads
        run_ise "run-b-below-70M" 69900000 70100000
        ;;
    above)
        # Run C: Above moat — 200K band at 100M (expect disconnected / low f(r))
        # 100 shells, ~2.1 hours at 2 threads
        run_ise "run-c-above-100M" 99900000 100100000
        ;;
    launch-all)
        # Launch all three runs in separate tmux sessions
        SCRIPT_PATH="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
        tmux new-session -d -s k36-moat  "cd $(pwd) && $SCRIPT_PATH moat;  echo 'DONE — press Enter'; read"
        tmux new-session -d -s k36-below "cd $(pwd) && $SCRIPT_PATH below; echo 'DONE — press Enter'; read"
        tmux new-session -d -s k36-above "cd $(pwd) && $SCRIPT_PATH above; echo 'DONE — press Enter'; read"
        echo "Launched 3 tmux sessions:"
        echo "  k36-moat  — Run A: moat hunt [79.7M, 80.1M] ~4.2h"
        echo "  k36-below — Run B: below moat [69.9M, 70.1M] ~2.1h"
        echo "  k36-above — Run C: above moat [99.9M, 100.1M] ~2.1h"
        echo ""
        echo "Monitor: tmux attach -t k36-moat"
        echo "Check:   tmux ls | grep k36"
        ;;
    status)
        # Quick status check
        for sess in k36-moat k36-below k36-above; do
            if tmux has-session -t "$sess" 2>/dev/null; then
                echo "$sess: RUNNING"
                # Show last trace line
                tmux capture-pane -t "$sess" -p | grep "^trace" | tail -1
            else
                echo "$sess: NOT RUNNING"
            fi
        done
        ;;
    *)
        echo "Usage: $0 {moat|below|above|launch-all|status}"
        echo ""
        echo "Individual runs:"
        echo "  moat   — Run A: 400K band around R=80M (moat hunt)"
        echo "  below  — Run B: 200K band at R=70M (baseline, expect connected)"
        echo "  above  — Run C: 200K band at R=100M (expect disconnected)"
        echo ""
        echo "Orchestration:"
        echo "  launch-all — Start all 3 runs in tmux sessions (2 threads each)"
        echo "  status     — Check running status of all tmux sessions"
        exit 1
        ;;
esac
