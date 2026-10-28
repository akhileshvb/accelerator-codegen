#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_optimization_probe.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
mkdir -p "${OUT}"

if [[ ! -d "${BUILD}" ]]; then
  echo "{\"gate\":\"aux_optimization_probe\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/aux_optimization_probe.json"
  exit 0
fi

PROBE_REGEX="${AUX_CTEST_SMOKE_REGEX}"
if ! engine_ready "${AUX_TEST_TARGET}"; then
  engine_not_ready "${OUT}/aux_optimization_probe.json" aux_optimization_probe "query:=${QUERY}"
fi
engine_run ctest --test-dir "${ACCEL_BUILD_DIR}" --no-tests=error -j "${ACCEL_CTEST_JOBS:-$(nproc)}" --stop-on-failure -R "${PROBE_REGEX}" --output-on-failure

STATUS="pass"
if [[ ${RC} -ne 0 ]]; then
  STATUS="fail"
fi

GATE_LOG_TEXT="${ENGINE_LOG}${LOG}" python3 - <<PY
import os, json, pathlib
path = pathlib.Path("${OUT}/aux_optimization_probe.json")
path.write_text(json.dumps({
    "gate": "aux_optimization_probe",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "ctest_regex": "${PROBE_REGEX}",
    "log_tail": os.environ["GATE_LOG_TEXT"][-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
