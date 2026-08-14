#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_optimization_probe.sh <query_number>" >&2
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
  echo "{\"gate\":\"aux_optimization_probe\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/aux_optimization_probe.json"
  exit 0
fi

if ! ensure_cmake_target "${ROOT}" "${BUILD}" "${AUX_TEST_TARGET}"; then
  cmake --build "${BUILD}" --target "${AUX_TEST_TARGET}" -j"$(nproc)" >/dev/null 2>&1 || true
fi

PROBE_REGEX="${AUX_CTEST_SMOKE_REGEX}"
if [[ -x "${BUILD}/ctest" ]]; then
  CTEST="${BUILD}/ctest"
else
  CTEST="ctest"
fi

set +e
LOG="$(cd "${BUILD}" && env "${DB_ENV_VAR}=${DATASET_DB}" "${CTEST}" -R "${PROBE_REGEX}" --output-on-failure 2>&1)"
RC=$?
set -e

STATUS="pass"
if [[ ${RC} -ne 0 ]]; then
  STATUS="fail"
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_optimization_probe.json")
path.write_text(json.dumps({
    "gate": "aux_optimization_probe",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "ctest_regex": "${PROBE_REGEX}",
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
