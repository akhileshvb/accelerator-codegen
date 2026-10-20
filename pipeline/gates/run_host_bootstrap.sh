#!/usr/bin/env bash
# Verify one-shot query-host bootstrap for the selected engine.
# Postgres: Docker build of differential + optional smoke against Docker PG.
# Do not fall through from docker/duckdb into the host-cmake path — that
# overwrites a docker pass with `make: No rule to make target`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-0}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"

CTX="$(python3 "$(dirname "${BASH_SOURCE[0]}")/read_gate_context.py" "${ROOT}" "${QUERY}")"
ENGINE_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('engine_name') or 'duckdb')" "${CTX}")"
DIFF_BINARY="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('differential_binary') or '')" "${CTX}")"
DIFF_SOURCE="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('differential_source') or '')" "${CTX}")"
REGISTER_PATH="$(python3 -c "import json,sys; h=json.loads(sys.argv[1]).get('engine_host') or {}; print(h.get('register_path') or '')" "${CTX}")"
ART_DIR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifacts_dir'])" "${CTX}")"

OUT="${ROOT}/pipeline/artifacts/engines/${ENGINE_NAME}/host_bootstrap"
mkdir -p "${OUT}"
QOUT="${ART_DIR}/gates"
mkdir -p "${QOUT}"

write_result() {
  local status="$1"
  local reason="$2"
  local log_tail="${3:-}"
  python3 - <<PY
import json, pathlib
payload = {
    "gate": "host_bootstrap",
    "query": int("${QUERY}"),
    "engine_name": "${ENGINE_NAME}",
    "status": "${status}",
    "reason": """${reason}""",
    "differential_binary": "${DIFF_BINARY}",
    "mode": "docker" if "${ENGINE_NAME}" == "postgres" else "host",
    "log_tail": """${log_tail}"""[-4000:],
}
for p in (pathlib.Path("${OUT}/host_bootstrap.json"), pathlib.Path("${QOUT}/host_bootstrap.json")):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(payload, indent=2) + "\n")
print(pathlib.Path("${OUT}/host_bootstrap.json").read_text())
PY
  if [[ "${status}" == "pass" ]]; then
    return 0
  fi
  exit 1
}

if [[ "${ENGINE_NAME}" == "duckdb" ]]; then
  write_result "pass" "duckdb host already present"
  exit 0
fi

DIFF_TARGET="${DIFF_BINARY:-test_${ENGINE_NAME}_tpch_differential}"

if [[ "${ENGINE_NAME}" == "postgres" ]]; then
  BUILD_LOG=""
  if ! BUILD_LOG="$(bash "${ROOT}/scripts/ensure_accel_pg_test.sh" 2>&1)"; then
    write_result "fail" "docker ensure failed for postgres host_bootstrap" "${BUILD_LOG}"
  fi
  export ACCEL_PG_NINJA_TARGETS="${DIFF_TARGET}"
  if ! BUILD_LOG="${BUILD_LOG}"$'\n'"$(bash "${ROOT}/scripts/build_accel_postgres_extension.sh" 2>&1)"; then
    write_result "fail" "docker build failed for ${DIFF_TARGET}" "${BUILD_LOG}"
  fi
  CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
  if ! docker exec "${CONTAINER}" test -x "/accel-build/accel/${DIFF_TARGET}"; then
    write_result "fail" "missing in-container ${DIFF_TARGET}: create ${DIFF_SOURCE} + CMake + host SQL on ${REGISTER_PATH}" "${BUILD_LOG}"
  fi
  # Optional TPC-H DB for later correctness (best-effort).
  ACCEL_PG_ENSURE_TPCH=0 bash "${ROOT}/scripts/setup_accel_postgres_tpch_test_db.sh" >/dev/null 2>&1 || true
  write_result "pass" "${DIFF_TARGET} built in docker (${CONTAINER})" "${BUILD_LOG}"
  exit 0
fi

# Non-postgres / non-duckdb fallback: host cmake
# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if [[ ! -d "${BUILD}" ]]; then
  write_result "fail" "build dir missing: ${BUILD}"
fi

BUILD_LOG=""
if ! ensure_cmake_target "${ROOT}" "${BUILD}" "${DIFF_TARGET}"; then
  BUILD_LOG="$(cmake --build "${BUILD}" --target "${DIFF_TARGET}" -j"$(nproc)" 2>&1 | tail -80 || true)"
fi

if [[ ! -x "${BUILD}/${DIFF_TARGET}" ]]; then
  write_result "fail" "missing ${DIFF_TARGET}: create ${DIFF_SOURCE:-eval/tests/${DIFF_TARGET}.cpp} + CMake and host SQL entrypoints on ${REGISTER_PATH}" "${BUILD_LOG}"
fi

write_result "pass" "${DIFF_TARGET} built" "${BUILD_LOG}"
exit 0
