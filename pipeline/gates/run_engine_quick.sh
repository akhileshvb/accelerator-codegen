#!/usr/bin/env bash
# The inner loop of an engine_backend layer, in seconds: an incremental host.sh build of the layer's
# target, one run of its harness (no repeats), and the canary checks the layer owns when the backend
# has its canary binding. Not a gate of record: the layer gate and the canary gates still decide.
# ACCEL_QUICK_LAYER selects the layer; the result carries per-phase seconds.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ROOT

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_engine_gate_context "${ROOT}" 0

GATE=engine_quick
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/${GATE}.json}"
LAYER="${ACCEL_QUICK_LAYER:-}"
mapfile -t INFO < <(python3 - "${CTX}" "${LAYER}" <<'PY'
import json, sys
ctx, layer_id = json.loads(sys.argv[1]), sys.argv[2]
layer = next((l for l in ctx.get("implementation_layers") or [] if l.get("id") == layer_id), {})
print(layer.get("id") or "")
print(layer.get("cmake_target") or "")
print((ctx.get("target_paths") or {}).get("canary_test_target") or "test_%s_canary" % ctx["engine_name"])
PY
)
LAYER_ID="${INFO[0]}"; TARGET="${INFO[1]}"; CANARY="${INFO[2]}"
[[ -n "${LAYER_ID}" ]] || emit_result "${OUT_JSON}" "${GATE}" fail "unknown layer '${LAYER}'" ""
[[ -n "${TARGET}" ]] || emit_result "${OUT_JSON}" "${GATE}" fail "layer ${LAYER_ID} has no cmake target: run its gate with run_layer_gate" ""

TIMINGS="{}"
phase() { # name seconds
  TIMINGS="$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); d[sys.argv[2]]=round(float(sys.argv[3]),1); print(json.dumps(d))" "${TIMINGS}" "$1" "$2")"
}
timed() { # name cmd... ; sets T_RC
  local start=${EPOCHREALTIME}
  "${@:2}"; T_RC=$?
  phase "$1" "$(python3 -c "import sys; print(float(sys.argv[2]) - float(sys.argv[1]))" "${start}" "${EPOCHREALTIME}")"
}
finish() { # status reason log
  local status="$1"
  write_result "${OUT_JSON}" "${GATE}" "$@" "layer=${LAYER_ID}" "phase_seconds:=${TIMINGS}" >/dev/null
  echo "engine_quick ${LAYER_ID}: ${status} ${TIMINGS}"
  [[ "${status}" == pass ]] && exit 0 || exit 1
}

export TMPDIR="${TMPDIR:-/dev/shm}"
timed up engine_steps "up"
[[ ${T_RC} -eq 0 ]] || finish fail "${ENGINE_FAIL}" "${ENGINE_LOG}"
LOGS="${ENGINE_LOG}"
timed build engine_steps "build ${TARGET}"
LOGS="${LOGS}${ENGINE_LOG}"
[[ ${T_RC} -eq 0 ]] || finish fail "build of ${TARGET} failed: ${ENGINE_FAIL}" "${LOGS}"
ACCEL_GATE_RUN_TIMEOUT="${ACCEL_QUICK_RUN_TIMEOUT:-120}" timed harness engine_run "${TARGET}"
LOGS="${LOGS}${LOG}"
[[ ${RC} -eq 0 ]] || finish fail "${RUN_REASON:-harness ${TARGET} failed (exit ${RC})}" "${LOGS}"

if [[ -f "${ROOT}/backends/${ENGINE_NAME}/tests/${CANARY}.cpp" ]] && python3 - "${CTX}" "${LAYER_ID}" <<'PY'
import json, sys
sys.exit(0 if sys.argv[2] in {"capture", "scan", "runtime", "api"} else 1)
PY
then
  timed canary_build engine_steps "build ${CANARY}"
  LOGS="${LOGS}${ENGINE_LOG}"
  [[ ${T_RC} -eq 0 ]] || finish fail "build of ${CANARY} failed: ${ENGINE_FAIL}" "${LOGS}"
  ACCEL_GATE_RUN_TIMEOUT="${ACCEL_QUICK_RUN_TIMEOUT:-300}" timed canary engine_run "${CANARY}" --owner "${LAYER_ID}"
  LOGS="${LOGS}${LOG}"
  if [[ ${RC} -ne 0 ]]; then
    failing="$(grep '^{"check"' <<<"${LOG}" | grep -v '"status": "pass"' | head -3 | cut -c1-600)"
    finish fail "canary checks owned by ${LAYER_ID} failed" "${failing:-${LOG}}"
  fi
fi
finish pass "${LAYER_ID}: built, harness ran once${ENGINE_NAME:+, canary subset where bound}" "${LOGS}"
