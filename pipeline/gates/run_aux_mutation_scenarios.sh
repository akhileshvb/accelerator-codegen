#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_mutation_scenarios.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
mkdir -p "${OUT}"

if [[ "${BENCHMARK_NAME}" == "tpch" ]]; then
  PATTERN="tpch_q${QUERY}_(write_policies|hybrid_policy|staged_publication|multi_partition_delta|action_failure)"
else
  PATTERN="${BENCHMARK_NAME}_q${QUERY}_(write_policies|hybrid_policy|staged_publication|multi_partition_delta|action_failure)"
fi

if ! engine_ready; then
  emit_result "${OUT}/aux_mutation_scenarios.json" aux_mutation_scenarios fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "query:=${QUERY}"
fi
engine_run ctest --test-dir "${ACCEL_BUILD_DIR}" --no-tests=error -j "${ACCEL_CTEST_JOBS:-$(nproc)}" --stop-on-failure -R "${PATTERN}" --output-on-failure

STATUS="pass"
if [[ ${RC} -ne 0 ]]; then
  STATUS="fail"
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_mutation_scenarios.json")
path.write_text(json.dumps({
    "gate": "aux_mutation_scenarios",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "log_tail": """${ENGINE_LOG}${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
