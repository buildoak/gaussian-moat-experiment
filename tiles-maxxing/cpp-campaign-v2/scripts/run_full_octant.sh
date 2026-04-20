#!/usr/bin/env bash
# scripts/run_full_octant.sh
#
# Production invocation for cpp-campaign-v2 at project parameters.
# Phase 2 will wire this to emit snapshot + verdict; Phase 1 exercises
# grid enumeration only.

set -euo pipefail

: "${K_SQ:=36}"
: "${R_INNER:=80000000}"
: "${R_OUTER:=80008192}"
: "${BUILD_DIR:=build}"

cd "$(dirname "$0")/.."

if [[ ! -x "${BUILD_DIR}/campaign_main" ]]; then
  cmake -DK_SQ=${K_SQ} -S . -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" --target campaign_main
fi

"${BUILD_DIR}/campaign_main" \
  --k-sq=${K_SQ} \
  --r-inner=${R_INNER} \
  --r-outer=${R_OUTER} \
  --region full-octant
