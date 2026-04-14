#!/usr/bin/env bash
# K_SQ=36 post-fix sweep — depth-6 face extraction fix (46d73db) + diagonal fix (cf2d0e6)
# RTX 4090, sm_89, MAX_PRIMES_GPU=4096, burst=56000
set -euo pipefail

CAMPAIGN_DIR="/workspace/gaussian-moat-cuda/tiles-maxxing/campaign-sqrt-36/tiles-compositor/build-k36"
RESULTS_DIR="/workspace/k36-postfix-sweep"
RESULTS_JSONL="$RESULTS_DIR/sweep-results.jsonl"
LOG_DIR="$RESULTS_DIR/logs"

mkdir -p "$LOG_DIR"

# Phase 1: Baseline (SPANNING expected)
# Phase 2: Transition zone (dense, 1M steps)
# Phase 3: MOAT stability (dense around 80M)
R_VALUES=(
    # Phase 1 — baseline
    50000000 55000000 58000000
    # Phase 2 — transition zone
    59000000 60000000 61000000 62000000 63000000
    64000000 65000000 66000000 67000000 68000000
    69000000 70000000 71000000 72000000 73000000
    # Phase 3 — dense around 80M
    75000000 76000000 77000000 78000000 79000000
    80000000 81000000 82000000 83000000 84000000
    85000000 90000000 100000000 150000000 200000000
)

TOTAL=${#R_VALUES[@]}
echo "=== K36 Post-Fix Sweep ==="
echo "Fixes: depth-6 face extraction (46d73db) + diagonal grid gap (cf2d0e6)"
echo "Total runs: $TOTAL"
echo "Results: $RESULTS_JSONL"
echo "Started: $(date -u)"
echo ""

cd "$CAMPAIGN_DIR"

for i in "${!R_VALUES[@]}"; do
    R=${R_VALUES[$i]}
    RUN_NUM=$((i + 1))
    R_LABEL=$((R / 1000000))M

    echo "[$RUN_NUM/$TOTAL] R=${R_LABEL} — $(date -u +%H:%M:%S)"

    ./campaign "$R" --k-sq 36 --cuda-stream \
        --burst-size 56000 --progress-interval 5000 \
        > "$LOG_DIR/campaign-R${R}.log" 2>&1 || true

    # Extract JSON from log file
    if python3 -c "
import json, sys
data = open(sys.argv[1]).read()
s = data.rfind('{')
e = data.rfind('}') + 1
if s >= 0 and e > s:
    obj = json.loads(data[s:e])
    print(json.dumps(obj))
    sys.exit(0)
sys.exit(1)
" "$LOG_DIR/campaign-R${R}.log" >> "$RESULTS_JSONL" 2>/dev/null; then
        VERDICT=$(tail -1 "$RESULTS_JSONL" | python3 -c "import sys,json; print(json.load(sys.stdin)['verdict'])" 2>/dev/null || echo "UNKNOWN")
        WALL=$(tail -1 "$RESULTS_JSONL" | python3 -c "import sys,json; print(f\"{json.load(sys.stdin)['wall_time_seconds']:.0f}s\")" 2>/dev/null || echo "?s")
        echo "  → $VERDICT ($WALL)"
    else
        echo "  → ERROR (no JSON in output)"
        echo "{\"R\": $R, \"K_SQ\": 36, \"verdict\": \"ERROR\", \"error\": \"no JSON output\"}" >> "$RESULTS_JSONL"
    fi
done

echo ""
echo "=== Sweep Complete ==="
echo "Finished: $(date -u)"
echo "Results: $RESULTS_JSONL"
echo ""
echo "--- Summary ---"
python3 -c "
import json
with open('$RESULTS_JSONL') as f:
    runs = [json.loads(l) for l in f]
spanning = [r for r in runs if r['verdict'] == 'SPANNING']
moat = [r for r in runs if r['verdict'] == 'MOAT']
error = [r for r in runs if r['verdict'] == 'ERROR']
print(f'SPANNING: {len(spanning)} runs')
for r in spanning:
    print(f'  R={r[\"R\"]//1000000}M  towers={r.get(\"towers_processed\",\"?\")}')
print(f'MOAT: {len(moat)} runs')
if moat:
    rvals = sorted([r['R'] for r in moat])
    print(f'  First MOAT: R={rvals[0]//1000000}M')
    print(f'  Last MOAT:  R={rvals[-1]//1000000}M')
if error:
    print(f'ERROR: {len(error)} runs')
    for r in error:
        print(f'  R={r[\"R\"]//1000000}M')
total_time = sum(r.get('wall_time_seconds', 0) for r in runs)
print(f'Total wall time: {total_time:.0f}s ({total_time/60:.1f}min)')
"
