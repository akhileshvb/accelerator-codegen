#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_unit_tests.sh <query_number>" >&2
  exit 1
fi

OUT="${ROOT}/pipeline/artifacts/q${QUERY}/gates"
mkdir -p "${OUT}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if [[ ! -d "${BUILD}" ]]; then
  echo "{\"gate\":\"aux_unit\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/aux_unit.json"
  exit 0
fi

ensure_aux_unit_tests "${ROOT}" "${BUILD}" "${QUERY}" || true

# Match aux unit tests (q{N}_*) and tpch_q{N}_aux smoke, but not base tpch_q{N}_smoke_test.
PATTERN="q${QUERY}_"
EXCLUDE="tpch_q${QUERY}_smoke_test"
if [[ -x "${BUILD}/ctest" ]]; then
  CTEST="${BUILD}/ctest"
else
  CTEST="ctest"
fi

set +e
LOG="$(cd "${BUILD}" && "${CTEST}" -R "${PATTERN}" -E "${EXCLUDE}" --output-on-failure 2>&1)"
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
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "pattern": "${PATTERN}",
    "exclude": "${EXCLUDE}",
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
