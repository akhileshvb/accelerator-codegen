#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build}"
TPCH_DB="${TPCH_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf1.duckdb}"
SEC_DB="${SEC_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
CALIBRATOR="${BUILD}/calibrate_shared_aux_cost_model"

# Prefer the dedicated DuckDB TPC-H calibrator (Q1..Q22 → config/qN_aux_cost_model.json).
if [[ -x "${ROOT}/scripts/calibrate_duckdb_tpch_aux_cost_models.sh" ]]; then
  BUILD="${BUILD}" TPCH_DB="${TPCH_DB}" \
    bash "${ROOT}/scripts/calibrate_duckdb_tpch_aux_cost_models.sh"
else
  if [[ ! -x "${CALIBRATOR}" ]]; then
    echo "missing calibrator binary: ${CALIBRATOR}" >&2
    echo "configure with -DACCEL_AUX_CALIBRATION=ON and build target calibrate_shared_aux_cost_model" >&2
    exit 1
  fi
  for query in $(seq 1 22); do
    echo "calibrating tpch Q${query} using ${TPCH_DB}"
    "${CALIBRATOR}" tpch "${query}" "${TPCH_DB}"
  done
fi

if [[ -f "${SEC_DB}" && -x "${CALIBRATOR}" ]]; then
  for query in 1 2 3 4 6 24; do
    echo "calibrating sec_edgar Q${query} using ${SEC_DB}"
    "${CALIBRATOR}" sec_edgar "${query}" "${SEC_DB}"
  done
fi

echo "shared aux cost model calibration complete"
