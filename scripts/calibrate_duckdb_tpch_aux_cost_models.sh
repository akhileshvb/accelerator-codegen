#!/usr/bin/env bash
# Calibrate DuckDB TPC-H aux cost models for Q1..Q22 into config/qN_aux_cost_model.json.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-/tmp/accel_duckdb_build}"
TPCH_DB="${TPCH_DB:-${ACCEL_TPCH_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf1.duckdb}}"
CALIBRATOR="${BIN:-${BUILD}/calibrate_shared_aux_cost_model}"
QUERIES="${ACCEL_CALIBRATE_QUERIES:-$(seq 1 22)}"
SAMPLES="${ACCEL_AUX_CALIBRATION_SAMPLES:-5}"
WARMS="${ACCEL_AUX_CALIBRATION_WARMUPS:-1}"

cd "${ROOT}"
mkdir -p "${ROOT}/config"

if [[ ! -x "${CALIBRATOR}" ]]; then
  echo "missing calibrator: ${CALIBRATOR}" >&2
  echo "build with: cmake -S . -B ${BUILD} -DACCEL_AUX_CALIBRATION=ON && cmake --build ${BUILD} --target calibrate_shared_aux_cost_model" >&2
  exit 1
fi
if [[ ! -f "${TPCH_DB}" ]]; then
  echo "missing DB: ${TPCH_DB}" >&2
  exit 1
fi

export ACCEL_AUX_CALIBRATION_SAMPLES="${SAMPLES}"
export ACCEL_AUX_CALIBRATION_WARMUPS="${WARMS}"
export ACCELERATOR_CODEGEN_CONFIG_DIR="${ROOT}/config"

pass=0
fail=0
for q in ${QUERIES}; do
  out="${ROOT}/config/q${q}_aux_cost_model.json"
  echo "=== DuckDB calibrate TPC-H Q${q} → ${out} ==="
  set +e
  log=$("${CALIBRATOR}" tpch "${q}" "${TPCH_DB}" "${out}" 2>&1)
  rc=$?
  set -e
  printf '%s\n' "${log}" | tail -8
  if [[ ${rc} -eq 0 && -f "${out}" ]]; then
    pass=$((pass + 1))
  else
    echo "FAIL Q${q} rc=${rc}"
    fail=$((fail + 1))
  fi
done

echo "duckdb tpch calibration complete pass=${pass} fail=${fail}"
exit "${fail}"
