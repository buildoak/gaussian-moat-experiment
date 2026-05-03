#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_k34_regression_gate.sh [options]

Runs the honest K34 cross-K regression gate:
  1. K34 CPU/CUDA snapshot SHA smoke.
  2. K34 cuda_vs_cpu_diff M4+K5 parity on the Tsuchimura-scale shell.
  3. K34 Tsuchimura-scale shell sentinel:
       R_inner=24289452 R_outer=24297644 => observed SPANNING, zero overflows.

This is NOT an external Tsuchimura truth gate. Tsuchimura's K34 result bounds
the origin-connected component, while this campaign checks annular spanning.
See reference/sqrt34-gate-feasibility.md.

Options:
  --cpu-bin PATH           CPU campaign_main executable compiled with K_SQ=34
  --cuda-bin PATH          CUDA campaign_main_cuda executable compiled with K_SQ=34
  --diff-bin PATH          cuda_vs_cpu_diff executable compiled with K_SQ=34
  --chunk-size N           CUDA campaign chunk size for shell sentinel
  --smoke-chunk-size N     CUDA chunk size for snapshot smoke
  --timing                 Print script-level elapsed time; forwarded if supported
  --profile-dir PATH       Directory for logs and profile JSON
  --work-dir PATH          Directory for generated logs
  --keep                   Keep auto-created work directory
  -h, --help               Show this help

Environment overrides:
  CPU_CAMPAIGN_BIN, CUDA_CAMPAIGN_BIN, CUDA_VS_CPU_DIFF_BIN
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 2
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

k_sq="34"
r_inner="24289452"
r_outer="24297644"
region="full-octant"
cpu_bin="${CPU_CAMPAIGN_BIN:-}"
cuda_bin="${CUDA_CAMPAIGN_BIN:-}"
diff_bin="${CUDA_VS_CPU_DIFF_BIN:-}"
chunk_size="200000"
smoke_chunk_size="64"
timing=0
profile_dir=""
work_dir=""
keep=0

while [[ $# -gt 0 ]]; do
  case "$1" in
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
    --chunk-size)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      chunk_size="$2"
      shift 2
      ;;
    --smoke-chunk-size)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      smoke_chunk_size="$2"
      shift 2
      ;;
    --timing)
      timing=1
      shift
      ;;
    --profile-dir)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      profile_dir="$2"
      shift 2
      ;;
    --work-dir)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      work_dir="$2"
      shift 2
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

extract_counter() {
  local output="$1"
  local counter="$2"
  printf '%s\n' "$output" | awk -v name="${counter}:" '
    $1 == name { print $2; found = 1 }
    END { if (!found) exit 1 }
  '
}

extract_verdict() {
  local output="$1"
  printf '%s\n' "$output" | awk '
    $1 == "VERDICT:" { verdict = $2; found = 1 }
    END { if (found) print verdict; else exit 1 }
  '
}

require_zero_overflows() {
  local output="$1"
  local counter value
  for counter in \
      k1_cand_overflow_count \
      k4_prime_overflow_count \
      k4_group_overflow_count \
      k5_port_overflow_count; do
    value="$(extract_counter "$output" "$counter")" ||
      die "missing overflow counter in CUDA output: $counter"
    [[ "$value" == "0" ]] ||
      die "overflow-contaminated result: $counter=$value"
  done
}

if [[ -z "$cpu_bin" ]]; then
  cpu_bin="$(first_executable \
    "$repo_root/../cpp-campaign-v2/build-k${k_sq}/campaign_main" \
    "$repo_root/../cpp-campaign-v2/build-k34/campaign_main" || true)"
fi
if [[ -z "$cuda_bin" ]]; then
  cuda_bin="$(first_executable \
    "$repo_root/build-k${k_sq}/campaign_main_cuda" \
    "$repo_root/build-k34/campaign_main_cuda" || true)"
fi
if [[ -z "$diff_bin" ]]; then
  diff_bin="$(first_executable \
    "$repo_root/build-k${k_sq}/cuda_vs_cpu_diff" \
    "$repo_root/build-k34/cuda_vs_cpu_diff" || true)"
