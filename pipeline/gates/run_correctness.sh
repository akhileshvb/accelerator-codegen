#!/usr/bin/env bash
# Base-query correctness: the differential binary, built and run through the engine host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_correctness.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/suite_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/correctness.json"
if [[ ! -d "${BUILD}" ]]; then
  emit_result "${JSON}" correctness skip "build dir missing" ""
  exit 0
fi
if [[ -z "${DIFF_BINARY}" ]]; then
  emit_result "${JSON}" correctness fail "no differential target in the gate context (engine_host.differential_binary). ${HOST_HINT}" "" "owner=pipeline"
fi

if ! engine_ready "${DIFF_BINARY}"; then
  emit_result "${JSON}" correctness fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "owner=${ENGINE_OWNER}" "query:=${QUERY}"
fi
# The differential calls this workload's own SQL entry (a base kernel's, not the aux one the
# templates name).
ACCEL_SQL_ENTRY="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('function_name') or '')" "${CTX}")"
export ACCEL_SQL_ENTRY
suite_prepare "${ACCEL_GATE_TIER:-full}" "${JSON}" correctness
engine_run "${DIFF_BINARY}" "${QUERY}"
suite_extras run_correctness.sh
if [[ ${RC} -ne 0 ]]; then
  emit_result "${JSON}" correctness fail "${DIFF_BINARY} failed for query ${QUERY}" "${ENGINE_LOG}${LOG}" "query:=${QUERY}" "exit_code:=${RC}" "${SUITE_EXTRAS[@]}"
fi
emit_result "${JSON}" correctness pass "" "${ENGINE_LOG}${LOG}" "query:=${QUERY}" "exit_code:=0" "${SUITE_EXTRAS[@]}"
