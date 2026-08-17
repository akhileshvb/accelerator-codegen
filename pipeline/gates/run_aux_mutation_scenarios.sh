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

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if ! ensure_build_tree "${ROOT}" "${BUILD}"; then
  echo "{\"gate\":\"aux_mutation_scenarios\",\"query\":${QUERY},\"status\":\"fail\",\"reason\":\"cmake configure failed for ${BUILD}\"}" > "${OUT}/aux_mutation_scenarios.json"
  exit 1
fi

if [[ "${BENCHMARK_NAME}" == "tpch" ]]; then
  PATTERN="tpch_q${QUERY}_(write_policies|hybrid_policy|staged_publication|multi_partition_delta|action_failure)"
else
  PATTERN="${BENCHMARK_NAME}_q${QUERY}_(write_policies|hybrid_policy|staged_publication|multi_partition_delta|action_failure)"
fi

if [[ -x "${BUILD}/ctest" ]]; then
  CTEST="${BUILD}/ctest"
else
  CTEST="ctest"
fi

set +e
LOG="$(cd "${BUILD}" && env "${DB_ENV_VAR}=${DATASET_DB}" "${CTEST}" -R "${PATTERN}" --output-on-failure 2>&1)"
RC=$?
set -e

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
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
