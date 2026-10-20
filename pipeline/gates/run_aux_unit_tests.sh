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

# Scope by benchmark prefix — bare "q${N}_" also matches sec_edgar_q${N}_* (and vice versa).
if [[ "${BENCHMARK_NAME}" == "tpch" ]]; then
  PATTERN="tpch_q${QUERY}_"
  # Keep aux smoke; drop base-kernel smoke for this query.
  EXCLUDE="^tpch_q${QUERY}_smoke_test$"
else
  PATTERN="${BENCHMARK_NAME}_q${QUERY}_"
  EXCLUDE="^${BENCHMARK_NAME}_q${QUERY}_smoke_test$"
fi

if ! engine_ready; then
  emit_result "${OUT}/aux_unit.json" aux_unit fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "query:=${QUERY}"
fi
engine_run ctest --test-dir "${ACCEL_BUILD_DIR}" -R "${PATTERN}" -E "${EXCLUDE}" --output-on-failure

STATUS="pass"
if [[ ${RC} -ne 0 ]]; then
  STATUS="fail"
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_unit.json")
path.write_text(json.dumps({
    "gate": "aux_unit",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "pattern": "${PATTERN}",
    "exclude": "${EXCLUDE}",
    "log_tail": """${ENGINE_LOG}${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
