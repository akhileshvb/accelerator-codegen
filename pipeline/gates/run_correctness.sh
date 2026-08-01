#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_correctness.sh <query_number>" >&2
  exit 1
fi

CTX="$(python3 "$(dirname "${BASH_SOURCE[0]}")/read_gate_context.py" "${ROOT}" "${QUERY}")"
ART_NS="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifact_namespace'])" "${CTX}")"
CTEST_REGEX="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['ctest_smoke_regex'])" "${CTX}")"
TEST_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['test_target'])" "${CTX}")"
DB="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['dataset_db'])" "${CTX}")"
DB_ENV="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['db_env_var'])" "${CTX}")"

OUT="${ROOT}/pipeline/artifacts/${ART_NS}/q${QUERY}/gates"
mkdir -p "${OUT}"

if [[ ! -d "${BUILD}" ]]; then
  echo "{\"gate\":\"correctness\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/correctness.json"
  exit 0
fi

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

BUILD_LOG=""
if ! ensure_base_smoke_test "${ROOT}" "${BUILD}" "${TEST_TARGET}"; then
  BUILD_LOG="$(cmake --build "${BUILD}" --target "${TEST_TARGET}" -j"$(nproc)" 2>&1 | tail -40 || true)"
fi

set +e
LOG="$(cd "${BUILD}" && env "${DB_ENV}=${DB}" ctest -R "${CTEST_REGEX}" --output-on-failure 2>&1)"
RC=$?
set -e

STATUS="pass"
if [[ ${RC} -ne 0 ]] || echo "${LOG}" | grep -q 'No tests were found'; then
  STATUS="fail"
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/correctness.json")
path.write_text(json.dumps({
    "gate": "correctness",
    "query": int("${QUERY}"),
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "log_tail": """${BUILD_LOG}${LOG}"""[-4000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
