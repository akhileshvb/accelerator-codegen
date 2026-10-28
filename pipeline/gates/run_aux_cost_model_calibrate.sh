#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
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
  SKIP_REASON="${reason}" python3 - <<PY
import json, os, pathlib
path = pathlib.Path("${OUT}/aux_cost_model_calibrate.json")
payload = {
    "gate": "aux_cost_model_calibrate",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "skip",
    "reason": os.environ["SKIP_REASON"],
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

TARGET="${AUX_CALIBRATE_TARGET}"
if [[ -z "${TARGET}" ]]; then
  emit_result "${OUT}/aux_cost_model_calibrate.json" aux_cost_model_calibrate fail "no calibrate target in the gate context" "" "owner=pipeline"
fi
if ! engine_ready "${TARGET}"; then
  engine_not_ready "${OUT}/aux_cost_model_calibrate.json" aux_cost_model_calibrate
fi

export ACCEL_MULTI_CONN_TEST_QUERIES="${ACCEL_MULTI_CONN_TEST_QUERIES:-${QUERY}}"
export ACCEL_INTERFERENCE_CALIB_SEC="${ACCEL_INTERFERENCE_CALIB_SEC:-4}"
export ACCEL_INTERFERENCE_ISOLATED_SAMPLES="${ACCEL_INTERFERENCE_ISOLATED_SAMPLES:-20}"
export ACCEL_AUX_BENCHMARK="${BENCHMARK_NAME}"

# written beside the config and moved into place when done: a bench running at the same time (aux_exit) never reads a half-written file
mkdir -p "$(dirname "${CONFIG}")"  # the gate names the path; the query's config dir may not exist yet
engine_measure "${TARGET}" --query "${QUERY}" "${CONFIG}.new"
[[ ${RC} -ne 0 || ! -f "${CONFIG}.new" ]] || mv -f "${CONFIG}.new" "${CONFIG}"
RC_RUN=${RC}
BUILD_LOG="${ENGINE_LOG}"

# logs and reasons go through the environment: pasted into the heredoc, a quote in them broke the Python
GATE_LOG="${BUILD_LOG}${LOG}" python3 - <<PY
import json, os, pathlib, sys
path = pathlib.Path("${OUT}/aux_cost_model_calibrate.json")
config_path = pathlib.Path("${CONFIG}")
status = "pass"
reason = ""
interference = {}
if int("${RC_RUN}") != 0:
    status = "fail"
    last = next((l for l in reversed(os.environ["GATE_LOG"].splitlines()) if l.strip()), "")
    reason = f"calibrate binary failed (exit ${RC_RUN})" + (f": {last.strip()}" if last else "")
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
    "exit_code": int("${RC_RUN}"),
    "config_path": str(config_path),
    "interference": interference,
    "reason": reason,
    "log_tail": os.environ["GATE_LOG"][-4000:],
}
path.write_text(json.dumps(payload, indent=2))
print(path.read_text())
sys.exit(0 if status == "pass" else 1)
PY
