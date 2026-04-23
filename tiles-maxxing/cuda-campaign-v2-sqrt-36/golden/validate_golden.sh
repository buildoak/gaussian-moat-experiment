#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: validate_golden.sh <batch-name|all> [build-dir]

Examples:
  golden/validate_golden.sh k36-small-r10m build-k36
  golden/validate_golden.sh k40-r100m build-k40
  golden/validate_golden.sh all
USAGE
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAMPAIGN_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BATCH="$1"

default_build_dir() {
  local batch="$1"
  case "$batch" in
    k40-*) printf '%s\n' "$CAMPAIGN_DIR/build-k40" ;;
    *) printf '%s\n' "$CAMPAIGN_DIR/build-k36" ;;
  esac
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

validate_one() {
  local batch="$1"
  local build_dir="${2:-$(default_build_dir "$batch")}"
  local exe="$build_dir/cuda_golden_dump"
  local expected="$SCRIPT_DIR/$batch.json"

  if [[ ! -x "$exe" ]]; then
    echo "missing executable: $exe" >&2
    exit 1
  fi
  if [[ ! -f "$expected" ]]; then
    echo "missing golden file: $expected" >&2
    exit 1
  fi

  local tmp
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN

  "$exe" --batch "$batch" --out "$tmp" >/dev/null
  local actual="$tmp/$batch.json"
  if ! cmp -s "$expected" "$actual"; then
    echo "golden mismatch: $batch" >&2
    echo "expected sha256: $(sha256_file "$expected")" >&2
    echo "actual sha256:   $(sha256_file "$actual")" >&2
    diff -u "$expected" "$actual" | sed -n '1,120p' >&2 || true
    exit 1
  fi
  echo "OK $batch $(sha256_file "$expected")"
}

if [[ "$BATCH" == "all" ]]; then
  validate_one k36-small-r10m "${2:-}"
  validate_one k36-medium-r50m "${2:-}"
  validate_one k36-large-r85m "${2:-}"
  validate_one k36-edge-tsuchimura-r80015790 "${2:-}"
  validate_one k40-r100m "${2:-$(default_build_dir k40-r100m)}"
else
  validate_one "$BATCH" "${2:-}"
fi
