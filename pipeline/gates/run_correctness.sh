#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_correctness.sh <query_number>" >&2
  exit 1
fi

CTX="$(python3 "$(dirname "${BASH_SOURCE[0]}")/read_gate_context.py" "${ROOT}" "${QUERY}")"
ART_DIR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifacts_dir'])" "${CTX}")"
CTEST_REGEX="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['ctest_smoke_regex'])" "${CTX}")"
TEST_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['test_target'])" "${CTX}")"
DB="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['dataset_db'])" "${CTX}")"
DB_ENV="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['db_env_var'])" "${CTX}")"
ENGINE_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('engine_name') or 'duckdb')" "${CTX}")"
DIFF_BINARY="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('differential_binary') or '')" "${CTX}")"
DIFF_SOURCE="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('differential_source') or '')" "${CTX}")"
AGENT_BUILDS_HOST="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print('1' if h.get('agent_builds_query_host') else '0')" "${CTX}")"
HOST_HINT="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('query_host_ready_hint') or '')" "${CTX}")"

OUT="${ART_DIR}/gates"
mkdir -p "${OUT}"

if [[ ! -d "${BUILD}" ]]; then
  echo "{\"gate\":\"correctness\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/correctness.json"
  exit 0
fi

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

BUILD_LOG=""
LOG=""
RC=0
STATUS="pass"
REASON=""

# Explicit non-duckdb engine_name: never fall back to DuckDB ctest/smoke.
if [[ "${ENGINE_NAME}" != "duckdb" ]]; then
  DIFF_TARGET="${DIFF_BINARY:-test_${ENGINE_NAME}_tpch_differential}"
  if [[ "${ENGINE_NAME}" == "postgres" || "${ACCEL_PG_DOCKER:-}" == "1" ]]; then
    CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
    set +e
    BUILD_LOG="$(bash "${ROOT}/scripts/ensure_accel_pg_test.sh" 2>&1)"
    ENSURE_RC=$?
    set -e
    if [[ ${ENSURE_RC} -ne 0 ]]; then
      STATUS="fail"
      REASON="docker ensure failed for postgres correctness"
      RC=1
      LOG="${BUILD_LOG}"
    else
      export ACCEL_PG_NINJA_TARGETS="${DIFF_TARGET}"
      set +e
      BUILD_LOG="${BUILD_LOG}"$'\n'"$(bash "${ROOT}/scripts/build_accel_postgres_extension.sh" 2>&1)"
      BUILD_RC=$?
      set -e
      if [[ ${BUILD_RC} -ne 0 ]] || ! docker exec "${CONTAINER}" test -x "/accel-build/accel/${DIFF_TARGET}"; then
        STATUS="fail"
        REASON="missing ${DIFF_TARGET} in docker (no DuckDB fallback). ${HOST_HINT}"
        RC=1
        LOG="${BUILD_LOG}"
      else
        set +e
        LOG="$(docker exec \
          -e ACCEL_PG_DOCKER=1 -e PGHOST=127.0.0.1 -e PGPORT=5432 \
          -e PGUSER="${PGUSER:-postgres}" -e PGPASSWORD="${PGPASSWORD:-postgres}" \
          -e ACCEL_ENGINE_NAME=postgres \
          -e ACCEL_PG_DIFF_KIND=base \
          -e ACCEL_PG_DIFF_SKIP_WRITE_MAINT=1 \
          -e TPCH_PG_DB="${TPCH_PG_DB:-accel_tpch_test}" \
          "${CONTAINER}" "/accel-build/accel/${DIFF_TARGET}" "${QUERY}" 2>&1)"
        RC=$?
        set -e
        if [[ ${RC} -ne 0 ]]; then
          STATUS="fail"
          REASON="${DIFF_TARGET} failed for query ${QUERY} (engine=postgres docker)"
        fi
      fi
    fi
  else
    if ! ensure_cmake_target "${ROOT}" "${BUILD}" "${DIFF_TARGET}"; then
      BUILD_LOG="$(cmake --build "${BUILD}" --target "${DIFF_TARGET}" -j"$(nproc)" 2>&1 | tail -80 || true)"
    fi
    if [[ ! -x "${BUILD}/${DIFF_TARGET}" ]]; then
      STATUS="fail"
      REASON="missing ${DIFF_TARGET} for engine=${ENGINE_NAME} (no DuckDB fallback). Run host_bootstrap / implement query host SQL entrypoints + ${DIFF_SOURCE:-eval/tests/${DIFF_TARGET}.cpp}. ${HOST_HINT}"
      RC=1
      LOG="${BUILD_LOG}"
    else
      set +e
      LOG="$(cd "${BUILD}" && env ACCEL_ENGINE_NAME="${ENGINE_NAME}" ./"${DIFF_TARGET}" "${QUERY}" 2>&1)"
      RC=$?
      set -e
      if [[ ${RC} -ne 0 ]]; then
        STATUS="fail"
        REASON="${DIFF_TARGET} failed for query ${QUERY} (engine=${ENGINE_NAME})"
      fi
    fi
  fi
else
  if ! ensure_base_smoke_test "${ROOT}" "${BUILD}" "${TEST_TARGET}"; then
    BUILD_LOG="$(cmake --build "${BUILD}" --target "${TEST_TARGET}" -j"$(nproc)" 2>&1 | tail -40 || true)"
  fi

  set +e
  LOG="$(cd "${BUILD}" && env "${DB_ENV}=${DB}" ctest -R "${CTEST_REGEX}" --output-on-failure 2>&1)"
  RC=$?
  set -e

  if [[ ${RC} -ne 0 ]] || echo "${LOG}" | grep -q 'No tests were found'; then
    STATUS="fail"
  fi
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/correctness.json")
path.write_text(json.dumps({
    "gate": "correctness",
    "query": int("${QUERY}"),
    "engine_name": "${ENGINE_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "reason": """${REASON}""",
    "log_tail": """${BUILD_LOG}${LOG}"""[-8000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
