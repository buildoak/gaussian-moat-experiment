#!/bin/bash
# sqrt(40) Gaussian Moat — Phase 1 Geometric Bracketing
# Each probe: single sieve + single solver call
# Answers: does the origin component survive past distance D?
set -euo pipefail

REPO=/root/gaussian-moat-cuda
SIEVE=$REPO/build/gm_cuda_primes
SOLVER=$REPO/solver/target/release/gaussian-moat-solver
LOG=$REPO/research/results/sqrt40-bracket-log.jsonl
PRIME_FILE=/tmp/ub-probe.gprf
K=7  # ceil(sqrt(40))
K2=40
WEDGES=8

mkdir -p "$(dirname $LOG)"
echo "=== sqrt(40) Phase 1 Bracket: $(date -u --iso-8601=seconds) ===" | tee -a $LOG.txt

# Session header
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader | tee -a $LOG.txt
nvcc --version | tail -1 | tee -a $LOG.txt
echo "sieve_sha256=$(sha256sum $SIEVE | cut -d' ' -f1)" | tee -a $LOG.txt
echo "solver_sha256=$(sha256sum $SOLVER | cut -d' ' -f1)" | tee -a $LOG.txt
echo "git_commit=$(cat $REPO/.git-commit 2>/dev/null || echo 'unavailable')" | tee -a $LOG.txt
echo "" | tee -a $LOG.txt

probe() {
    local D=$1
    local NORM_LO=$(python3 -c "print(($D - $K) * ($D - $K))")
    local NORM_HI=$(python3 -c "print(($D + $K) * ($D + $K))")

    echo "=== PROBE D=$D norm=[$NORM_LO, $NORM_HI] $(date -u --iso-8601=seconds) ===" | tee -a $LOG.txt

    # GPU memory before
    nvidia-smi --query-gpu=memory.used --format=csv,noheader | tee -a $LOG.txt

    # Sieve
    local T0=$(date +%s%N)
    timeout 600 $SIEVE \
        --norm-lo $NORM_LO --norm-hi $NORM_HI \
        --output $PRIME_FILE --mode sieve --k-squared $K2
    local T1=$(date +%s%N)
    local SIEVE_MS=$(( (T1 - T0) / 1000000 ))

    # GPU memory after sieve
    nvidia-smi --query-gpu=memory.used --format=csv,noheader | tee -a $LOG.txt

    # Count primes in file
    local PRIME_SIZE=$(stat --printf="%s" $PRIME_FILE 2>/dev/null || stat -f%z $PRIME_FILE)

    # Solver
    local T2=$(date +%s%N)
    timeout 1200 $SOLVER \
        --k-squared $K2 --start-distance $D --angular $WEDGES \
        --prime-file $PRIME_FILE --batch-size 1000000 \
        --norm-bound $NORM_HI --profile \
        2>&1 | tee -a $LOG.txt
    local T3=$(date +%s%N)
    local SOLVE_MS=$(( (T3 - T2) / 1000000 ))

    echo "TIMING: sieve=${SIEVE_MS}ms solver=${SOLVE_MS}ms total=$(( SIEVE_MS + SOLVE_MS ))ms" | tee -a $LOG.txt
    echo "" | tee -a $LOG.txt
}

# Phase 0: Validation — k²=36 at known UB boundary
echo "=== PHASE 0: k²=36 VALIDATION ===" | tee -a $LOG.txt
NORM_LO_36=$(python3 -c "print((85000000 - 7) * (85000000 - 7))")
NORM_HI_36=$(python3 -c "print((85000000 + 7) * (85000000 + 7))")
timeout 300 $SIEVE --norm-lo $NORM_LO_36 --norm-hi $NORM_HI_36 --output /tmp/validation.gprf --mode sieve --k-squared 36
timeout 300 $SOLVER --k-squared 36 --start-distance 85000000 --angular $WEDGES --prime-file /tmp/validation.gprf --norm-bound $NORM_HI_36 --profile 2>&1 | tee -a $LOG.txt
echo "" | tee -a $LOG.txt

# Phase 1: Geometric bracketing for k²=40
echo "=== PHASE 1: GEOMETRIC BRACKETING k²=40 ===" | tee -a $LOG.txt

# Start near ensemble median, then bracket outward
for D in 200000000 400000000 100000000 50000000 800000000; do
    probe $D
done

echo "=== PHASE 1 COMPLETE: $(date -u --iso-8601=seconds) ===" | tee -a $LOG.txt
echo "Results logged to: $LOG.txt" | tee -a $LOG.txt
echo "Attach: tmux attach -t sqrt40"
