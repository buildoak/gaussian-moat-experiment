#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_snapshot_sha_gate.sh --smoke [options]
  scripts/run_snapshot_sha_gate.sh --full --r 80000000 --k 36 [options]

Options:
  --smoke                  Quick full-octant gate at small radius
  --full                   Ship gate defaults: R_outer = R + production delta
  --r N                    Alias for --r-inner N
  --r-inner N              Inner radius
  --r-outer N              Outer radius
  --k N                    K_SQ value
  --region SPEC            Region argument passed to both campaign mains
  --cpu-bin PATH           CPU campaign_main executable
  --cuda-bin PATH          CUDA campaign_main_cuda executable
  --diff-bin PATH          cuda_vs_cpu_diff executable
  --work-dir PATH          Directory for generated snapshots and helper binary
  --chunk-size N           CUDA campaign chunk size
  --threads N              CPU campaign --threads value
  --inject-mismatch        Append one byte to CUDA snapshot before comparison
  --keep                   Keep auto-created work directory
  -h, --help               Show this help

Environment overrides:
  CPU_CAMPAIGN_BIN, CUDA_CAMPAIGN_BIN, CUDA_VS_CPU_DIFF_BIN, CXX
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 2
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mode=""
k_sq="36"
r_inner=""
r_outer=""
region="full-octant"
cpu_bin="${CPU_CAMPAIGN_BIN:-}"
cuda_bin="${CUDA_CAMPAIGN_BIN:-}"
diff_bin="${CUDA_VS_CPU_DIFF_BIN:-}"
work_dir=""
chunk_size="64"
threads="1"
inject_mismatch=0
keep=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --smoke)
      mode="smoke"
      shift
      ;;
    --full)
      mode="full"
      shift
      ;;
    --r|--r-inner)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      r_inner="$2"
      shift 2
      ;;
    --r-outer)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      r_outer="$2"
      shift 2
      ;;
    --k)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      k_sq="$2"
      shift 2
      ;;
    --region)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      region="$2"
      shift 2
      ;;
    --cpu-bin)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      cpu_bin="$2"
      shift 2
      ;;
    --cuda-bin)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      cuda_bin="$2"
      shift 2
      ;;
    --diff-bin)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      diff_bin="$2"
      shift 2
      ;;
    --work-dir)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      work_dir="$2"
      shift 2
      ;;
    --chunk-size)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      chunk_size="$2"
      shift 2
      ;;
    --threads)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      threads="$2"
      shift 2
      ;;
    --inject-mismatch)
      inject_mismatch=1
      shift
      ;;
    --keep)
      keep=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
done

[[ -n "$mode" ]] || die "choose --smoke or --full"

production_delta() {
  case "$1" in
    36) echo 8192 ;;
    40) echo 10000 ;;
    *) echo 8192 ;;
  esac
}

if [[ "$mode" == "smoke" ]]; then
  : "${r_inner:=1000}"
  : "${r_outer:=1600}"
else
  : "${r_inner:=80000000}"
  if [[ -z "$r_outer" ]]; then
    r_outer=$((r_inner + $(production_delta "$k_sq")))
  fi
fi

is_executable() {
  [[ -n "$1" && -x "$1" ]]
}

first_executable() {
  local path
  for path in "$@"; do
    if [[ -x "$path" ]]; then
      printf '%s\n' "$path"
      return 0
    fi
  done
  return 1
}

supports_flag() {
  local bin="$1"
  local flag="$2"
  "$bin" --help 2>&1 | grep -Eq -- "(^|[[:space:]])${flag}([=[:space:]]|$)"
}

snapshot_flag_for() {
  local bin="$1"
  if supports_flag "$bin" "--snapshot-out"; then
    printf '%s\n' "--snapshot-out"
  else
    printf '%s\n' "--out"
  fi
}

if [[ -z "$cpu_bin" ]]; then
  cpu_bin="$(first_executable \
    "$repo_root/../cpp-campaign-v2/build-k${k_sq}/campaign_main" \
    "$repo_root/../cpp-campaign-v2/build-k${k_sq}-tests/campaign_main" \
    "$repo_root/../cpp-campaign-v2/build/campaign_main" \
    "$repo_root/../cpp-campaign-v2/build-k36/campaign_main" || true)"
