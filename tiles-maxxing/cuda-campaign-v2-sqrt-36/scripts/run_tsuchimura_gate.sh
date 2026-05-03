#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_tsuchimura_gate.sh [options]

Runs the two known-answer K36 full-octant cases through campaign_main_cuda:
  R_inner=80000000 R_outer=80015782 => SPANNING
  R_inner=80000000 R_outer=80015790 => MOAT

Options:
  --cuda-bin PATH          CUDA campaign_main_cuda executable
  --chunk-size N           CUDA campaign chunk size
  --timing                 Print script-level elapsed time; forwarded if supported
  --profile-dir PATH       Directory for per-case logs and profile JSON
  --work-dir PATH          Directory for generated logs
  --keep                   Keep auto-created work directory
  -h, --help               Show this help

Environment overrides:
  CUDA_CAMPAIGN_BIN
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 2
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

k_sq="36"
r_inner="80000000"
region="full-octant"
cuda_bin="${CUDA_CAMPAIGN_BIN:-}"
chunk_size="200000"
timing=0
profile_dir=""
work_dir=""
keep=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --cuda-bin)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      cuda_bin="$2"
      shift 2
      ;;
    --chunk-size)
      [[ $# -ge 2 ]] || die "$1 needs a value"
      chunk_size="$2"
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

if [[ -z "$cuda_bin" ]]; then
  cuda_bin="$(first_executable \
    "$repo_root/build-k${k_sq}/campaign_main_cuda" \
    "$repo_root/build/campaign_main_cuda" \
    "$repo_root/build-k36/campaign_main_cuda" || true)"
fi
is_executable "$cuda_bin" || die "CUDA campaign_main_cuda not found; pass --cuda-bin"

auto_work_dir=0
if [[ -z "$work_dir" ]]; then
  base_dir="${AGENT_MUX_ARTIFACT_DIR:-${TMPDIR:-/tmp}}"
  work_dir="$(mktemp -d "$base_dir/tsuchimura-gate.XXXXXX")"
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

run_case() {
  local label="$1"
  local r_outer="$2"
  local expected="$3"
  local full_mode="${4:-0}"
  local log="$work_dir/${label}.log"
  local profile="$work_dir/${label}.profile.json"
  local output rc start_time end_time elapsed verdict
  local cmd=(
    "$cuda_bin"
    "--k-sq=$k_sq"
    "--r-inner=$r_inner"
    "--r-outer=$r_outer"
    --region "$region"
    "--chunk-size=$chunk_size"
  )

  if [[ "$full_mode" -eq 1 ]]; then
    cmd+=(--no-early-exit)
  fi
  if [[ "$timing" -eq 1 ]] && supports_flag "$cuda_bin" "--timing"; then
    cmd+=(--timing)
  fi

  if [[ -n "$profile_dir" ]]; then
    log="$profile_dir/${label}.log"
    profile="$profile_dir/${label}.profile.json"
  fi
  if [[ -n "$profile_dir" ]] && supports_flag "$cuda_bin" "--profile"; then
    cmd+=(--profile "$profile")
  fi

  echo "tsuchimura-gate: running $label R_outer=$r_outer expected=$expected"
  start_time="$(date +%s)"
  set +e
  output="$("${cmd[@]}" 2>&1)"
  rc=$?
  set -e
  end_time="$(date +%s)"
  printf '%s\n' "$output" >"$log"
  printf '%s\n' "$output"

  if [[ "$rc" -ne 0 ]]; then
    die "$label failed with exit code $rc; log: $log"
  fi

  verdict="$(extract_verdict "$output")" ||
    die "$label did not print a VERDICT line; log: $log"
  [[ "$verdict" == "$expected" ]] ||
    die "$label verdict mismatch: expected $expected, got $verdict; log: $log"

  require_zero_overflows "$output"

  if [[ "$timing" -eq 1 ]]; then
    elapsed=$((end_time - start_time))
    echo "tsuchimura-gate: $label elapsed=${elapsed}s"
  fi
  echo "tsuchimura-gate: PASS $label verdict=$verdict overflows=0"
}

echo "tsuchimura-gate: K=$k_sq R_inner=$r_inner region=$region chunk_size=$chunk_size"
echo "tsuchimura-gate: work_dir=$work_dir"
run_case "R80015782_spanning_early" "80015782" "SPANNING" 0
run_case "R80015782_spanning_full" "80015782" "SPANNING" 1
run_case "R80015790_moat" "80015790" "MOAT" 1
echo "tsuchimura-gate: PASS"
