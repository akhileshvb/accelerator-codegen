#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_cross_accel.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
mkdir -p "${OUT}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if ! ensure_build_tree "${ROOT}" "${BUILD}"; then
  echo "{\"gate\":\"aux_cross_accel\",\"query\":${QUERY},\"status\":\"fail\",\"reason\":\"cmake configure failed for ${BUILD}\"}" > "${OUT}/aux_cross_accel.json"
  exit 1
fi

ensure_cross_accel_tests "${ROOT}" "${BUILD}" || true

if [[ -x "${BUILD}/ctest" ]]; then
  CTEST="${BUILD}/ctest"
else
  CTEST="ctest"
fi

export ACCEL_AUX_BENCHMARK="${BENCHMARK_NAME}"
export ACCEL_CROSS_ACCEL_TEST_QUERIES="${QUERY}"
export ACCEL_BUILD_DIR="${BUILD}"
WORK_DB="${BUILD}/cross_accel_q${QUERY}.duckdb"
cp "${DATASET_DB}" "${WORK_DB}"
export ACCEL_CROSS_ACCEL_WORK_DB="${WORK_DB}"

set +e
LOG="$(cd "${BUILD}" && env "${DB_ENV_VAR}"="${DATASET_DB}" "${CTEST}" -R "test_cross_accel" --output-on-failure 2>&1)"
RC=$?
set -e

STATUS="pass"
if [[ ${RC} -ne 0 ]]; then
  STATUS="fail"
fi
if echo "${LOG}" | grep -q "No tests were found"; then
  STATUS="fail"
  RC=1
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_cross_accel.json")
path.write_text(json.dumps({
    "gate": "aux_cross_accel",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "test_queries": "${ACCEL_CROSS_ACCEL_TEST_QUERIES}",
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
