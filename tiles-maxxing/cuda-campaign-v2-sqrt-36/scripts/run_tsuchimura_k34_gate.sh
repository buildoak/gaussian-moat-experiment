#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/run_tsuchimura_k34_gate.sh [options]

Runs the Tsuchimura-derived K34 upper-bound full-octant MOAT check:
  K_SQ=34
  R_inner=24289452
  R_outer=24297644
  expected verdict: MOAT

This is not an exact two-sided boundary like the K36 gate. Tsuchimura reports
sqrt(34) finite with farthest distance < 24,289,452, so a shell outside that
bound should be a MOAT. The gate requires all overflow counters to be zero.

Options:
  --cuda-bin PATH          CUDA campaign_main_cuda executable compiled with K_SQ=34
  --chunk-size N           CUDA campaign chunk size
  --timing                 Print script-level elapsed time; forwarded if supported
  --profile-dir PATH       Directory for logs and profile JSON
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

k_sq="34"
r_inner="24289452"
r_outer="24297644"
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
    "$repo_root/build-k34/campaign_main_cuda" || true)"
fi
is_executable "$cuda_bin" || die "K34 CUDA campaign_main_cuda not found; pass --cuda-bin"

auto_work_dir=0
if [[ -z "$work_dir" ]]; then
  base_dir="${AGENT_MUX_ARTIFACT_DIR:-${TMPDIR:-/tmp}}"
  work_dir="$(mktemp -d "$base_dir/tsuchimura-k34-gate.XXXXXX")"
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

log="$work_dir/R24289452_moat.log"
profile="$work_dir/R24289452_moat.profile.json"
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
if [[ -n "$profile_dir" ]]; then
  log="$profile_dir/R24289452_moat.log"
  profile="$profile_dir/R24289452_moat.profile.json"
fi
if [[ -n "$profile_dir" ]] && supports_flag "$cuda_bin" "--profile"; then
  cmd+=(--profile "$profile")
fi

echo "tsuchimura-k34-gate: K=$k_sq R_inner=$r_inner R_outer=$r_outer region=$region chunk_size=$chunk_size"
echo "tsuchimura-k34-gate: work_dir=$work_dir"
echo "tsuchimura-k34-gate: running R24289452_moat expected=MOAT"
start_time="$(date +%s)"
set +e
output="$("${cmd[@]}" 2>&1)"
rc=$?
set -e
end_time="$(date +%s)"
printf '%s\n' "$output" >"$log"
printf '%s\n' "$output"

if [[ "$rc" -ne 0 ]]; then
  die "R24289452_moat failed with exit code $rc; log: $log"
fi

verdict="$(extract_verdict "$output")" ||
  die "R24289452_moat did not print a VERDICT line; log: $log"
[[ "$verdict" == "MOAT" ]] ||
  die "R24289452_moat verdict mismatch: expected MOAT, got $verdict; log: $log"

require_zero_overflows "$output"

if [[ "$timing" -eq 1 ]]; then
  elapsed=$((end_time - start_time))
  echo "tsuchimura-k34-gate: R24289452_moat elapsed=${elapsed}s"
fi
echo "tsuchimura-k34-gate: PASS R24289452_moat verdict=$verdict overflows=0"
echo "tsuchimura-k34-gate: PASS"
