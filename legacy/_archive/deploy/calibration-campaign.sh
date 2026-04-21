#!/usr/bin/env bash
set -euo pipefail

# ISE Calibration Campaign — 2026-03-22
# Full calibration sweep across k²=2, 26, 32, 36
# Validates against known Tsuchimura moats + probes k²=36 transition
#
# Run from: ~/gaussian-moat-cuda/

ISE="./tile-probe/target/release/ise"
DATE=$(date +%Y-%m-%d)
RESULTS="research/results/calibration-${DATE}"
mkdir -p "$RESULTS"

# Common runner: name k² rmin rmax tile [stripes]
run_ise() {
    local name="$1" ksq="$2" rmin="$3" rmax="$4" tile="$5" stripes="${6:-32}"
    local json="$RESULTS/${name}.json"
    local csv="$RESULTS/${name}.csv"
    echo "[$(date '+%H:%M:%S')] Starting $name: k²=$ksq r=[$rmin, $rmax] tile=$tile stripes=$stripes"
    "$ISE" --k-squared "$ksq" \
        --r-min "$rmin" --r-max "$rmax" \
        --tile-size "$tile" \
        --stripes "$stripes" \
        --threads 0 \
        --json-trace "$json" \
        --csv "$csv" \
        --trace \
        2>"$RESULTS/${name}.log"
    echo "[$(date '+%H:%M:%S')] Finished $name → $csv"
}

case "${1:-all}" in
    k2)
        run_ise "k2-moat" 2 0 100 8 8
        ;;
    k26)
        # Dense sweep through known Tsuchimura moat at R≈1,015,639
        run_ise "k26-moat-dense" 26 950000 1100000 500 32
        # Control: well below moat (expect high f(r), zero candidates)
        run_ise "k26-control-below" 26 500000 550000 500 32
        ;;
    k32)
        # Dense sweep through known Tsuchimura moat at R≈2,823,055
        run_ise "k32-moat-dense" 32 2700000 2900000 500 32
        # Control: well below moat
        run_ise "k32-control-below" 32 1500000 1550000 500 32
        ;;
    k36)
        # Dense sweep around the transition zone (79.7M-80.1M)
        run_ise "k36-moat-dense" 36 79500000 80500000 2000 32
        # Control: well below (should be connected)
        run_ise "k36-control-50M" 36 49900000 50100000 2000 32
        ;;
    tile-height)
        # Tile height sensitivity test at k²=26 known moat
        # Does tile height affect moat detection precision?
        run_ise "k26-tile200" 26 1000000 1030000 200 32
        run_ise "k26-tile500" 26 1000000 1030000 500 32
        run_ise "k26-tile1000" 26 1000000 1030000 1000 32
        run_ise "k26-tile2000" 26 1000000 1030000 2000 32
        ;;
    all)
        "$0" k2
        "$0" tile-height
        "$0" k26
        "$0" k32
        "$0" k36
        ;;
    *)
        echo "Usage: $0 {k2|k26|k32|k36|tile-height|all}"
        exit 1
        ;;
esac

echo ""
echo "=== Campaign complete ==="
echo "Results in: $RESULTS/"
ls -la "$RESULTS/"
