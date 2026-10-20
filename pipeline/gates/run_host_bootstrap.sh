#!/usr/bin/env bash
# Verify the query-host bootstrap: the engine host brings the instance up, loads the
# dataset and builds the differential target.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-0}"

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

ENGINE_JSON="${ROOT}/pipeline/artifacts/engines/${ENGINE_NAME}/host_bootstrap/host_bootstrap.json"
QUERY_JSON="${GATE_OUT}/host_bootstrap.json"

finish() { # status reason log
  local rc=0
  write_result "${ENGINE_JSON}" host_bootstrap "$@" "query:=${QUERY}" "differential_binary=${DIFF_BINARY}" >/dev/null || rc=$?
  write_result "${QUERY_JSON}" host_bootstrap "$@" "query:=${QUERY}" "differential_binary=${DIFF_BINARY}" || rc=$?
  exit "${rc}"
}

if [[ -z "${DIFF_BINARY}" ]]; then
  finish fail "no differential target in the gate context. ${HOST_HINT}" ""
fi
if ! engine_ready "${DIFF_BINARY}"; then
  finish fail "${ENGINE_FAIL}: create ${DIFF_SOURCE} + CMake and the host SQL entrypoints" "${ENGINE_LOG}"
fi
finish pass "${DIFF_BINARY} built" "${ENGINE_LOG}"
