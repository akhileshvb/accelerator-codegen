#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_baseline.sh <query_number>" >&2
  exit 1
fi

CTX="$(python3 "$(dirname "${BASH_SOURCE[0]}")/read_gate_context.py" "${ROOT}" "${QUERY}")"
ART_NS="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifact_namespace'])" "${CTX}")"
DB="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['dataset_db'])" "${CTX}")"
DB_ENV="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['db_env_var'])" "${CTX}")"
BENCH_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['bench_target'])" "${CTX}")"

OUT="${ROOT}/pipeline/artifacts/${ART_NS}/q${QUERY}/gates"
mkdir -p "${OUT}"
BIN="${BUILD}/${BENCH_TARGET}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"
build_cmake_target "${ROOT}" "${BUILD}" "${BENCH_TARGET}" || true

if [[ ! -x "${BIN}" ]]; then
  python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/baseline.json").write_text(json.dumps({
    "gate": "baseline", "query": int("${QUERY}"), "status": "skip",
    "reason": "benchmark binary missing",
}, indent=2))
PY
  exit 0
fi

set +e
LOG="$(env "${DB_ENV}=${DB}" "${BIN}" 2>&1)"
RC=$?
set -e

if [[ ${RC} -eq 0 ]]; then
  STATUS="pass"
else
  STATUS="fail"
fi
python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/baseline.json").write_text(json.dumps({
    "gate": "baseline",
    "query": int("${QUERY}"),
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
PY
