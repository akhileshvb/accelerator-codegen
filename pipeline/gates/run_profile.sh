#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_profile.sh <query_number>" >&2
  exit 1
fi

CTX="$(python3 "$(dirname "${BASH_SOURCE[0]}")/read_gate_context.py" "${ROOT}" "${QUERY}")"
ART_DIR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifacts_dir'])" "${CTX}")"
DB="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['dataset_db'])" "${CTX}")"
DB_ENV="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['db_env_var'])" "${CTX}")"
BENCH_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['bench_target'])" "${CTX}")"
ENGINE_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('engine_name') or 'duckdb')" "${CTX}")"

OUT="${ART_DIR}/gates"
mkdir -p "${OUT}"
BIN="${BUILD}/${BENCH_TARGET}"

if [[ "${ENGINE_NAME}" != "duckdb" ]]; then
  ENGINE_BENCH="benchmark_${ENGINE_NAME}_tpch_q${QUERY}"
  if [[ -x "${BUILD}/${ENGINE_BENCH}" ]]; then
    BIN="${BUILD}/${ENGINE_BENCH}"
    BENCH_TARGET="${ENGINE_BENCH}"
  else
    python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/profile.json").write_text(json.dumps({
    "gate": "profile",
    "query": int("${QUERY}"),
    "engine_name": "${ENGINE_NAME}",
    "status": "skip",
    "reason": (
        f"DuckDB profile harness not used for engine={ENGINE_NAME}; "
        f"correctness is {ENGINE_NAME} differential."
    ),
}, indent=2))
print(pathlib.Path("${OUT}/profile.json").read_text())
PY
    exit 0
  fi
fi

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"
build_cmake_target "${ROOT}" "${BUILD}" "${BENCH_TARGET}" || true

if [[ ! -x "${BIN}" ]]; then
  python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/profile.json").write_text(json.dumps({
    "gate": "profile", "query": int("${QUERY}"), "status": "skip",
    "reason": "benchmark binary missing",
    "engine_name": "${ENGINE_NAME}",
}, indent=2))
PY
  exit 0
fi

set +e
LOG="$(env "${DB_ENV}=${DB}" ACCEL_ENGINE_NAME="${ENGINE_NAME}" ACCEL_BENCH_PROFILE=1 "${BIN}" 2>&1)"
RC=$?
set -e

if [[ ${RC} -eq 0 ]]; then
  STATUS="pass"
else
  STATUS="fail"
fi
python3 - <<PY
import json, pathlib
profile_path = pathlib.Path("${ART_DIR}/profile_summary.json")
profile_path.write_text(json.dumps({"log_tail": """${LOG}"""[-8000:]}, indent=2))
pathlib.Path("${OUT}/profile.json").write_text(json.dumps({
    "gate": "profile",
    "query": int("${QUERY}"),
    "engine_name": "${ENGINE_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "profile_summary": str(profile_path),
    "log_tail": """${LOG}"""[-8000:],
}, indent=2))
PY

[[ "${STATUS}" == "pass" || "${STATUS}" == "skip" ]]
