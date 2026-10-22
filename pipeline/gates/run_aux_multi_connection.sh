#!/usr/bin/env bash
# Multi-connection stress: readers and a writer on separate connections, via the engine host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_multi_connection.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/aux_multi_connection.json"
if [[ -z "${MULTI_CONN_TARGET}" ]]; then
  emit_result "${JSON}" aux_multi_connection fail "no multi-connection target in the gate context (engine_host.multi_conn_target). ${HOST_HINT}" ""
fi

export ACCEL_AUX_BENCHMARK="${BENCHMARK_NAME}"
export ACCEL_MULTI_CONN_TEST_QUERIES="${QUERY}"
export ACCEL_MULTI_CONN_TARGET_READERS="${ACCEL_MULTI_CONN_TARGET_READERS:-3}"
export ACCEL_MULTI_CONN_BACKGROUND_READERS="${ACCEL_MULTI_CONN_BACKGROUND_READERS:-2}"
export ACCEL_MULTI_CONN_STRESS_SEC="${ACCEL_MULTI_CONN_STRESS_SEC:-5}"

if ! engine_ready "${MULTI_CONN_TARGET}"; then
  emit_result "${JSON}" aux_multi_connection fail "${ENGINE_FAIL}" "${ENGINE_LOG}"
fi
engine_run "${MULTI_CONN_TARGET}" "${QUERY}"
STATUS=pass
[[ ${RC} -eq 0 ]] || STATUS=fail
emit_result "${JSON}" aux_multi_connection "${STATUS}" "" "${ENGINE_LOG}${LOG}" \
  "benchmark=${BENCHMARK_NAME}" "target=${MULTI_CONN_TARGET}" "exit_code:=${RC}" \
  "target_readers:=${ACCEL_MULTI_CONN_TARGET_READERS}" \
  "background_readers:=${ACCEL_MULTI_CONN_BACKGROUND_READERS}" \
  "stress_sec:=${ACCEL_MULTI_CONN_STRESS_SEC}"
