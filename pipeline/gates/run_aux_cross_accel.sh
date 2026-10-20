#!/usr/bin/env bash
# Cross-accelerator interaction test, via the engine host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_cross_accel.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/aux_cross_accel.json"
if [[ -z "${CROSS_ACCEL_TARGET}" ]]; then
  emit_result "${JSON}" aux_cross_accel fail "no cross-accel target in the gate context (engine_host.cross_accel_target). ${HOST_HINT}" ""
fi

export ACCEL_AUX_BENCHMARK="${BENCHMARK_NAME}"
export ACCEL_CROSS_ACCEL_TEST_QUERIES="${QUERY}"

if ! engine_ready "${CROSS_ACCEL_TARGET}"; then
  emit_result "${JSON}" aux_cross_accel fail "${ENGINE_FAIL}" "${ENGINE_LOG}"
fi
engine_run "${CROSS_ACCEL_TARGET}" "${QUERY}"
STATUS=pass
[[ ${RC} -eq 0 ]] || STATUS=fail
emit_result "${JSON}" aux_cross_accel "${STATUS}" "" "${ENGINE_LOG}${LOG}" \
  "benchmark=${BENCHMARK_NAME}" "target=${CROSS_ACCEL_TARGET}" "exit_code:=${RC}"
