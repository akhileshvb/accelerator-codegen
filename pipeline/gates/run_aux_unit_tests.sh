#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_unit_tests.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
mkdir -p "${OUT}"

# Scope by benchmark prefix (a bare "q${N}_" would match every benchmark's q<N>); keep the aux smoke test, drop the
# base kernel's.
PATTERN="${BENCHMARK_NAME}_q${QUERY}_"
EXCLUDE="^${BENCHMARK_NAME}_q${QUERY}_smoke_test$"

thread_lint
if [[ ${THREAD_LINT_RC} -ne 0 ]]; then
  emit_result "${OUT}/aux_unit.json" aux_unit fail \
    "hard-coded thread count or hardware_concurrency in the query sources: take caps from the substrate thread budget (ACCEL_AUX_THREADS ...)" \
    "${THREAD_LINT_LOG}" "query:=${QUERY}"
fi

if ! engine_ready_for_ctest "${PATTERN}" "${EXCLUDE}"; then
  emit_result "${OUT}/aux_unit.json" aux_unit fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "query:=${QUERY}"
fi
engine_run ctest --test-dir "${ACCEL_BUILD_DIR}" --no-tests=error -j "${ACCEL_CTEST_JOBS:-$(nproc)}" --stop-on-failure -R "${PATTERN}" -E "${EXCLUDE}" --output-on-failure

STATUS="pass"
if [[ ${RC} -ne 0 ]]; then
  STATUS="fail"
fi

GATE_LOG_TEXT="${ENGINE_LOG}${LOG}" python3 - <<PY
import os, json, pathlib
path = pathlib.Path("${OUT}/aux_unit.json")
path.write_text(json.dumps({
    "gate": "aux_unit",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "pattern": "${PATTERN}",
    "exclude": "${EXCLUDE}",
    "log_tail": os.environ["GATE_LOG_TEXT"][-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
