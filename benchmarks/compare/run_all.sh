#!/usr/bin/env bash
# The full C++ vs Verus comparison at SF20, one experiment after another:
# read-only (all 22), concurrent readers + writer (all 22: base, cpp, verus),
# multi-accelerator (one process).  Each runs even if an earlier one failed.
#   bash benchmarks/compare/run_all.sh [out_dir]
# Env: DB, WORK (see run_compare.py), READERS (8), RUN_SECONDS (60), WPS (5),
#      MULTI_SECONDS (120).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
OUT="${1:-${ROOT}/benchmarks/compare/results/final_$(date +%Y%m%d_%H%M%S)}"
READERS="${READERS:-8}" RUN_SECONDS="${RUN_SECONDS:-60}" WPS="${WPS:-5}" MULTI_SECONDS="${MULTI_SECONDS:-120}"
mkdir -p "${OUT}"
echo "out=${OUT} readers=${READERS} seconds=${RUN_SECONDS} wps=${WPS} multi_seconds=${MULTI_SECONDS} start=$(date)" | tee "${OUT}/run_all.log"

run() {
  local name="$1"; shift
  echo "=== ${name} start $(date +%T)" | tee -a "${OUT}/run_all.log"
  python3 benchmarks/compare/run_compare.py "$@" 2>&1 | tee "${OUT}/${name}.log"
  echo "=== ${name} exit ${PIPESTATUS[0]} $(date +%T)" | tee -a "${OUT}/run_all.log"
}

run read read --out "${OUT}/read"
run concurrent concurrent --readers "${READERS}" --seconds "${RUN_SECONDS}" --wps "${WPS}" --out "${OUT}/concurrent"
run multi multi --readers "${READERS}" --seconds "${MULTI_SECONDS}" --wps "${WPS}" --out "${OUT}/multi"
echo "DONE $(date)" | tee -a "${OUT}/run_all.log"
python3 benchmarks/compare/plot_results.py "${OUT}" 2>&1 | tee -a "${OUT}/run_all.log"
touch "${OUT}/DONE"
