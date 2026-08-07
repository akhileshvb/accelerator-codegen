#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build}"
TPCH_DB="${TPCH_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf1.duckdb}"
SEC_DB="${SEC_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
CALIBRATOR="${BUILD}/calibrate_shared_aux_cost_model"

if [[ ! -x "${CALIBRATOR}" ]]; then
  echo "missing calibrator binary: ${CALIBRATOR}" >&2
  echo "configure with -DACCEL_AUX_CALIBRATION=ON and build target calibrate_shared_aux_cost_model" >&2
  exit 1
fi

calibrate_one() {
  local workload="$1"
  local query="$2"
  local db="$3"
  echo "calibrating ${workload} Q${query} using ${db}"
  "${CALIBRATOR}" "${workload}" "${query}" "${db}"
}

for query in 1 3 6 10 12 14; do
  calibrate_one tpch "${query}" "${TPCH_DB}"
done

for query in 1 2 3 4 6 24; do
  calibrate_one sec_edgar "${query}" "${SEC_DB}"
done

echo "shared aux cost model calibration complete"
