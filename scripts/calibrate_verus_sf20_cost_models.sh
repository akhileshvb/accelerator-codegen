#!/usr/bin/env bash
# Calibrate the maintenance cost models of the TPC-H Verus kits (tpch_qN_vgen)
# at SF20, on the Verus store, into config/verus/qN_aux_cost_model_sf20.json.
# (config/qN_aux_cost_model*.json are the C++ accelerators' and stay as they are.)
#
# Reads are timed with each kit's first test parameter tuple
# (benchmarks/verus/tpch/qN/params.sql); maintenance with the query's
# registered calibration writes (src/generated/tpch/qN/calibration_writes.hpp).
# A kit has one partition and a write-independent rebuild, so the rebuild is
# measured once (ACCEL_AUX_CALIBRATION_REBUILD_ONCE=1).
#
# Usage:
#   bash scripts/calibrate_verus_sf20_cost_models.sh
#   QUERIES="6 14" bash scripts/calibrate_verus_sf20_cost_models.sh
# Env: DB, WORK, QUERIES, SAMPLES (1), WARMUPS (0), CALIBRATOR, QUERY_TIMEOUT_SEC (14400).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="${DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf20.duckdb}"
WORK="${WORK:-/flash1/akhilvb/tpch/work}"
QUERIES="${QUERIES:-$(seq -s ' ' 1 22)}"
CALIBRATOR="${CALIBRATOR:-${ROOT}/build-verus-duckdb/calibrate_shared_aux_cost_model}"
QUERY_TIMEOUT_SEC="${QUERY_TIMEOUT_SEC:-14400}"
export ACCEL_AUX_STORE=verus
export ACCEL_AUX_CALIBRATION_SAMPLES="${SAMPLES:-1}"
export ACCEL_AUX_CALIBRATION_WARMUPS="${WARMUPS:-0}"
export ACCEL_AUX_CALIBRATION_REBUILD_ONCE=1

[[ -f "${DB}" ]] || { echo "no database at ${DB}" >&2; exit 1; }
[[ -x "${CALIBRATOR}" ]] || {
  echo "no calibrator at ${CALIBRATOR}: cmake build-verus-duckdb -DACCEL_AUX_CALIBRATION=ON &&" \
       "cmake --build build-verus-duckdb --target calibrate_shared_aux_cost_model" >&2; exit 1; }
mkdir -p "${WORK}" "${ROOT}/config/verus"
cd "${ROOT}"

pass=0; fail=0
for q in ${QUERIES}; do
  out="${ROOT}/config/verus/q${q}_aux_cost_model_sf20.json"
  params="$(grep -v '^[[:space:]]*--' "benchmarks/verus/tpch/q${q}/params.sql" | grep -v '^[[:space:]]*$' | head -1 || true)"
  copy="${WORK}/calibrate_q${q}.duckdb"
  echo "=== q${q} $(date +%T) params: ${params:-(none)}"
  cp "${DB}" "${copy}"
  # the model it seeds from and refines is the one the runtime reads
  if ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL="${out}" timeout "${QUERY_TIMEOUT_SEC}" \
      "${CALIBRATOR}" verus "tpch_q${q}_vgen" "${copy}" "${out}" ${params:+"${params}"} \
      > "${WORK}/calibrate_q${q}.log" 2>&1; then
    tail -n 6 "${WORK}/calibrate_q${q}.log"
    pass=$((pass + 1))
  else
    echo "  FAILED: see ${WORK}/calibrate_q${q}.log"; tail -n 3 "${WORK}/calibrate_q${q}.log"
    fail=$((fail + 1))
  fi
  rm -f "${copy}" "${copy}.wal"
done
echo "verus sf20 calibration: pass=${pass} fail=${fail}"
exit "${fail}"
