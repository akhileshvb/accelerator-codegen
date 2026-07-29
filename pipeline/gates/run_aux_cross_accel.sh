#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_cross_accel.sh <query_number>" >&2
  exit 1
fi

OUT="${ROOT}/pipeline/artifacts/q${QUERY}/gates"
mkdir -p "${OUT}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if [[ ! -d "${BUILD}" ]]; then
  echo "{\"gate\":\"aux_cross_accel\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/aux_cross_accel.json"
  exit 0
fi

ensure_cross_accel_tests "${ROOT}" "${BUILD}" || true

if [[ -x "${BUILD}/ctest" ]]; then
  CTEST="${BUILD}/ctest"
else
  CTEST="ctest"
fi

export ACCEL_CROSS_ACCEL_TEST_QUERIES="${QUERY}"

set +e
LOG="$(cd "${BUILD}" && "${CTEST}" -R "test_cross_accel" --output-on-failure 2>&1)"
RC=$?
set -e

STATUS="pass"
if [[ ${RC} -ne 0 ]]; then
  STATUS="fail"
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_cross_accel.json")
path.write_text(json.dumps({
    "gate": "aux_cross_accel",
    "query": int("${QUERY}"),
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "test_queries": "${ACCEL_CROSS_ACCEL_TEST_QUERIES}",
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
