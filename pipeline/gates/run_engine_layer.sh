#!/usr/bin/env bash
# Build + run one engine_backend layer harness through the engine's host.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ROOT
GATE="${ACCEL_GATE_NAME:-engine_layer}"
JSON="${ACCEL_GATE_OUT:-/tmp/engine_layer.json}"

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_engine_gate_context "${ROOT}" 0

# The gate's layer: id, harness source, cmake target, skip condition (one per line).
mapfile -t LAYER < <(python3 - "${GATE}" "${CTX}" <<'PY'
import json, sys
gate, ctx = sys.argv[1], json.loads(sys.argv[2])
layers = list(ctx.get("implementation_layers") or [])
layer = next((l for l in layers if l.get("gate") == gate), None)
if layer is None and gate.startswith("engine_"):
    layer = next((l for l in layers if l.get("id") == gate[len("engine_"):]), None)
layer = layer or {}
skip = layer.get("skip_unless") == "requires_in_process_host" and not ctx.get("requires_in_process_host", False)
print(layer.get("id") or "")
print(layer.get("harness") or "")
print(layer.get("cmake_target") or "")
print("skip" if skip else "")
PY
)
LAYER_ID="${LAYER[0]}"; HARNESS="${LAYER[1]}"; TARGET="${LAYER[2]}"; SKIP="${LAYER[3]}"

[[ -n "${LAYER_ID}" ]] || emit_result "${JSON}" "${GATE}" fail "unknown layer gate: ${GATE}" ""
[[ -z "${SKIP}" ]] || emit_result "${JSON}" "${GATE}" skip "host layer skipped (requires_in_process_host=false)" "" "layer=${LAYER_ID}"
if [[ -z "${HARNESS}" || ! -f "${ROOT}/${HARNESS}" ]]; then
  emit_result "${JSON}" "${GATE}" fail "missing layer harness: ${HARNESS:-(unset)}" "" "layer=${LAYER_ID}"
fi
if ! engine_steps "up" "build ${TARGET}"; then
  emit_result "${JSON}" "${GATE}" fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "layer=${LAYER_ID}" "cmake_target=${TARGET}"
fi
# A race shows up only sometimes: the harness runs up to ACCEL_LAYER_GATE_RUNS times (while the
# repeats fit in ACCEL_LAYER_GATE_REPEAT_BUDGET seconds) and every run must pass.
runs="${ACCEL_LAYER_GATE_RUNS:-10}" budget="${ACCEL_LAYER_GATE_REPEAT_BUDGET:-120}"
started=${SECONDS} run=0
while :; do
  run=$((run + 1))
  ACCEL_GATE_RUN_TIMEOUT="${ACCEL_GATE_RUN_TIMEOUT:-300}" engine_run "${TARGET}"   # a toy-module harness runs in seconds
  [[ ${RC} -eq 0 ]] || break
  (( run < runs && SECONDS - started < budget )) || break
done
if [[ ${RC} -ne 0 && ${run} -gt 1 && -z "${RUN_REASON}" ]]; then
  RUN_REASON="${TARGET} failed on run ${run} after passing $((run - 1)) times: a race (the harness or the backend depends on timing)"
fi
STATUS=pass
[[ ${RC} -eq 0 ]] || STATUS=fail
emit_result "${JSON}" "${GATE}" "${STATUS}" "${RUN_REASON}" "${ENGINE_LOG}${LOG}" \
  "layer=${LAYER_ID}" "harness=${HARNESS}" "cmake_target=${TARGET}" "returncode:=${RC}" "runs:=${run}"
