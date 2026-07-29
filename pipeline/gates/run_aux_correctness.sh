#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_correctness.sh <query_number>" >&2
  exit 1
fi

OUT="${ROOT}/pipeline/artifacts/q${QUERY}/gates"
mkdir -p "${OUT}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if [[ ! -d "${BUILD}" ]]; then
  echo "{\"gate\":\"aux_correctness\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/aux_correctness.json"
  exit 0
fi

BUILD_LOG=""
if ! ensure_aux_smoke_test "${ROOT}" "${BUILD}" "${QUERY}"; then
	BUILD_LOG="$(cmake --build "${BUILD}" --target "test_tpch_q${QUERY}_aux" -j"$(nproc)" 2>&1 | tail -40 || true)"
fi

if [[ ! -x "${BUILD}/test_tpch_q${QUERY}_aux" ]]; then
	python3 - <<PY
import json, pathlib, sys
path = pathlib.Path("${OUT}/aux_correctness.json")
payload = {
    "gate": "aux_correctness",
    "query": int("${QUERY}"),
    "status": "fail",
    "reason": "test_tpch_q${QUERY}_aux missing after cmake configure/build",
    "log_tail": """${BUILD_LOG}"""[-4000:],
}
path.write_text(json.dumps(payload, indent=2))
print(path.read_text())
sys.exit(1)
PY
fi

if [[ -x "${BUILD}/ctest" ]]; then
  CTEST="${BUILD}/ctest"
else
  CTEST="ctest"
fi

set +e
LOG="$(cd "${BUILD}" && "${CTEST}" -R "tpch_q${QUERY}_aux" --output-on-failure 2>&1)"
RC=$?
set -e

STATUS="pass"
if [[ ${RC} -ne 0 ]]; then
  STATUS="fail"
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_correctness.json")
path.write_text(json.dumps({
    "gate": "aux_correctness",
    "query": int("${QUERY}"),
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
