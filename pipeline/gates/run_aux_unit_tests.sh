#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_unit_tests.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
mkdir -p "${OUT}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if [[ ! -d "${BUILD}" ]]; then
  echo "{\"gate\":\"aux_unit\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/aux_unit.json"
  exit 0
fi

ensure_aux_unit_tests "${ROOT}" "${BUILD}" "${QUERY}" "${BENCHMARK_NAME}" || true

# Scope by benchmark prefix — bare "q${N}_" also matches sec_edgar_q${N}_* (and vice versa).
if [[ "${BENCHMARK_NAME}" == "tpch" ]]; then
  PATTERN="tpch_q${QUERY}_"
  # Keep aux smoke; drop base-kernel smoke for this query.
  EXCLUDE="^tpch_q${QUERY}_smoke_test$"
else
  PATTERN="${BENCHMARK_NAME}_q${QUERY}_"
  EXCLUDE="^${BENCHMARK_NAME}_q${QUERY}_smoke_test$"
fi

if [[ -x "${BUILD}/ctest" ]]; then
  CTEST="${BUILD}/ctest"
else
  CTEST="ctest"
fi

set +e
LOG="$(cd "${BUILD}" && env "${DB_ENV_VAR}=${DATASET_DB}" "${CTEST}" -R "${PATTERN}" -E "${EXCLUDE}" --output-on-failure 2>&1)"
RC=$?
set -e

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
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
