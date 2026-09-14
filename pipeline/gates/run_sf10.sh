#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_sf10.sh <query_number>" >&2
  exit 1
fi

CTX="$(python3 "$(dirname "${BASH_SOURCE[0]}")/read_gate_context.py" "${ROOT}" "${QUERY}")"
ART_NS="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifact_namespace'])" "${CTX}")"
SCALE_DB="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('scale_db',''))" "${CTX}")"
DB_ENV="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['db_env_var'])" "${CTX}")"
BENCH_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['bench_target'])" "${CTX}")"

ENGINE_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('engine_name') or 'duckdb')" "${CTX}")"
OUT="${ROOT}/pipeline/artifacts/${ART_NS}/q${QUERY}/gates"
mkdir -p "${OUT}"
BIN="${BUILD}/${BENCH_TARGET}"

if [[ "${ENGINE_NAME}" != "duckdb" ]]; then
  python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/sf10.json").write_text(json.dumps({
    "gate": "sf10",
    "query": int("${QUERY}"),
    "engine_name": "${ENGINE_NAME}",
    "status": "skip",
    "reason": f"DuckDB sf10 harness not used for engine={ENGINE_NAME}",
}, indent=2))
print(pathlib.Path("${OUT}/sf10.json").read_text())
PY
  exit 0
fi

if [[ -z "${SCALE_DB}" ]] || [[ ! -e "${SCALE_DB}" ]] || [[ ! -x "${BIN}" ]]; then
  python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/sf10.json").write_text(json.dumps({
    "gate": "sf10", "query": int("${QUERY}"), "status": "skip",
    "reason": "scale db not configured, benchmark binary missing, or sf10 db missing",
    "engine_name": "${ENGINE_NAME}",
}, indent=2))
PY
  exit 0
fi

set +e
LOG="$(env "${DB_ENV}=${SCALE_DB}" ACCEL_ENGINE_NAME="${ENGINE_NAME}" "${BIN}" 2>&1)"
RC=$?
set -e

if [[ ${RC} -eq 0 ]]; then
  STATUS="pass"
else
  STATUS="fail"
fi
python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/sf10.json").write_text(json.dumps({
    "gate": "sf10",
    "query": int("${QUERY}"),
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "log_tail": """${LOG}"""[-4000:],
}, indent=2))
PY
