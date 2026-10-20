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
ART_DIR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['artifacts_dir'])" "${CTX}")"
DB="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['dataset_db'])" "${CTX}")"
DB_ENV="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['db_env_var'])" "${CTX}")"
BENCH_TARGET="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['bench_target'])" "${CTX}")"
ENGINE_NAME="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('engine_name') or 'duckdb')" "${CTX}")"
WORKLOAD_KIND="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('workload_kind') or '')" "${CTX}")"

OUT="${ART_DIR}/gates"
mkdir -p "${OUT}"
BIN="${BUILD}/${BENCH_TARGET}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if [[ "${ENGINE_NAME}" != "duckdb" ]]; then
  ENGINE_BENCH="benchmark_${ENGINE_NAME}_tpch_q${QUERY}"
  SHARED_BENCH="benchmark_${ENGINE_NAME}_tpch_aux"
  if [[ "${ENGINE_NAME}" == "postgres" || "${ACCEL_PG_DOCKER:-}" == "1" ]]; then
    CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
    set +e
    ENSURE_LOG="$(bash "${ROOT}/scripts/ensure_accel_pg_test.sh" 2>&1)"
    ENSURE_RC=$?
    set -e
    if [[ ${ENSURE_RC} -ne 0 ]]; then
      python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/baseline.json").write_text(json.dumps({
    "gate": "baseline", "query": int("${QUERY}"), "engine_name": "${ENGINE_NAME}",
    "status": "fail", "reason": "docker ensure failed",
    "log_tail": """${ENSURE_LOG}"""[-4000:],
}, indent=2))
print(pathlib.Path("${OUT}/baseline.json").read_text())
PY
      exit 1
    fi
    export ACCEL_PG_NINJA_TARGETS="${SHARED_BENCH}"
    set +e
    BUILD_LOG="$(bash "${ROOT}/scripts/build_accel_postgres_extension.sh" 2>&1)"
    BUILD_RC=$?
    set -e
    if [[ ${BUILD_RC} -ne 0 ]] || ! docker exec "${CONTAINER}" test -x "/accel-build/accel/${SHARED_BENCH}"; then
      python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/baseline.json").write_text(json.dumps({
    "gate": "baseline", "query": int("${QUERY}"), "engine_name": "${ENGINE_NAME}",
    "status": "fail",
    "reason": "missing ${SHARED_BENCH} (fail-closed; no silent skip)",
    "log_tail": """${BUILD_LOG}"""[-6000:],
}, indent=2))
print(pathlib.Path("${OUT}/baseline.json").read_text())
PY
      exit 1
    fi
    set +e
    LOG0="$(docker exec -e ACCEL_PG_DOCKER=1 -e PGHOST=127.0.0.1 -e PGPORT=5432 \
      -e PGUSER="${PGUSER:-postgres}" -e PGPASSWORD="${PGPASSWORD:-postgres}" \
      -e ACCEL_ENGINE_NAME=postgres -e ACCEL_WORKLOAD_KIND="${WORKLOAD_KIND}" \
      -e TPCH_PG_DB=accel_tpch_test \
      "${CONTAINER}" "/accel-build/accel/${SHARED_BENCH}" "${QUERY}" 0 2>&1)"
    RC0=$?
    LOG10="$(docker exec -e ACCEL_PG_DOCKER=1 -e PGHOST=127.0.0.1 -e PGPORT=5432 \
      -e PGUSER="${PGUSER:-postgres}" -e PGPASSWORD="${PGPASSWORD:-postgres}" \
      -e ACCEL_ENGINE_NAME=postgres -e ACCEL_WORKLOAD_KIND="${WORKLOAD_KIND}" \
      -e TPCH_PG_DB=accel_tpch_test \
      "${CONTAINER}" "/accel-build/accel/${SHARED_BENCH}" "${QUERY}" 10 2>&1)"
    RC10=$?
    set -e
    LOG="${LOG0}"$'\n'"${LOG10}"
    RC=0
    [[ ${RC0} -eq 0 && ${RC10} -eq 0 ]] || RC=1
    STATUS="pass"
    [[ ${RC} -eq 0 ]] || STATUS="fail"
    # Dual-axis: require read_only speedup >= min (default 2.0) for pass when aux.
    python3 - <<PY
import json, pathlib, re, math
log = """${LOG}"""
status = "${STATUS}"
reason = ""
ro = None
wm = None
for line in log.splitlines():
    if line.startswith("read_only_query_speedup,"):
        ro = float(line.split(",")[1])
    if line.startswith("write_mix_workload_speedup,"):
        wm = float(line.split(",")[1])
min_ro = float("${ACCEL_AUX_MIN_SPEEDUP:-2.0}")
if status == "pass" and "${WORKLOAD_KIND}" == "aux_state":
    if ro is None or ro < min_ro:
        status = "fail"
        reason = f"read_only_query_speedup {ro} < min {min_ro} vs Postgres SQL"
    elif wm is not None:
        score = (ro ** 0.75) * (max(wm, 1e-9) ** 0.25)
        if score < 1.0 and wm < 0.5:
            status = "fail"
            reason = f"dual-axis cliff score={score:.3f} write_mix={wm}"
pathlib.Path("${OUT}/baseline.json").write_text(json.dumps({
    "gate": "baseline",
    "query": int("${QUERY}"),
    "engine_name": "${ENGINE_NAME}",
    "status": status,
    "exit_code": 0 if status == "pass" else 1,
    "read_only_query_speedup": ro,
    "write_mix_workload_speedup": wm,
    "reason": reason,
    "log_tail": log[-8000:],
}, indent=2))
print(pathlib.Path("${OUT}/baseline.json").read_text())
raise SystemExit(0 if status == "pass" else 1)
PY
    exit $?
  fi

  # Non-docker non-duckdb: fail closed if shared bench missing.
  ensure_cmake_target "${ROOT}" "${BUILD}" "${SHARED_BENCH}" || true
  if [[ ! -x "${BUILD}/${SHARED_BENCH}" ]]; then
    python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/baseline.json").write_text(json.dumps({
    "gate": "baseline", "query": int("${QUERY}"), "engine_name": "${ENGINE_NAME}",
    "status": "fail",
    "reason": f"missing {\"${SHARED_BENCH}\"} (fail-closed for non-duckdb)",
}, indent=2))
print(pathlib.Path("${OUT}/baseline.json").read_text())
PY
    exit 1
  fi
  BIN="${BUILD}/${SHARED_BENCH}"
  BENCH_TARGET="${SHARED_BENCH}"
fi

build_cmake_target "${ROOT}" "${BUILD}" "${BENCH_TARGET}" || true

if [[ ! -x "${BIN}" ]]; then
  python3 - <<PY
import json, pathlib
pathlib.Path("${OUT}/baseline.json").write_text(json.dumps({
    "gate": "baseline", "query": int("${QUERY}"), "status": "fail",
    "reason": "benchmark binary missing (fail-closed)",
    "engine_name": "${ENGINE_NAME}",
}, indent=2))
PY
  exit 1
fi

set +e
LOG="$(env "${DB_ENV}=${DB}" ACCEL_ENGINE_NAME="${ENGINE_NAME}" ACCEL_WORKLOAD_KIND="${WORKLOAD_KIND}" \
  "${BIN}" "${QUERY}" 0 2>&1)"
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
    "engine_name": "${ENGINE_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${RC}"),
    "log_tail": """${LOG}"""[-8000:],
}, indent=2))
PY

[[ "${STATUS}" == "pass" ]]
