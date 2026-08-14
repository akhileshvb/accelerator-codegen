#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ROOT}/build-release"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_cost_model_calibrate.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
mkdir -p "${OUT}"
CONFIG="${ROOT}/${COST_MODEL_CONFIG}"

write_skip() {
  local reason="$1"
  python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_cost_model_calibrate.json")
payload = {
    "gate": "aux_cost_model_calibrate",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "skip",
    "reason": """${reason}""",
    "config_path": "${CONFIG}",
}
path.write_text(json.dumps(payload, indent=2))
print(path.read_text())
PY
}

if [[ "${ACCEL_SKIP_AUX_CALIBRATE:-0}" == "1" ]]; then
  write_skip "ACCEL_SKIP_AUX_CALIBRATE=1"
  exit 0
fi

if [[ -f "${CONFIG}" ]]; then
  if python3 - <<PY
import json, sys
from pathlib import Path
data = json.loads(Path("${CONFIG}").read_text(encoding="utf-8"))
delta = float(data.get("interference_delta_per_query_us") or 0)
rebuild = float(data.get("interference_rebuild_per_query_us") or 0)
sys.exit(0 if delta > 0 and rebuild > 0 else 1)
PY
  then
    write_skip "reusing existing cost model at ${CONFIG}"
    exit 0
  fi
fi

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if [[ ! -d "${BUILD}" ]]; then
  echo "{\"gate\":\"aux_cost_model_calibrate\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/aux_cost_model_calibrate.json"
  exit 0
fi

TARGET="${AUX_CALIBRATE_TARGET}"
CALIBRATOR_SOURCE="${ROOT}/benchmarks/${BENCHMARK_NAME}/single-threaded/calibrate_q${QUERY}_aux_cost_model.cpp"
if [[ -f "${CALIBRATOR_SOURCE}" ]] && ! grep -q 'CalibrateAndWriteCostModel' "${CALIBRATOR_SOURCE}"; then
  python3 - <<PY
import json, pathlib, sys
path = pathlib.Path("${OUT}/aux_cost_model_calibrate.json")
payload = {
    "gate": "aux_cost_model_calibrate",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "fail",
    "reason": "calibrate wrapper must call CalibrateAndWriteCostModel (stub calibrators are rejected)",
    "config_path": "${CONFIG}",
}
path.write_text(json.dumps(payload, indent=2))
print(path.read_text())
sys.exit(1)
PY
fi

BUILD_LOG=""
if ! ensure_cmake_target "${ROOT}" "${BUILD}" "${TARGET}"; then
	BUILD_LOG="$(cmake --build "${BUILD}" --target "${TARGET}" -j"$(nproc)" 2>&1 | tail -40 || true)"
fi

if [[ ! -x "${BUILD}/${TARGET}" ]]; then
	python3 - <<PY
import json, pathlib, sys
path = pathlib.Path("${OUT}/aux_cost_model_calibrate.json")
payload = {
    "gate": "aux_cost_model_calibrate",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "fail",
    "reason": "calibrate binary missing after cmake configure/build",
    "log_tail": """${BUILD_LOG}"""[-4000:],
}
path.write_text(json.dumps(payload, indent=2))
print(path.read_text())
sys.exit(1)
PY
fi

export ACCEL_MULTI_CONN_TEST_QUERIES="${ACCEL_MULTI_CONN_TEST_QUERIES:-${QUERY}}"
export ACCEL_INTERFERENCE_CALIB_SEC="${ACCEL_INTERFERENCE_CALIB_SEC:-4}"
export ACCEL_INTERFERENCE_ISOLATED_SAMPLES="${ACCEL_INTERFERENCE_ISOLATED_SAMPLES:-20}"

CALIB_WORK_DIR="${BUILD}/q${QUERY}_aux_calibrate_${BENCHMARK_NAME}"
mkdir -p "${CALIB_WORK_DIR}"
CALIB_DB="${CALIB_WORK_DIR}/calibrate.duckdb"
cp "${DATASET_DB}" "${CALIB_DB}"

export ACCEL_AUX_BENCHMARK="${BENCHMARK_NAME}"
export "${DB_ENV_VAR}"="${CALIB_DB}"

set +e
LOG="$("${BUILD}/${TARGET}" "${CALIB_DB}" "${CONFIG}" 2>&1)"
RC=$?
set -e

python3 - <<PY
import json, pathlib, sys
path = pathlib.Path("${OUT}/aux_cost_model_calibrate.json")
config_path = pathlib.Path("${CONFIG}")
status = "pass"
reason = ""
interference = {}
if int("${RC}") != 0:
    status = "fail"
    reason = "calibrate binary failed"
elif not config_path.is_file():
    status = "fail"
    reason = "cost model json missing"
else:
    data = json.loads(config_path.read_text())
    interference = {k: data.get(k) for k in (
        "interference_delta_per_query_us",
        "interference_invalidate_per_query_us",
        "interference_rebuild_per_query_us",
        "interference_query_slowdown_factor",
    )}
    if float(data.get("interference_delta_per_query_us") or 0) <= 0:
        status = "fail"
        reason = "interference_delta_per_query_us must be > 0 after calibration"
    if float(data.get("interference_rebuild_per_query_us") or 0) <= 0:
        status = "fail"
        reason = reason or "interference_rebuild_per_query_us must be > 0 after calibration"

payload = {
    "gate": "aux_cost_model_calibrate",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": status,
    "exit_code": int("${RC}"),
    "config_path": str(config_path),
    "interference": interference,
    "reason": reason,
    "log_tail": """${LOG}"""[-4000:],
}
path.write_text(json.dumps(payload, indent=2))
print(path.read_text())
sys.exit(0 if status == "pass" else 1)
PY