fi
if [[ -z "$cuda_bin" ]]; then
  cuda_bin="$(first_executable \
    "$repo_root/build-k${k_sq}/campaign_main_cuda" \
    "$repo_root/build/campaign_main_cuda" \
    "$repo_root/build-k36/campaign_main_cuda" || true)"
fi
if [[ -z "$diff_bin" ]]; then
  diff_bin="$(first_executable \
    "$repo_root/build-k${k_sq}/cuda_vs_cpu_diff" \
    "$repo_root/build/cuda_vs_cpu_diff" \
    "$repo_root/build-k36/cuda_vs_cpu_diff" || true)"
fi

is_executable "$cpu_bin" || die "CPU campaign_main not found; pass --cpu-bin"
is_executable "$cuda_bin" || die "CUDA campaign_main_cuda not found; pass --cuda-bin"

auto_work_dir=0
if [[ -z "$work_dir" ]]; then
  base_dir="${AGENT_MUX_ARTIFACT_DIR:-${TMPDIR:-/tmp}}"
  work_dir="$(mktemp -d "$base_dir/snapshot-sha-gate.XXXXXX")"
  auto_work_dir=1
else
  mkdir -p "$work_dir"
fi

cleanup() {
  if [[ "$auto_work_dir" -eq 1 && "$keep" -eq 0 ]]; then
    rm -rf "$work_dir"
  fi
}
trap cleanup EXIT

sha_test="$work_dir/test_snapshot_sha_R80M"
cxx="${CXX:-c++}"
"$cxx" -std=c++17 -O2 "$repo_root/tests/test_snapshot_sha_R80M.cpp" -o "$sha_test"
"$sha_test" --self-test >/dev/null

cpu_snapshot="$work_dir/cpu.snapshot.bin"
cuda_snapshot="$work_dir/cuda.snapshot.bin"
cuda_snapshot_flag="$(snapshot_flag_for "$cuda_bin")"

echo "snapshot-sha-gate: mode=$mode K=$k_sq R_inner=$r_inner R_outer=$r_outer region=$region"
echo "snapshot-sha-gate: work_dir=$work_dir"

echo "snapshot-sha-gate: generating CPU snapshot"
"$cpu_bin" \
  --k-sq="$k_sq" \
  --r-inner="$r_inner" \
  --r-outer="$r_outer" \
  --region "$region" \
  --out "$cpu_snapshot" \
  --threads "$threads"

echo "snapshot-sha-gate: generating CUDA snapshot"
"$cuda_bin" \
  --k-sq="$k_sq" \
  --r-inner="$r_inner" \
  --r-outer="$r_outer" \
  --region "$region" \
  "$cuda_snapshot_flag" "$cuda_snapshot" \
  --chunk-size="$chunk_size" \
  --threads "$threads"

if [[ "$inject_mismatch" -eq 1 ]]; then
  printf '\001' >>"$cuda_snapshot"
  echo "snapshot-sha-gate: injected mismatch into $cuda_snapshot"
fi

set +e
"$sha_test" "$cpu_snapshot" "$cuda_snapshot"
compare_rc=$?
set -e

if [[ "$compare_rc" -eq 0 ]]; then
  echo "snapshot-sha-gate: PASS"
  exit 0
fi

echo "snapshot-sha-gate: FAIL" >&2
echo "snapshot-sha-gate: CPU snapshot:  $cpu_snapshot" >&2
echo "snapshot-sha-gate: CUDA snapshot: $cuda_snapshot" >&2

if is_executable "$diff_bin"; then
  if "$diff_bin" --self-test-parse --sample 2 >/dev/null 2>&1; then
    echo "snapshot-sha-gate: running sampled cuda_vs_cpu_diff --k5 --verbose for divergence" >&2
    "$diff_bin" --r-inner "$r_inner" --r-outer "$r_outer" --sample 32 --k5 --verbose || true
    echo "snapshot-sha-gate: running sampled cuda_vs_cpu_diff --m4 --verbose for divergence" >&2
    "$diff_bin" --r-inner "$r_inner" --r-outer "$r_outer" --sample 32 --m4 --verbose || true
  else
    echo "snapshot-sha-gate: running legacy cuda_vs_cpu_diff --verbose for first divergence" >&2
    "$diff_bin" --r-inner "$r_inner" --r-outer "$r_outer" --limit 16 --k5 --verbose || true
  fi
else
  echo "snapshot-sha-gate: cuda_vs_cpu_diff not found; skipping verbose divergence probe" >&2
fi

exit "$compare_rc"
