#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_multi_connection.sh <query_number>" >&2
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
  echo "{\"gate\":\"aux_multi_connection\",\"query\":${QUERY},\"status\":\"fail\",\"reason\":\"cmake configure failed for ${BUILD}\"}" > "${OUT}/aux_multi_connection.json"
  exit 1
fi

CTEST_REGEX="${MULTI_CONN_CTEST:-test_aux_multi_connection_stress}"
export ACCEL_AUX_BENCHMARK="${BENCHMARK_NAME}"
export ACCEL_MULTI_CONN_TEST_QUERIES="${QUERY}"
export ACCEL_MULTI_CONN_TARGET_READERS="${ACCEL_MULTI_CONN_TARGET_READERS:-3}"
export ACCEL_MULTI_CONN_BACKGROUND_READERS="${ACCEL_MULTI_CONN_BACKGROUND_READERS:-2}"
export ACCEL_MULTI_CONN_STRESS_SEC="${ACCEL_MULTI_CONN_STRESS_SEC:-5}"
export ACCEL_BUILD_DIR="${BUILD}"

if [[ "${ENGINE_NAME}" == "postgres" || "${ACCEL_PG_DOCKER:-}" == "1" ]]; then
  set +e
  run_postgres_docker_harness test_postgres_aux_multi_connection_stress "${QUERY}" \
    -e ACCEL_MULTI_CONN_TEST_QUERIES="${QUERY}" \
    -e ACCEL_MULTI_CONN_TARGET_READERS="${ACCEL_MULTI_CONN_TARGET_READERS}" \
    -e ACCEL_MULTI_CONN_BACKGROUND_READERS="${ACCEL_MULTI_CONN_BACKGROUND_READERS}" \
    -e ACCEL_MULTI_CONN_STRESS_SEC="${ACCEL_MULTI_CONN_STRESS_SEC}"
  RC=$?
  set -e
  LOG="${PG_DOCKER_LOG:-}"
  STATUS="pass"
  if [[ ${RC} -ne 0 ]]; then
    STATUS="fail"
  fi
else
if [[ "${ENGINE_NAME}" == "duckdb" ]]; then
  ensure_multi_connection_stress_test "${ROOT}" "${BUILD}" || true
else
  # Agent-replicated harness: ensure cmake knows the engine-specific target if named.
  ensure_cmake_target "${ROOT}" "${BUILD}" "test_${ENGINE_NAME}_aux_multi_connection_stress" || true
fi

if [[ -x "${BUILD}/ctest" ]]; then
  CTEST="${BUILD}/ctest"
else
  CTEST="ctest"
fi

prepare_engine_work_db "${BUILD}" "multi_conn_q${QUERY}" || fail_missing_engine_harness "${OUT}/aux_multi_connection.json" "aux_multi_connection" "${QUERY}" "work db/dataset"

set +e
LOG="$(cd "${BUILD}" && env "${DB_ENV_VAR}"="${DATASET_DB}" ACCEL_ENGINE_NAME="${ENGINE_NAME}" "${CTEST}" -R "${CTEST_REGEX}" --output-on-failure 2>&1)"
RC=$?
set -e

STATUS="pass"
if [[ ${RC} -ne 0 ]]; then
  STATUS="fail"
fi
if echo "${LOG}" | grep -q "No tests were found"; then
  STATUS="fail"
  RC=1
  if [[ "${ENGINE_NAME}" != "duckdb" ]]; then
    fail_missing_engine_harness "${OUT}/aux_multi_connection.json" "aux_multi_connection" "${QUERY}" "ctest ${CTEST_REGEX}"
  fi
fi
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_multi_connection.json")
path.write_text(json.dumps({
    "gate": "aux_multi_connection",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "engine_name": "${ENGINE_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "ctest_regex": "${CTEST_REGEX}",
    "test_queries": "${ACCEL_MULTI_CONN_TEST_QUERIES}",
    "target_readers": int("${ACCEL_MULTI_CONN_TARGET_READERS}"),
    "background_readers": int("${ACCEL_MULTI_CONN_BACKGROUND_READERS}"),
    "stress_sec": int("${ACCEL_MULTI_CONN_STRESS_SEC}"),
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