fi

is_executable "$cpu_bin" || die "K34 CPU campaign_main not found; pass --cpu-bin"
is_executable "$cuda_bin" || die "K34 CUDA campaign_main_cuda not found; pass --cuda-bin"
is_executable "$diff_bin" || die "K34 cuda_vs_cpu_diff not found; pass --diff-bin"

auto_work_dir=0
if [[ -z "$work_dir" ]]; then
  base_dir="${AGENT_MUX_ARTIFACT_DIR:-${TMPDIR:-/tmp}}"
  work_dir="$(mktemp -d "$base_dir/k34-regression-gate.XXXXXX")"
  auto_work_dir=1
else
  mkdir -p "$work_dir"
fi

if [[ -n "$profile_dir" ]]; then
  mkdir -p "$profile_dir"
fi

cleanup() {
  if [[ "$auto_work_dir" -eq 1 && "$keep" -eq 0 ]]; then
    rm -rf "$work_dir"
  fi
}
trap cleanup EXIT

echo "k34-regression-gate: K=$k_sq R_inner=$r_inner R_outer=$r_outer region=$region chunk_size=$chunk_size"
echo "k34-regression-gate: work_dir=$work_dir"

start_time="$(date +%s)"

echo "k34-regression-gate: snapshot SHA smoke"
"$repo_root/scripts/run_snapshot_sha_gate.sh" \
  --smoke \
  --k "$k_sq" \
  --chunk-size "$smoke_chunk_size" \
  --cpu-bin "$cpu_bin" \
  --cuda-bin "$cuda_bin" \
  --work-dir "$work_dir/snapshot-smoke"

echo "k34-regression-gate: cuda_vs_cpu_diff M4+K5 parity"
"$diff_bin" \
  --r-inner "$r_inner" \
  --r-outer "$r_outer" \
  --m4 \
  --verbose \
  --limit 16 | tee "$work_dir/cuda_vs_cpu_diff.log"
"$diff_bin" \
  --r-inner "$r_inner" \
  --r-outer "$r_outer" \
  --k5 \
  --verbose \
  --limit 16 | tee "$work_dir/cuda_vs_cpu_diff_k5.log"

log="$work_dir/R24289452_shell_spanning.log"
profile="$work_dir/R24289452_shell_spanning.profile.json"
if [[ -n "$profile_dir" ]]; then
  log="$profile_dir/R24289452_shell_spanning.log"
  profile="$profile_dir/R24289452_shell_spanning.profile.json"
fi

cmd=(
  "$cuda_bin"
  "--k-sq=$k_sq"
  "--r-inner=$r_inner"
  "--r-outer=$r_outer"
  --region "$region"
  "--chunk-size=$chunk_size"
  --no-early-exit
)
if [[ "$timing" -eq 1 ]] && supports_flag "$cuda_bin" "--timing"; then
  cmd+=(--timing)
fi
if [[ -n "$profile_dir" ]] && supports_flag "$cuda_bin" "--profile"; then
  cmd+=(--profile "$profile")
fi

echo "k34-regression-gate: shell sentinel expected=SPANNING overflows=0"
set +e
output="$("${cmd[@]}" 2>&1)"
rc=$?
set -e
printf '%s\n' "$output" >"$log"
printf '%s\n' "$output"

if [[ "$rc" -ne 0 ]]; then
  die "K34 shell sentinel failed with exit code $rc; log: $log"
fi

verdict="$(extract_verdict "$output")" ||
  die "K34 shell sentinel did not print a VERDICT line; log: $log"
[[ "$verdict" == "SPANNING" ]] ||
  die "K34 shell sentinel changed: expected SPANNING, got $verdict; log: $log"
require_zero_overflows "$output"

if [[ "$timing" -eq 1 ]]; then
  end_time="$(date +%s)"
  echo "k34-regression-gate: elapsed=$((end_time - start_time))s"
fi
echo "k34-regression-gate: PASS"
